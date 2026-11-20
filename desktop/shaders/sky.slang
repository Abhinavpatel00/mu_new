/* shaders/sky.slang — the sky behind the scene: gradient, sun, cloud sea.
 * ============================================================================
 *  Drawn as one fullscreen triangle at the far plane with the depth test left
 *  on (reverse-Z: far is 0), so only the pixels no geometry covered pay for it.
 *  It writes the same three targets as the scene, with a far depth and ink
 *  weight 0: silhouettes against the sky get a line, the sky itself gets none.
 * ==========================================================================*/
#include "shared.slang"
#include "../external/mu_lib/mu/shaders/noise.slang"

struct SkyPush
{
    float4x4 inverse_view_proj;
    float4   camera;    /* xyz eye, w time                 */
    float4   sun;       /* xyz direction towards the sun   */
    float4   sun_color; /* rgb                             */
    float4   zenith;    /* rgb straight up                 */
    float4   horizon;   /* rgb at the horizon              */
    float4   haze;      /* rgb below the horizon, w cloud sea height */
};
[[vk::push_constant]] SkyPush push;

struct SkyVaryings
{
    float4 clip : SV_Position;
    float3 ray : TEXCOORD0; /* unnormalised: linear across the triangle */
};

struct SceneTargets
{
    float4 color : SV_Target0;
    float4 aux : SV_Target1;
    float4 id : SV_Target2;
};

[shader("vertex")]
SkyVaryings vs_sky(uint vertex_id : SV_VertexID)
{
    ScreenVaryings s = screen_triangle(vertex_id, 0.0);
    /* a point on the near plane (reverse-Z: ndc z = 1) under this pixel */
    float4 near = mul(push.inverse_view_proj, float4(s.clip.xy, 1.0, 1.0));
    SkyVaryings o;

    o.clip = s.clip;
    o.ray  = near.xyz / near.w - push.camera.xyz;
    return o;
}

/* A soft-edged band, one pixel wide. */
float band(float value, float threshold)
{
    float width = clamp(fwidth(value), 0.002, 0.2);
    return smoothstep(threshold - width, threshold + width, value);
}

[shader("fragment")]
SceneTargets fs_sky(SkyVaryings i)
{
    float3 dir   = normalize(i.ray);
    float  time  = push.camera.w;
    float  up    = dir.y;
    float  sun   = dot(dir, push.sun.xyz);
    SceneTargets o;

    /* gradient: horizon glow rising into the zenith */
    float3 color = lerp(push.horizon.rgb, push.zenith.rgb, mu_smooth01(mu_saturate(up * 2.6)));
    /* the whole horizon is brighter towards the sun */
    color += push.sun_color.rgb * mu_pow4(mu_saturate(sun * 0.5 + 0.5)) * mu_pow2(1.0 - abs(up)) * 0.25;

    /* toon sun: a disk and two stepped halos */
    color = lerp(color, color + push.sun_color.rgb * 0.10, band(sun, 0.9930));
    color = lerp(color, color + push.sun_color.rgb * 0.16, band(sun, 0.9972));
    color = lerp(color, push.sun_color.rgb * 2.6, band(sun, 0.99915));

    /* high clouds: two-tone shapes on a plane far above, thinning to the horizon */
    float2 high  = dir.xz / max(up, 0.03) * 0.42 + float2(time * 0.006, 0.0);
    float  puffs = mu_fbm4(mu_domain_warp2(high * 1.1, 0.7), 2.0, 0.5) * 0.5 + 0.5;
    float  thin  = mu_saturate(up * 9.0) * mu_saturate(1.5 - length(high) * 0.16);
    float3 cloud = lerp(push.horizon.rgb * float3(1.06, 0.98, 1.0), float3(1.0, 0.95, 0.90), band(puffs, 0.60));
    color        = lerp(color, cloud, band(puffs, 0.53) * thin * 0.9);

    /* Cloud sea: the plane below the island, hit by this ray. Three flat tones
     * cut from one warped noise, melting into the horizon with distance. */
    float  travel = (push.camera.y - push.haze.w) / max(-up, 0.012);
    float2 sea    = (push.camera.xz + dir.xz * travel) * 0.028 + float2(time * 0.004, time * 0.002);
    float  billow = mu_fbm4(mu_domain_warp2(sea, 0.9), 2.0, 0.5) * 0.5 + 0.5 + mu_value_noise2(sea * 6.0) * 0.03;
    float3 sea_c  = lerp(push.haze.rgb * float3(0.74, 0.70, 0.94), push.haze.rgb, band(billow, 0.44));
    sea_c         = lerp(sea_c, float3(1.0, 0.94, 0.88), band(billow, 0.57));
    sea_c         = lerp(sea_c, push.horizon.rgb, smoothstep(0.25, 1.0, mu_saturate(travel * 0.0045)));
    color         = lerp(color, sea_c, mu_saturate(-up * 40.0));

    o.color = float4(color, 1.0);
    o.aux   = float4(0.0, 0.0, 0.0, 400.0);
    o.id    = float4(0.0, 0.0, 0.0, 0.0);
    return o;
}
