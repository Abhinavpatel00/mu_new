/* mu/proc_clouds.slang — cloud recipes from Texturing & Modeling.
 * ============================================================================
 *  Cumulus, stratus, and cirrus clouds built from fbm/turbulence/ridged
 *  noise with soft edges, lighting anisotropy, and vertical layering.
 *  All returns [0,1] cloud density unless noted.
 *
 *  Sources: Ebert et al. "Texturing & Modeling: A Procedural Approach"
 *  (clouds chapter), Inigo Quilez (cloud SDF / soft-edge tricks).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_CLOUDS_SLANG
#define MU_PROC_CLOUDS_SLANG

#include "proc_noise.slang"

/* =========================================================================
 *  CUMULUS  — puffy, soft-edged, vertically developing clouds.
 *  fbm base + turbulence detail, masked by a soft radial falloff so the
 *  cloud looks like a puff rather than an infinite field.
 * =========================================================================*/

float mu_cloud_cumulus(float2 p, float time, float freq, uint seed)
{
    float2 q = p * freq + float2(time * 0.15, -time * 0.10);
    float  base = mu_turbulence4(mu_domain_warp2(q, 0.35), 3.0, 0.55);
    /* Soft radial falloff: bright center, fades at edges. */
    float r = length(p);                              /* 1 quarter-rate sqrt */
    float falloff = mu_saturate(1.0 - r * 1.6);
    float density = base * falloff * falloff;         /* squared for softer edge */
    return mu_saturate(density * 1.2 - 0.2);
}

/* =========================================================================
 *  STRATUS  — flat, layered, sheet-like clouds.
 *  Strong vertical (y) layering, low horizontal frequency, slight drift.
 * =========================================================================*/

float mu_cloud_stratus(float2 p, float time, float freq, uint seed)
{
    float2 q = p * freq + float2(time * 0.05, 0.0);
    float  layer = sin(mu_mad(4.0, p.y, time * 0.03));   /* 1 quarter-rate sin */
    float  base  = mu_fbm4(q, 2.0, 0.5);
    float  density = mu_saturate(base * 0.6 + layer * 0.4 - 0.35);
    return density;
}

/* =========================================================================
 *  CIRRUS  — thin, wispy, streaky high-altitude clouds.
 *  Stretched noise along a wind direction, high anisotropy, very soft.
 * =========================================================================*/

float mu_cloud_cirrus(float2 p, float time, float freq, float wind_dir_x,
                      float wind_dir_y, uint seed)
{
    float2 wind = mu_safe_normalize(float2(wind_dir_x, wind_dir_y));
    float  along = dot(p, wind);
    float2 q = p * float2(freq * 0.25, freq * 2.0);    /* stretched */
    q += float2(time * 0.2, 0.0);
    float  base = mu_ridged4(mu_domain_warp2(q, 0.5), 2.0, 0.5);
    float  streak = mu_saturate(1.0 - abs(sin(mu_mad(8.0, along, 0.0)))); /* 1 sin */
    float density = base * streak * 0.8;
    return mu_saturate(density - 0.15);
}

/* =========================================================================
 *  CLOUD LAYER  — combine cumulus + stratus + cirrus into one sky layer.
 *  Weighted blend controlled by `type` (0=cumulus, 1=stratus, 2=cirrus).
 * =========================================================================*/

float mu_cloud_layer(float2 p, float time, float freq, float type, uint seed)
{
    float c = mu_cloud_cumulus(p, time, freq, seed);
    float s = mu_cloud_stratus(p, time, freq, seed);
    float r = mu_cloud_cirrus(p, time, freq, 1.0, 0.2, seed);
    /* Branch-free type select via smoothsteps. */
    float w0 = mu_saturate(1.0 - abs(type - 0.0) * 2.0);
    float w1 = mu_saturate(1.0 - abs(type - 1.0) * 2.0);
    float w2 = mu_saturate(1.0 - abs(type - 2.0) * 2.0);
    float sum = w0 + w1 + w2;
    float inv = mu_safe_rcp(max(sum, MU_EPS));
    return mu_saturate((c * w0 + s * w1 + r * w2) * inv);
}

#endif /* MU_PROC_CLOUDS_SLANG */