/* mu/proc_curves.slang — mathematical curve patterns.
 * ============================================================================
 *  Lissajous, hypotrochoid (spirograph), rose (rhodonea), and epitrochoid
 *  curves. Return signed distance to the curve stroke so they can be used
 *  as ornamental patterns or procedural geometry.
 *
 *  Sources: Weisstein (math curves), Quilez (curve SDFs).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_CURVES_SLANG
#define MU_PROC_CURVES_SLANG

#include "common.slang"

/* =========================================================================
 *  LISSAJOUS — x = sin(a*t + d), y = sin(b*t). Stroke distance via
 *  nearest-sample approximation over N unrolled t values.
 * =========================================================================*/

float mu_sd_lissajous2(float2 p, int a, int b, float d, float stroke,
                       int samples)
{
    float  best = 1e3;
    float2 sc   = mu_sincos(d);                     /* 2 quarter-rate */
    float  cd   = sc.y, sd = sc.x;
    [ForceUnroll]
    for (int i = 0; i < 16; ++i)                    /* hard cap 16 samples */
    {
        int    active = (i < samples) ? 1 : 0;
        float  t = float(i) * MU_2PI / 16.0;
        float  x = sin(mu_mad(float(a), t, d));    /* 1 quarter-rate sin */
        float  y = sin(mu_mad(float(b), t, 0.0));  /* 1 quarter-rate sin */
        float2 c = float2(x, y);
        float  dd = length(p - c);                  /* 1 quarter-rate sqrt */
        best = select(active && dd < best, dd, best);
    }
    return mu_saturate(best - stroke);
}

/* =========================================================================
 *  HYPOTROCHOID (spirograph) — x=(R-r)cos(t)+d*cos((R-r)/r*t),
 *  y=(R-r)sin(t)-d*sin((R-r)/r*t). Stroke distance via sampling.
 * =========================================================================*/

float mu_sd_hypotrochoid2(float2 p, float R, float r, float d, float stroke,
                          int samples)
{
    float  best = 1e3;
    float  k = (R - r) / max(r, MU_EPS);
    [ForceUnroll]
    for (int i = 0; i < 16; ++i)                    /* hard cap 16 samples */
    {
        int    active = (i < samples) ? 1 : 0;
        float  t = float(i) * MU_2PI / 16.0;
        float  kt = k * t;
        float2 sc1 = mu_sincos(t);                  /* 2 quarter-rate */
        float2 sc2 = mu_sincos(kt);                 /* 2 quarter-rate */
        float  x = (R - r) * sc1.y + d * sc2.y;
        float  y = (R - r) * sc1.x - d * sc2.x;
        float2 c = float2(x, y);
        float  dd = length(p - c);                  /* 1 quarter-rate sqrt */
        best = select(active && dd < best, dd, best);
    }
    return mu_saturate(best - stroke);
}

/* =========================================================================
 *  ROSE CURVE (rhodonea) — r = cos(k * theta). k=n/d gives n petals if
 *  n odd, 2n petals if n even. Stroke distance via angular sampling.
 * =========================================================================*/

float mu_sd_rose2(float2 p, int n, int d, float stroke, int samples)
{
    float  best = 1e3;
    float  k = float(n) / max(float(d), 1.0);
    [ForceUnroll]
    for (int i = 0; i < 16; ++i)                    /* hard cap 16 samples */
    {
        int    active = (i < samples) ? 1 : 0;
        float  th = float(i) * MU_2PI / 16.0;
        float  r = cos(mu_mad(k, th, 0.0));         /* 1 quarter-rate cos */
        float2 sc = mu_sincos(th);                  /* 2 quarter-rate */
        float2 c = r * sc;
        float  dd = length(p - c);                  /* 1 quarter-rate sqrt */
        best = select(active && dd < best, dd, best);
    }
    return mu_saturate(best - stroke);
}

/* =========================================================================
 *  EPITROCHOID — x=(R+r)cos(t)-d*cos((R+r)/r*t),
 *  y=(R+r)sin(t)-d*sin((R+r)/r*t). Stroke distance via sampling.
 * =========================================================================*/

float mu_sd_epitrochoid2(float2 p, float R, float r, float d, float stroke,
                         int samples)
{
    float  best = 1e3;
    float  k = (R + r) / max(r, MU_EPS);
    [ForceUnroll]
    for (int i = 0; i < 16; ++i)                    /* hard cap 16 samples */
    {
        int    active = (i < samples) ? 1 : 0;
        float  t = float(i) * MU_2PI / 16.0;
        float  kt = k * t;
        float2 sc1 = mu_sincos(t);                  /* 2 quarter-rate */
        float2 sc2 = mu_sincos(kt);                 /* 2 quarter-rate */
        float  x = (R + r) * sc1.y - d * sc2.y;
        float  y = (R + r) * sc1.x - d * sc2.x;
        float2 c = float2(x, y);
        float  dd = length(p - c);                  /* 1 quarter-rate sqrt */
        best = select(active && dd < best, dd, best);
    }
    return mu_saturate(best - stroke);
}

#endif /* MU_PROC_CURVES_SLANG */