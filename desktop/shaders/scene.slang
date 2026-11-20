/* shaders/scene.slang — toon geometry: scene pass, shadow depth, petals.
 * ============================================================================
 *  The scene pass writes three targets, all 4x MSAA and resolved by average:
 *    0  HDR colour
 *    1  world normal (xyz) + view depth (w)   -> ink lines in frame.slang
 *    2  outline id colour (rgb) + ink weight  -> ink lines in frame.slang
 *  Averaging is what anti-aliases the ink: an edge pixel holds a blend of the
 *  two surfaces, and the line detector turns that blend into coverage.
 *
 *  Cel bands are cut with screen-space derivatives (fwidth), so a band edge is
 *  one pixel wide at any distance instead of a fixed softness that blurs up
 *  close and aliases far away.
 * ==========================================================================*/
#include "shared.slang"
#include "../external/mu_lib/mu/shaders/toon.slang"
#include "../external/mu_lib/mu/shaders/shadows.slang"
#include "../external/mu_lib/mu/shaders/noise.slang"

struct ScenePush
{
    float4x4  view_proj;
    float4x4  light_view_proj;
    float4    camera;    /* xyz eye, w time in seconds                             */
    float4    sun;       /* xyz direction towards the sun, w shadow texel in world */
    float4    sun_color; /* rgb key light, w 1 / shadow map size                   */
    float4    sky_color; /* rgb ambient from above, w hatch strength               */
    Instance *instances; /* device address: first instance of this draw            */
    uint      vertex_offset; /* binding 3: mesh vertices, or petal records         */
    uint      index_offset;  /* binding 3: mesh indices                            */
    uint      shadow_map;
    uint      shadow_sampler;
};
[[vk::push_constant]] ScenePush push;

struct SceneVaryings
{
    float4 clip : SV_Position;
    float3 world : POSITION0;
    float3 local : POSITION1;
    float3 normal : NORMAL0;
    float2 uv : TEXCOORD0;
    float  depth : DEPTH0; /* view depth; SV_Position.w means different things per API */
    nointerpolation uint instance : INSTANCE0;
};

struct SceneTargets
{
    float4 color : SV_Target0;
    float4 aux : SV_Target1;
    float4 id : SV_Target2;
};

/* ---------------------------------------------------------------- vertices */
/* Index pulled by SV_VertexID, vertex pulled by index: the whole of "vertex
 * input" is two loads from the device arena. */
Vertex fetch_vertex(uint vertex_id)
{
    uint index = g_data.Load<uint>(push.index_offset + vertex_id * 4u);
    return g_data.Load<Vertex>(push.vertex_offset + index * 32u);
}

/* Wind lives in one place so the shadow pass bends exactly like the scene. */
float3 wind_offset(float3 world, float3 local, Instance inst)
{
    float t     = push.camera.w;
    float phase = inst.extra.z;
    float3 sway = float3(sin(t * 1.3 + phase + world.y * 0.9),
                         sin(t * 2.1 + phase * 1.7 + world.x * 1.3) * 0.35,
                         cos(t * 1.1 + phase + world.z * 0.8));
    /* only what is above the object's origin moves: trunks stay planted */
    return sway * inst.extra.y * mu_saturate(local.y * 0.8 + 0.4);
}

[shader("vertex")]
SceneVaryings vs_scene(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    Vertex   v    = fetch_vertex(vertex_id);
    Instance inst = push.instances[instance_id];
    /* Cofactor normal transform: exact under the non-uniform scales that
     * squash-and-stretch produces, and no inverse. Columns of the 3x3 are the
     * rows of its transpose. */
    float3x3 cols = transpose((float3x3)inst.model);
    SceneVaryings o;

    o.world     = mul(inst.model, float4(v.p, 1.0)).xyz;
    o.world    += wind_offset(o.world, v.p, inst);
    o.local     = v.p;
    o.normal    = cross(cols[1], cols[2]) * v.n.x + cross(cols[2], cols[0]) * v.n.y + cross(cols[0], cols[1]) * v.n.z;
    o.uv        = v.uv;
    o.clip      = mul(push.view_proj, float4(o.world, 1.0));
    o.depth     = o.clip.w;
    o.instance  = instance_id;
    return o;
}

/* Depth only, no fragment shader. */
[shader("vertex")]
float4 vs_shadow(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) : SV_Position
{
    Vertex   v     = fetch_vertex(vertex_id);
    Instance inst  = push.instances[instance_id];
    float3   world = mul(inst.model, float4(v.p, 1.0)).xyz;

    world += wind_offset(world, v.p, inst);
    return mul(push.light_view_proj, float4(world, 1.0));
}

/* ------------------------------------------------------------------ light */
/* A band edge exactly one pixel wide: the threshold is smoothed over the
 * screen-space change of the value it cuts. */
float band(float value, float threshold)
{
    float width = clamp(fwidth(value), 0.002, 0.12);
    return smoothstep(threshold - width, threshold + width, value);
}

/* 16-tap box PCF in four gathers, the layout of mu_pcf_gather_box, written
 * against the bindless Texture2D array (a depth image is read through its red
 * channel). The shadow map is standard depth, 0 nearest the light, so the sign
 * handed to mu_shadow_compare4 is +1. */
float sun_visibility(float3 world, float3 normal)
{
    Texture2D    map     = g_textures[NonUniformResourceIndex(push.shadow_map)];
    SamplerState sampler = g_samplers[NonUniformResourceIndex(push.shadow_sampler)];
    float3       p       = mu_normal_offset(world, normal, push.sun.xyz, push.sun.w, 2.5);
    float4       clip    = mul(push.light_view_proj, float4(p, 1.0));
    float2       uv      = mu_mad(clip.xy, MU_NDC_TO_UV, float2(0.5));
    float        ref     = clip.z - 0.0015;
    float        o       = push.sun_color.w;
    float4       lit     = mu_shadow_compare4(map.GatherRed(sampler, uv + float2(-o, -o)), ref, 1.0)
                         + mu_shadow_compare4(map.GatherRed(sampler, uv + float2(o, -o)), ref, 1.0)
                         + mu_shadow_compare4(map.GatherRed(sampler, uv + float2(-o, o)), ref, 1.0)
                         + mu_shadow_compare4(map.GatherRed(sampler, uv + float2(o, o)), ref, 1.0);
    /* outside the map nothing is shadowed */
    return lerp(1.0, (lit.x + lit.y + lit.z + lit.w) * (1.0 / 16.0), mu_shadow_in_bounds(uv, clip.z));
}

/* Anti-aliased screen-space hatch: one family of diagonal ink lines. */
float hatch_lines(float2 pixel, float period, float thickness)
{
    float v = abs(frac(dot(pixel, float2(0.70710678, 0.70710678)) / period) - 0.5) * 2.0; /* 0 on a line centre */
    float w = 1.5 / period;                                                               /* one pixel, in v units */
    return 1.0 - smoothstep(thickness - w, thickness + w, v);
}

[shader("fragment")]
SceneTargets fs_scene(SceneVaryings i, bool front : SV_IsFrontFace)
{
    Instance inst   = push.instances[i.instance];
    uint     kind   = uint(inst.base.a + 0.5);
    float    time   = push.camera.w;
    float3   L      = push.sun.xyz;
    float3   V      = normalize(push.camera.xyz - i.world);
    float3   N      = normalize(i.normal) * select(front, 1.0, -1.0);
    float3   albedo = inst.base.rgb;
    float3   tint   = inst.shade.rgb;
    float    spec_strength = inst.surface.x;
    float    rim_strength  = inst.surface.z;
    float    shadow_bias   = 0.0; /* moves the terminator: > 0 grows the lit side */
    float3   glow   = float3(0.0);
    SceneTargets o;

    /* ---- what the material paints ---------------------------------------- */
    if (kind == KIND_TERRAIN)
    {
        /* Grass where the surface faces up, with a wobbly lip that hangs over
         * the rock; two-tone patches on the grass, strata on the rock. */
        float  lip    = mu_value_noise2(i.world.xz * 1.7) * 0.9 + mu_value_noise2(i.world.xz * 5.1) * 0.35;
        float  grass  = band(N.y + lip * 0.35 + i.local.y * 0.9, 0.62);
        float  patch  = band(mu_fbm4(i.world.xz * 0.45 + 3.0, 2.0, 0.5), 0.06); /* mu noise is signed: 0 is the middle */
        float3 green  = lerp(albedo, albedo * float3(1.22, 1.14, 0.62), patch * 0.55);
        float  layer  = i.world.y * 1.6 + mu_value_noise2(i.world.xz * 0.8 + i.world.y) * 0.9;
        float  strata = band(abs(frac(layer) - 0.5) * 2.0, 0.55);
        float3 rock   = inst.shade.rgb * lerp(0.78, 1.0, strata);
        /* the island narrows to a darker root */
        rock   = lerp(rock * float3(0.62, 0.55, 0.70), rock, mu_saturate(i.local.y * 0.22 + 1.05));
        albedo = lerp(rock, green, grass);
        tint   = lerp(float3(0.50, 0.42, 0.62), float3(0.42, 0.56, 0.62), grass);
    }
    else if (kind == KIND_WATER)
    {
        /* Flat toon water: drifting foam cells, a bright shore ring, and a
         * darker middle. Local xz is the disk's own radius. */
        float  radius = length(i.local.xz);
        float2 flow   = i.world.xz * 1.6 + float2(time * 0.12, time * 0.07);
        float2 cells  = mu_worley2_f1f2(flow + mu_value_noise2(flow * 0.7) * 0.6, 0.9);
        float  foam   = 1.0 - band(mu_worley_edge(cells), 0.085);
        float  shore  = band(radius + mu_value_noise2(i.world.xz * 2.3 + time * 0.2) * 0.07, 0.86);
        float  ripple = band(sin(radius * 16.0 - time * 1.4) * 0.5 + 0.5, 0.93) * band(radius, 0.35);
        albedo = lerp(albedo * float3(0.62, 0.80, 1.02), albedo, band(radius, 0.42));
        albedo = lerp(albedo, float3(0.96, 1.0, 1.0), mu_saturate(foam * 0.55 + shore + ripple * 0.5));
        N      = normalize(N + float3(cells.x - 0.5, 0.0, cells.y - 0.5) * 0.08);
        spec_strength *= 1.0 - shore;
    }
    else if (kind == KIND_FOLIAGE)
    {
        /* Leaf clumps: a second, sunnier green in blobs that follow the canopy. */
        float clump = band(mu_value_noise3(i.local * 3.2 + inst.extra.z), 0.12);
        albedo      = lerp(albedo, albedo * float3(1.20, 1.16, 0.80) + float3(0.02, 0.03, 0.0), clump * 0.6);
        shadow_bias = 0.10; /* light wraps into a canopy */
    }
    else if (kind == KIND_CLOUD)
    {
        shadow_bias = 0.28;
    }
    else if (kind == KIND_GLOW)
    {
        glow = albedo * inst.surface.w * (0.85 + 0.15 * sin(time * 3.0 + inst.extra.z));
    }

    /* ---- three tones ------------------------------------------------------- */
    /* One scalar carries both N.L and the sun's visibility, so a cast shadow
     * and the terminator are the same edge, cut by the same band. */
    float ndl      = dot(N, L);
    float visible  = kind == KIND_GLOW ? 1.0 : sun_visibility(i.world, N);
    float light    = min(ndl + shadow_bias, visible * 2.0 - 1.0);
    float lit      = band(light, 0.06);
    float bright   = band(light, 0.66) * 0.55;

    /* shadows keep their form: cooler and darker towards the ground */
    float3 shadow_col = albedo * tint * lerp(0.82, 1.08, N.y * 0.5 + 0.5);
    float3 lit_col    = albedo * push.sun_color.rgb;
    float3 color      = lerp(shadow_col, lit_col, lit);
    color             = lerp(color, lit_col * 1.16 + push.sun_color.rgb * 0.035, bright);

    /* ink hatching in the shadow tone, for materials that ask for it */
    float hatch = hatch_lines(i.clip.xy, 5.0, 0.30) * (1.0 - lit) * inst.shade.a * push.sky_color.w;
    color      *= 1.0 - hatch * 0.24;

    /* hard highlight: a banded Blinn lobe, only where the sun reaches */
    float3 H    = normalize(L + V);
    float  lobe = mu_pow(mu_saturate(dot(N, H)), max(inst.surface.y, 1.0));
    color      += push.sun_color.rgb * band(lobe, 0.5) * spec_strength * lit;

    /* sun rim on the lit side, sky rim on the dark side: both are banded */
    float fresnel = 1.0 - mu_saturate(dot(N, V));
    color += push.sun_color.rgb * albedo * band(mu_rim_light(N, V, L, 5.0, 0.22), 0.5) * rim_strength * 0.55;
    color += push.sky_color.rgb * albedo * band(mu_pow4(fresnel), 0.42) * (1.0 - lit) * 0.38;

    color = lerp(color, glow, kind == KIND_GLOW ? 1.0 : 0.0);

    o.color = float4(color, 1.0);
    o.aux   = float4(N, i.depth);
    o.id    = float4(mu_hash03(uint3(uint(inst.extra.x), 17u, 101u)), inst.extra.w);
    return o;
}

/* ----------------------------------------------------------------- petals */
/* One petal is 16 bytes in the device arena, simulated by cs_petals in compute.slang:
 * position and a seed. The quad is built from SV_VertexID. */
struct PetalVaryings
{
    float4 clip : SV_Position;
    float2 quad : TEXCOORD0;
    float3 color : COLOR0;
};

[shader("vertex")]
PetalVaryings vs_petal(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    static const float2 CORNERS[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
    float4 petal  = g_data.Load<float4>(push.vertex_offset + instance_id * 16u);
    float  seed   = petal.w;
    float  time   = push.camera.w;
    float2 corner = CORNERS[vertex_id];
    /* a tumbling frame: two axes that spin at seed-dependent rates */
    float2 spin   = mu_sincos(time * (1.5 + seed * 2.0) + seed * 40.0);
    float2 roll   = mu_sincos(time * (0.9 + seed) + seed * 17.0);
    float3 axis_u = float3(spin.y, spin.x * roll.x, spin.x * roll.y);
    float3 axis_v = normalize(cross(axis_u, float3(roll.y, 0.3, -roll.x)));
    float3 world  = petal.xyz + (axis_u * corner.x * 0.075 + axis_v * corner.y * 0.05);
    float3 normal = normalize(cross(axis_u, axis_v));
    PetalVaryings o;

    o.clip  = mul(push.view_proj, float4(world, 1.0));
    o.quad  = corner;
    /* two tones from the petal's own facing, like everything else here */
    o.color = lerp(float3(0.86, 0.42, 0.56), float3(1.0, 0.80, 0.86), step(0.0, abs(dot(normal, push.sun.xyz)) - 0.35)) * push.sun_color.rgb;
    return o;
}

[shader("fragment")]
float4 fs_petal(PetalVaryings i) : SV_Target0
{
    /* a petal outline cut from the quad: an ellipse pinched at one end */
    float shape = length(i.quad * float2(1.0, 1.25 - 0.35 * i.quad.x));
    if (shape > 1.0)
        discard;
    return float4(i.color, 1.0);
}
