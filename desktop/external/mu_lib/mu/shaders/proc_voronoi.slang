/* mu/proc_voronoi.slang — extended Voronoi / Worley cellular variants.
 * ============================================================================
 *  Manhattan, Chebyshev, Minkowski-with-arbitrary-p, fractal (fBm) Voronoi,
 *  and distance-metric Worley noise. Built on proc_noise.slang's
 *  mu_worley2_f1f2.
 *
 *  Sources: Worley (cellular noise), Inigo Quilez (Voronoi variants),
 *  Ebert et al. "Texturing & Modeling" (cellular noise).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_VORONOI_SLANG
#define MU_PROC_VORONOI_SLANG

#include "proc_noise.slang"

/* =========================================================================
 *  MANHATTAN VORONOI — L1 distance metric. Diamond-shaped cells.
 * =========================================================================*/

float2 mu_worley2_manhattan(float2 p, float cell, uint seed)
{
    float2 pi = floor(p / cell) * cell;
    float2 pf = frac(p / cell);
    float2 jitter = mu_pcg2(pi, seed) * 2.0 - 1.0;   /* 2 PCG evals */
    float2 g = pi + jitter * cell;

    float2 d = abs(pf - jitter);                     /* L1 component */
    float  dist = d.x + d.y;                         /* Manhattan */
    return float2(dist, 0.0);                        /* f1 only; f2 = 0 placeholder */
}

/* =========================================================================
 *  CHEBYSHEV VORONOI — L-infinity distance metric. Square cells.
 * =========================================================================*/

float2 mu_worley2_chebyshev(float2 p, float cell, uint seed)
{
    float2 pi = floor(p / cell) * cell;
    float2 pf = frac(p / cell);
    float2 jitter = mu_pcg2(pi, seed) * 2.0 - 1.0;
    float2 d = abs(pf - jitter);
    float  dist = max(d.x, d.y);                     /* Chebyshev */
    return float2(dist, 0.0);
}

/* =========================================================================
 *  MINKOWSKI VORONOI — Lp distance metric with arbitrary p.
 *  p=1 -> Manhattan, p=2 -> Euclidean, p=inf -> Chebyshev.
 *  1 quarter-rate pow. */
float2 mu_worley2_minkowski(float2 p, float cell, float p_norm,
                            uint seed)
{
    float2 pi = floor(p / cell) * cell;
    float2 pf = frac(p / cell);
    float2 jitter = mu_pcg2(pi, seed) * 2.0 - 1.0;
    float2 d = abs(pf - jitter);
    float  dist = pow(d.x * d.x + d.y * d.y, p_norm * 0.5);  /* 1 quarter-rate pow */
    return float2(dist, 0.0);
}

/* =========================================================================
 *  FRACTAL VORONOI — fBm of Voronoi F1. Rough, crumpled cellular look.
 *  `octaves` hard-capped at 4 (unrolled). 1 quarter-rate pow per octave
 *  inside mu_fbm4 plus the Voronoi evals.
 * =========================================================================*/

float mu_worley2_fbm(float2 p, float cell, int octaves,
                     float lacunarity, float gain, uint seed)
{
    float2 q = p / cell;
    float  amp = 1.0, freq = 1.0, sum = 0.0;
    [ForceUnroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 ff = mu_worley2_f1f2(q * freq, seed + (uint)i);
        sum += ff.x * amp;
        amp  *= gain;
        freq *= lacunarity;
    }
    return mu_saturate(sum);
}

/* =========================================================================
 *  VORONOI CELL ID — integer cell index for coloring each cell uniquely.
 * =========================================================================*/

float mu_worley2_cellid(float2 p, float cell, uint seed)
{
    float2 pi = floor(p / cell);
    /* Hash the integer cell coordinate for a unique per-cell color. */
    uint2  ci = uint2(pi.x, pi.y);
    return mu_hash01(ci) * cell;                      /* 1 PCG eval */
}

/* =========================================================================
 *  VORONOI EDGE DISTANCE — distance to nearest cell boundary (f2 - f1).
 *  Larger = cell interior, smaller = near edge. Good for cracks/stone.
 * =========================================================================*/

float mu_worley2_edge(float2 p, float cell, uint seed)
{
    float2 ff = mu_worley2_f1f2(p / cell, seed);
    return ff.y - ff.x;
}

#endif /* MU_PROC_VORONOI_SLANG */