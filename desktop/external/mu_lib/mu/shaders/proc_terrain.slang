/* mu/proc_terrain.slang — multifractal terrain & heightmap generation.
 * ============================================================================
 *  Ridged multifractal, valley noise, terrace/step erosion, domain-warped
 *  heightmaps, and basic hydraulic/thermal erosion approximations. All
 *  branch-free, no LUTs, built on proc_noise.slang.
 *
 *  Sources: Musgrave (multifractal terrain), Ebert et al. "Texturing &
 *  Modeling" (terrain chapter), Inigo Quilez (domain warping, ridge noise).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_TERRAIN_SLANG
#define MU_PROC_TERRAIN_SLANG

#include "proc_noise.slang"

/* =========================================================================
 *  RIDGED MULTIFRACTAL TERRAIN  — mountain ridges, sharp crests.
 *  Uses mu_ridged_mf2 from proc_noise; remaps to terrain height [0,1].
 * =========================================================================*/

float mu_terrain_ridge(float2 p, uint seed, int octaves,
                       float lacunarity, float gain, float offset,
                       float scale)
{
    float  h = mu_ridged_mf2(p * scale, seed, octaves, lacunarity, gain, offset);
    return mu_saturate(h);                          /* terrain height [0,1] */
}

/* =========================================================================
 *  VALLEY NOISE  — inverted ridged multifractal: ridges become valleys.
 *  Good for river networks, canyon floors, erosion basins.
 * =========================================================================*/

float mu_terrain_valley(float2 p, uint seed, int octaves,
                        float lacunarity, float gain, float offset,
                        float scale)
{
    float  h = mu_ridged_mf2(p * scale, seed, octaves, lacunarity, gain, offset);
    return mu_saturate(1.0 - h);                    /* invert: crests -> valleys */
}

/* =========================================================================
 *  TERRACE / STEP  — quantized heightmap for stepped terrain, rice
 *  paddies, eroded sediment layers. `steps` = number of discrete levels.
 *  1 quarter-rate op (floor via frac).
 * =========================================================================*/

float mu_terrain_terrace(float2 p, uint seed, int octaves,
                         float lacunarity, float gain, float offset,
                         float scale, int steps)
{
    float  h = mu_terrain_ridge(p, seed, octaves, lacunarity, gain, offset, scale);
    float  s = float(max(steps, 1));
    return floor(h * s) / max(s - 1.0, MU_EPS);     /* quantized levels */
}

/* =========================================================================
 *  DOMAIN-WARPED HEIGHTMAP  — classic terrain: base fbm warped by
 *  secondary fbm, producing natural-looking mountain ranges.
 * =========================================================================*/

float mu_terrain_heightmap(float2 p, uint seed, int octaves,
                           float lacunarity, float gain, float warp,
                           float scale)
{
    float2 q = mu_domain_warp2(p * scale, warp);
    float  h = mu_fbm4(q, 2.0, 0.5);
    return mu_saturate(h * 0.8 + 0.1);
}

/* =========================================================================
 *  EROSION APPROXIMATION  — hydraulic+thermal erosion heuristic:
 *  height-dependent slope softening. `water` = simulated water amount,
 *  `talus` = max slope angle (0..1). Returns eroded height [0,1].
 * =========================================================================*/

float mu_terrain_erode(float2 p, float height, float water, float talus)
{
    /* Slope approximation via central difference of heightfield. */
    float e = 0.01;
    float   hx = (height + e * mu_blue_noise2(p + float2(e, 0.0), 0u) -
                  height - e * mu_blue_noise2(p - float2(e, 0.0), 0.0)) * (0.5 / e);
    float   hy = (height + e * mu_blue_noise2(p + float2(0.0, e), 0u) -
                  height - e * mu_blue_noise2(p - float2(0.0, e), 0.0)) * (0.5 / e);
    float   slope = sqrt(max(hx * hx + hy * hy, MU_EPS_SQ));  /* 1 quarter-rate sqrt */
    /* Deposition where slope < talus, erosion where slope > talus. */
    float   deposit = mu_saturate((talus - slope) * water * 4.0);
    return mu_saturate(height + deposit * 0.1 - slope * (1.0 - talus) * 0.05);
}

/* =========================================================================
 *  BIOME BLEND  — blend between terrain types by height and moisture.
 *  Returns biome index (float) and blend factor.
 * =========================================================================*/

float2 mu_terrain_biome(float height, float moisture, int biome_count)
{
    float  n  = float(max(biome_count, 1));
    float  hb = height * n;                        /* height band */
    float  mb = moisture * n;                      /* moisture band */
    float   i = floor(hb + mb * 0.5) * (1.0 / n);   /* combined biome index */
    float   f = frac(hb + mb * 0.5);                /* blend within biome */
    return float2(i, f);
}

#endif /* MU_PROC_TERRAIN_SLANG */