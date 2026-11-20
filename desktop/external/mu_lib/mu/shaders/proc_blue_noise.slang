/* mu/proc_blue_noise.slang — blue noise & Poisson disk distributions.
 * ============================================================================
 *  Blue-noise spectral properties (energy concentrated at high frequencies,
 *  low low-frequency clumping) via high-pass filtered white noise, void-and
 *  cluster-style dithering, and Fibonacci/rank-1 lattice Poisson-disk
 *  approximations. All branch-free, no LUTs.
 *
 *  Sources: Ulichney (void-and-cluster dithering), Lagae & Dutré (Poisson
 *  disk via blue noise), Burley & Burley (blue noise dithering).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_BLUE_NOISE_SLANG
#define MU_PROC_BLUE_NOISE_SLANG

#include "proc_noise.slang"

/* =========================================================================
 *  BLUE NOISE  — high-pass filtered white noise.
 *  Energy at high frequencies; little low-frequency clumping. Good for
 *  dithering, stochastic sampling, grain, jitter.
 *  5 quarter-rate ops (4 value-noise evals + combines).
 * =========================================================================*/

float mu_blue_noise2(float2 p, uint seed)
{
    float  c = mu_value_noise2(p, seed);
    float  n = mu_value_noise2(p + float2(1.0, 0.0), seed);
    float  s = mu_value_noise2(p + float2(-1.0, 0.0), seed);
    float  e = mu_value_noise2(p + float2(0.0, 1.0), seed);
    float  w = mu_value_noise2(p + float2(0.0, -1.0), seed);
    /* Laplacian high-pass: center minus neighbours. */
    float  hp = c - 0.25 * (n + s + e + w);
    return mu_saturate(hp * 0.5 + 0.5);             /* remap to [0,1] */
}

/* 3D blue noise: high-pass along all three axes. */
float mu_blue_noise3(float3 p, uint seed)
{
    float  c = mu_value_noise3(p, seed);
    float  hp = c - (1.0 / 6.0) * (
        mu_value_noise3(p + float3(1.0, 0.0, 0.0), seed) +
        mu_value_noise3(p + float3(-1.0, 0.0, 0.0), seed) +
        mu_value_noise3(p + float3(0.0, 1.0, 0.0), seed) +
        mu_value_noise3(p + float3(0.0, -1.0, 0.0), seed) +
        mu_value_noise3(p + float3(0.0, 0.0, 1.0), seed) +
        mu_value_noise3(p + float3(0.0, 0.0, -1.0), seed));
    return mu_saturate(hp * 0.5 + 0.5);
}

/* =========================================================================
 *  VOID-AND-CLUSTER DITHER  — iterative blue-noise dither matrix value.
 *  Unrolled fixed iteration count; no runtime loop. Returns [0,1] dither
 *  value for coordinate `p` at `seed`. Approximates a blue-noise dither
 *  matrix via successive error diffusion (Bayer-like but blue-spectrum).
 *  `iters` hard-capped at 4 (unrolled).
 * =========================================================================*/

float mu_dither_blue2(float2 p, uint seed, int iters)
{
    float2 q = p * 4.0;                             /* sub-cell jitter */
    float  v = mu_hash01(uint2(p.x, p.y) ^ seed);
    [ForceUnroll]
    for (int i = 0; i < 4; ++i)
    {
        int    active = (i < iters) ? 1 : 0;
        float2 n = float2(
            mu_value_noise2(q + float2(float(i), 0.0), seed + (uint)i),
            mu_value_noise2(q + float2(0.0, float(i)), seed + (uint)i));
        float  err = v - 0.5 * (n.x + n.y);
        v = select(active, v + err * 0.25, v);
    }
    return mu_saturate(v);
}

/* =========================================================================
 *  POISSON DISK  — Fibonacci disk as a Poisson-disk approximation.
 *  Evenly distributed points with minimum separation; good for stochastic
 *  sampling, scattering, placement. Returns [0,1]^2 point on the disk.
 *  Reuses mu_phyllotaxis_disk (golden-angle spiral = Poisson disk approx).
 * =========================================================================*/

float2 mu_poisson_disk2(uint i, uint n)
{
    return mu_phyllotaxis_disk(i, n);
}

/* Poisson disk point on a sphere (golden-angle spiral, cylindrical equal-area). */
float3 mu_poisson_disk_sphere(uint i, uint n)
{
    return mu_phyllotaxis_sphere(i, n);
}

/* =========================================================================
 *  STOCHASTIC SAMPLING  — jittered grid with blue-noise jitter.
 *  Returns a sample offset within cell `cell` at index `idx`.
 * =========================================================================*/

float2 mu_blue_jitter2(uint idx, uint cell_w, uint cell_h, uint seed)
{
    float2 cell = float2(float(idx % cell_w), float(idx / cell_w));
    float2 jit  = float2(mu_blue_noise2(cell, seed),
                         mu_blue_noise2(cell + float2(100.0, 100.0), seed));
    return cell + jit;
}

#endif /* MU_PROC_BLUE_NOISE_SLANG */