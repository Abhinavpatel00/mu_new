/* mu/proc_shapes.slang — extended SDF primitives (2D).
 * ============================================================================
 *  Star, heart, cross, superellipse signed-distance functions.
 *  Complement the existing sdf.slang (circle, box, capsule, etc.).
 *
 *  Sources: Inigo Quilez (2D SDF collection), Gielis (superellipse).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_SHAPES_SLANG
#define MU_PROC_SHAPES_SLANG

#include "common.slang"

/* =========================================================================
 *  STAR SDF — n-pointed star, outer radius `r1`, inner radius `r2`.
 *  Quilez's sdStar: branch-free, unrolled over points. ~2 quarter-rate
 *  trig + 1 quarter-rate length per iteration.
 * =========================================================================*/

float mu_sd_star2(float2 p, int points, float r1, float r2)
{
    float  a = MU_PI / float(points);
    float2 sc = mu_sincos(a);                       /* 2 quarter-rate */
    float2x2 rot = float2x2(sc.y, -sc.x, sc.x, sc.y);
    float2 d = abs(p);
    [ForceUnroll]
    for (int i = 0; i < 8; ++i)                     /* hard cap 8 points */
    {
        int    active = (i < points) ? 1 : 0;
        p   = mul(rot, p);
        float2 pd = p * float2(r1, r2);
        float2 pd_abs = abs(pd);
        d = select(active, max(d, pd_abs), d);
    }
    float2 rh = float2(r1, r2);
    float  h = mu_saturate(0.5 * (d.x / max(r1, MU_EPS) +
                                d.y / max(r2, MU_EPS) + 1.0));
    float2 dh = d - rh * h;
    float  len = sqrt(max(dot(dh, dh), MU_EPS_SQ));  /* 1 quarter-rate sqrt */
    return len * select(h > 1.0, 1.0, -1.0);
}

/* =========================================================================
 *  HEART SDF — 2D heart curve, scaled by `size`.
 *  Implicit: (x^2 + y^2 - 1)^3 - x^2 y^3 = 0.
 *  1 quarter-rate sqrt.
 * =========================================================================*/

float mu_sd_heart2(float2 p, float size)
{
    float2 q = p / size;
    float  x = q.x, y = q.y;
    float  c = x * x + y * y - 1.0;
    float  f = c * c * c - x * x * y * y * y;
    float  g = sqrt(max(x * x + y * y, MU_EPS_SQ));   /* 1 quarter-rate sqrt */
    return f / max(g, MU_EPS) * size;
}

/* =========================================================================
 *  CROSS SDF — plus-sign cross, arm width `w`, arm length `l`.
 * =========================================================================*/

float mu_sd_cross2(float2 p, float w, float l)
{
    float2 q = abs(p);
    float  hw = w * 0.5, hl = l * 0.5;
    int    inside = ((q.x <= hw && q.y <= hl) || (q.x <= hl && q.y <= hw)) ? 1 : 0;
    /* Distance to nearest arm boundary. */
    float2 d1 = q - float2(hw, hl);   /* vertical arm corner */
    float2 d2 = q - float2(hl, hw);   /* horizontal arm corner */
    float  outside = max(max(d1.x, d1.y), max(d2.x, d2.y));
    float  d = select(inside, -outside, outside);
    return d;
}

/* =========================================================================
 *  SUPERELLIPSE SDF — |x/a|^n + |y/b|^n = 1. n=2 ellipse, n>2 squircle.
 *  1 quarter-rate pow, 1 quarter-rate sqrt.
 * =========================================================================*/

float mu_sd_superellipse2(float2 p, float a, float b, float n)
{
    float2 q = abs(p) / float2(a, b);
    float  h = pow(q.x * q.x + q.y * q.y, n * 0.5) - 1.0;   /* 1 quarter-rate pow */
    float  g = sqrt(max(q.x * q.x + q.y * q.y, MU_EPS_SQ)); /* 1 quarter-rate sqrt */
    return h * max(a, b) / max(g, MU_EPS);
}

#endif /* MU_PROC_SHAPES_SLANG */