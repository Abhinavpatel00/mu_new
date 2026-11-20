/* mu/proc_flowmap.slang — flow maps for water / liquid animation.
 * ============================================================================
 *  Flow vector fields, advection, and distortion textures for flowing
 *  water, lava, smoke, and energy. Built on proc_noise.slang curl and
 *  domain warp. Returns float2 flow vectors or float distortion values.
 *
 *  Sources: Inigo Quilez (flow noise, domain warping), Valve (flow maps
 *  for water), GPU Gems (flow map advection).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_FLOWMAP_SLANG
#define MU_PROC_FLOWMAP_SLANG

#include "proc_noise.slang"

/* =========================================================================
 *  FLOW NOISE  — divergence-free curl noise field. Returns a float2 flow
 *  vector (tangent to streamlines, no sources/sinks). Good for water
 *  currents, wind, lava swirls.
 * =========================================================================*/

float2 mu_flow_noise2(float2 p, float freq, float time, uint seed)
{
    float2 q = p * freq + float2(time * 0.1, -time * 0.08);
    return mu_curl_noise2(q, 0.01);                 /* divergence-free */
}

/* =========================================================================
 *  FLOW ADVECT  — advect a texture coordinate along a flow field for one
 *  step. `dt` = step size. Returns the advected coordinate.
 * =========================================================================*/

float2 mu_flow_advect(float2 uv, float2 flow, float dt)
{
    return uv + flow * dt;                          /* simple Euler advection */
}

/* =========================================================================
 *  FLOW DISTORT  — distort a lookup coordinate by a flow-map texture.
 *  `flow_tex` sampled at `uv`, scaled by `strength`. Returns distorted uv.
 * =========================================================================*/

float2 mu_flow_distort(float2 uv, Texture2D<float2> flow_tex,
                       SamplerState samp, float strength)
{
    float2 flow = flow_tex.Sample(samp, uv).xy * 2.0 - 1.0;
    return uv + flow * strength;
}

/* =========================================================================
 *  WATER SURFACE  — Gerstner-style wave sum: choppiness + displacement.
 *  Returns float2 horizontal displacement + float vertical height.
 *  `waves` hard-capped at 4 (unrolled).
 * =========================================================================*/

float3 mu_water_surface(float2 p, float time, float scale,
                        float amplitude, float choppiness, int waves)
{
    float2  disp = float2(0.0, 0.0);
    float   h    = 0.0;
    [ForceUnroll]
    for (int i = 0; i < 4; ++i)
    {
        int    active = (i < waves) ? 1 : 0;
        float  wlen = 1.0 / (float(i + 1) * scale);
        float  freq = MU_2PI * wlen;
        float2 dir  = float2(cos(float(i) * 0.7), sin(float(i) * 0.7));
        float  phase = dot(p, dir) * freq + time * freq * 0.5;
        float  a    = amplitude * (0.5f / float(i + 1));
        float  k    = freq;
        float  c    = cos(phase);
        float  s    = sin(phase);
        disp += dir * (a * k * s * choppiness * float(active));   /* horizontal chop */
        h    += a * c * float(active);                            /* vertical height */
    }
    return float3(disp, h);
}

/* =========================================================================
 *  FOAM / SPRAY  — whitewater from surface curvature + flow speed.
 *  Returns [0,1] foam intensity.
 * =========================================================================*/

float mu_water_foam(float2 p, float time, float freq, float speed,
                    uint seed)
{
    float2 q = p * freq + float2(time * speed, 0.0);
    float  n = mu_turbulence4(mu_domain_warp2(q, 0.3), 2.0, 0.5);
    float  foam = mu_saturate(n * 2.0 - 1.0);
    return foam;
}

/* =========================================================================
 *  CAUSTIC RIBBONS  — caustic light bands flowing along a direction.
 *  Returns [0,1] caustic intensity.
 * =========================================================================*/

float mu_caustic_ribbons(float2 p, float time, float freq,
                         float angle, float speed, uint seed)
{
    float2 sc = mu_sincos(angle);                   /* 2 quarter-rate */
    float2x2 rot = float2x2(sc.y, -sc.x, sc.x, sc.y);
    float2 q = mul(rot, p) * freq + float2(time * speed, 0.0);
    float  bands = sin(mu_mad(4.0, q.x, 0.0));      /* 1 quarter-rate sin */
    float  n   = mu_blue_noise2(q * 0.5 + float2(time * 0.1, 0.0), seed);
    return mu_saturate(bands * bands * 0.7 + n * 0.3);
}

#endif /* MU_PROC_FLOWMAP_SLANG */