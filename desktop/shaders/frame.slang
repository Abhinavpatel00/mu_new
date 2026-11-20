/* shaders/frame.slang — everything after the scene pass.
 * ============================================================================
 *    bloom      13-tap down (Jimenez) to 1/2, 1/4, 1/8, 1/16; 9-tap tent back
 *               up, each level added onto the next with BLEND_ADD.
 *    composite  ink lines from the resolved normal/depth/id targets, bloom,
 *               tone map, grade, paper, sRGB, dither — one fullscreen triangle
 *               straight into the frame image.
 *
 *  Why the ink is anti-aliased: the scene targets are MSAA-resolved by average,
 *  so a pixel on a silhouette holds depth = mix(near, far, coverage). The depth
 *  detector recovers that coverage as (far - depth) / (far - near) and uses it
 *  as the ink amount. One full pixel inside the edge plus the covered share of
 *  the edge pixel: a line with a smooth outer contour and no stair steps.
 * ==========================================================================*/
#include "shared.slang"
#include "../external/mu_lib/mu/shaders/post.slang"
#include "../external/mu_lib/mu/shaders/format.slang"

struct FramePush
{
    float4 texel; /* xy 1/size of the image being read, zw 1/size of the frame         */
    float4 ink;   /* rgb ink colour, a opacity (0 = no lines)                          */
    float4 edges; /* x tap radius in pixels, y depth, z normal, w id thresholds        */
    float4 grade; /* x exposure, y bloom strength, z saturation, w vignette radius     */
    float4 paper; /* x grain strength, y line wobble in pixels, z time, w bloom knee   */
    uint   color_map;
    uint   aux_map;
    uint   id_map;
    uint   bloom_map;
    uint   paper_map;
    uint   linear_sampler;
    uint   mirror_sampler;
    float  bloom_threshold;
};
[[vk::push_constant]] FramePush push;

[shader("vertex")]
ScreenVaryings vs_screen(uint vertex_id : SV_VertexID)
{
    return screen_triangle(vertex_id, 0.0);
}

float3 tap(float2 uv, float2 offset)
{
    return g_textures[NonUniformResourceIndex(push.color_map)].SampleLevel(g_samplers[NonUniformResourceIndex(push.linear_sampler)], uv + offset * push.texel.xy, 0.0).rgb;
}

/* The 13 taps mu_bloom_downsample13 expects, in its a..m order. */
float3 downsample13(float2 uv, bool prefilter)
{
    float3 t[13];
    t[0]  = tap(uv, float2(-2, -2)); t[1]  = tap(uv, float2(0, -2)); t[2]  = tap(uv, float2(2, -2));
    t[3]  = tap(uv, float2(-2, 0));  t[4]  = tap(uv, float2(0, 0));  t[5]  = tap(uv, float2(2, 0));
    t[6]  = tap(uv, float2(-2, 2));  t[7]  = tap(uv, float2(0, 2));  t[8]  = tap(uv, float2(2, 2));
    t[9]  = tap(uv, float2(-1, -1)); t[10] = tap(uv, float2(1, -1)); t[11] = tap(uv, float2(-1, 1)); t[12] = tap(uv, float2(1, 1));
    /* first level only: keep what is brighter than paper white, and stop one
     * hot pixel from flickering through the whole chain (Karis) */
    [ForceUnroll]
    for (int k = 0; k < 13; ++k)
        t[k] = select(prefilter, mu_karis_weight(mu_bloom_threshold(t[k], push.bloom_threshold, push.paper.w)), t[k]);
    return mu_bloom_downsample13(t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12]);
}

[shader("fragment")]
float4 fs_bloom_prefilter(ScreenVaryings i) : SV_Target0
{
    return float4(downsample13(i.uv, true), 1.0);
}

[shader("fragment")]
float4 fs_bloom_down(ScreenVaryings i) : SV_Target0
{
    return float4(downsample13(i.uv, false), 1.0);
}

/* Drawn with BLEND_ADD onto the next larger level. */
[shader("fragment")]
float4 fs_bloom_up(ScreenVaryings i) : SV_Target0
{
    return float4(mu_bloom_upsample9(tap(i.uv, float2(-1, -1)), tap(i.uv, float2(0, -1)), tap(i.uv, float2(1, -1)),
                                     tap(i.uv, float2(-1, 0)),  tap(i.uv, float2(0, 0)),  tap(i.uv, float2(1, 0)),
                                     tap(i.uv, float2(-1, 1)),  tap(i.uv, float2(0, 1)),  tap(i.uv, float2(1, 1))), 1.0);
}

/* --------------------------------------------------------------- ink lines */
float ink_amount(float2 uv)
{
    Texture2D    aux_map = g_textures[NonUniformResourceIndex(push.aux_map)];
    Texture2D    id_map  = g_textures[NonUniformResourceIndex(push.id_map)];
    SamplerState linear  = g_samplers[NonUniformResourceIndex(push.linear_sampler)];
    float2       reach   = push.texel.zw * push.edges.x;
    /* centre and a diagonal cross: two opposite pairs */
    float4 aux[5];
    float4 id[5];
    aux[0] = aux_map.SampleLevel(linear, uv, 0.0);
    aux[1] = aux_map.SampleLevel(linear, uv + float2(-reach.x, -reach.y), 0.0);
    aux[2] = aux_map.SampleLevel(linear, uv + float2(reach.x, reach.y), 0.0);
    aux[3] = aux_map.SampleLevel(linear, uv + float2(-reach.x, reach.y), 0.0);
    aux[4] = aux_map.SampleLevel(linear, uv + float2(reach.x, -reach.y), 0.0);
    id[0]  = id_map.SampleLevel(linear, uv, 0.0);
    id[1]  = id_map.SampleLevel(linear, uv + float2(-reach.x, -reach.y), 0.0);
    id[2]  = id_map.SampleLevel(linear, uv + float2(reach.x, reach.y), 0.0);
    id[3]  = id_map.SampleLevel(linear, uv + float2(-reach.x, reach.y), 0.0);
    id[4]  = id_map.SampleLevel(linear, uv + float2(reach.x, -reach.y), 0.0);

    float depth    = aux[0].w;
    float nearest  = depth, farthest = depth;
    float weight   = id[0].a; /* the ink weight of whoever is in front owns the line */
    [ForceUnroll]
    for (int k = 1; k < 5; ++k)
    {
        weight   = select(aux[k].w < nearest, id[k].a, weight);
        nearest  = min(nearest, aux[k].w);
        farthest = max(farthest, aux[k].w);
    }

    /* Depth: 1/z is linear across a flat surface at any slant, so the bend of
     * 1/z along each diagonal is zero on a plane and large on a silhouette.
     * No slope threshold to tune, no false lines on floors seen edge-on. */
    float bend_a = abs(1.0 / aux[1].w + 1.0 / aux[2].w - 2.0 / depth) * depth;
    float bend_b = abs(1.0 / aux[3].w + 1.0 / aux[4].w - 2.0 / depth) * depth;
    float gap    = smoothstep(push.edges.y, push.edges.y * 2.5, max(bend_a, bend_b));
    /* how much of this pixel the nearer surface covers */
    float cover  = mu_saturate((farthest - depth) / max(farthest - nearest, 1e-4));
    float depth_line = gap * cover;

    /* Normals: creases. The sky has no normal and is left to the depth test. */
    float3 n1 = aux[1].xyz, n2 = aux[2].xyz, n3 = aux[3].xyz, n4 = aux[4].xyz;
    float  solid = step(0.5, min(min(dot(n1, n1), dot(n2, n2)), min(dot(n3, n3), dot(n4, n4))));
    float  turn  = max(1.0 - dot(mu_safe_normalize(n1), mu_safe_normalize(n2)), 1.0 - dot(mu_safe_normalize(n3), mu_safe_normalize(n4)));
    float  normal_line = smoothstep(push.edges.z, push.edges.z * 2.5, turn) * solid;

    /* Ids: two objects that touch with no depth gap and no crease. Where there
     * is a depth gap, the depth line's coverage already says who gets ink. */
    float  differ  = max(length(id[1].rgb - id[2].rgb), length(id[3].rgb - id[4].rgb));
    float  id_line = smoothstep(push.edges.w, push.edges.w * 3.0, differ) * lerp(1.0, cover, gap);

    /* distant lines thin out instead of turning the horizon black */
    float fade = mu_saturate(1.35 - nearest * 0.012);
    return mu_saturate(max(max(depth_line, normal_line), id_line)) * weight * fade;
}

[shader("fragment")]
float4 fs_composite(ScreenVaryings i) : SV_Target0
{
    SamplerState linear = g_samplers[NonUniformResourceIndex(push.linear_sampler)];
    SamplerState mirror = g_samplers[NonUniformResourceIndex(push.mirror_sampler)];
    /* the baked paper, one texel per two pixels, mirrored so it never seams */
    float4 paper = g_textures[NonUniformResourceIndex(push.paper_map)].SampleLevel(mirror, i.clip.xy / 512.0, 0.0);
    /* a hand does not draw a line where the ruler says: the detector looks
     * through the paper's low-frequency channels, a pixel or so off */
    float2 wobble = (paper.rg - 0.5) * push.paper.y * push.texel.zw;
    float3 color  = g_textures[NonUniformResourceIndex(push.color_map)].SampleLevel(linear, i.uv, 0.0).rgb;
    float3 bloom  = g_textures[NonUniformResourceIndex(push.bloom_map)].SampleLevel(linear, i.uv, 0.0).rgb;
    float  ink    = ink_amount(i.uv + wobble) * push.ink.a;

    /* ink is tinted by what it covers, never flat black */
    color = lerp(color, push.ink.rgb + color * 0.10, ink);
    color = color + bloom * push.grade.y;

    /* Khronos neutral keeps authored toon colours exactly and only rolls off
     * what is brighter than white: emissives and the sun */
    color = mu_tonemap_neutral(color * push.grade.x);
    color = mu_saturation(color, push.grade.z);
    color = mu_vignette(color, i.uv, push.grade.w, 0.55);
    /* paper: pigment pools in the tooth and thins along the fibres, and the
     * blue of the wash goes first */
    color *= 1.0 + ((paper.b - 0.5) + (paper.a - 0.5) * 0.35) * push.paper.x * float3(1.0, 1.04, 1.12);

    /* the frame image is UNORM: this is the transfer function of record */
    color = mu_linear_to_srgb(color);
    color = mu_dither_ign(color, i.clip.xy, 1.0 / 255.0);
    return float4(color, 1.0);
}
