/* mu/proc_texture.slang — procedural material patterns.
 * ============================================================================
 *  Wood, marble, brick/tile, fabric, leather, granite, water/caustics,
 *  fire/smoke, and galactic/nebula textures. All built on the extended
 *  noise in proc_noise.slang and the base noise in noise.slang.
 *
 *  Sources: Ebert et al. "Texturing & Modeling: A Procedural Approach"
 *  (wood, marble, marble noise, stone, cloud), Inigo Quilez (domain
 *  warping, repetition), and classic shader blog recipes.
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_TEXTURE_SLANG
#define MU_PROC_TEXTURE_SLANG

#include "proc_noise.slang"

/* =========================================================================
 *  WOOD  (rings + grain + warp)
 * =========================================================================
 *  Concentric rings from sin of radial distance, distorted by fbm and
 *  grain along the grain direction. Returns [0,1].
 * =========================================================================*/

float mu_wood(float2 p, float rings, float grain, float warp,
              uint seed, float2 grain_dir)
{
    float2 gd = mu_safe_normalize(grain_dir);
    float   along = dot(p, gd);
    float   warp_n = mu_fbm4(mu_domain_warp2(p, warp), 2.0, 0.5);
    float   r = length(p) + warp_n * 4.0;          /* 1 quarter-rate sqrt */
    float   rings_s = sin(mu_mad(rings, r, warp_n * 6.0));
    float   grain_s = sin(mu_mad(grain, along, warp_n * 3.0));
    return mu_saturate(0.5 * rings_s + 0.3 * grain_s + 0.2);
}

/* =========================================================================
 *  MARBLE VEIN  (banded sine displaced by noise)
 * =========================================================================
 *  Classic marble banding (see noise.slang mu_marble) with the band
 *  position displaced by value noise, creating organic veins.
 * Returns [0,1]. One quarter-rate sin.
 * =========================================================================*/

/* Veined marble: noise displaces the banding, creating organic veins. */
float mu_marble_vein(float2 p, float frequency, float warp,
                     float vein, uint seed)
{
    float2 q = p + float2(
        mu_value_noise2(p + float2(0.0, 5.2)) * warp,
        mu_value_noise2(p + float2(5.2, 0.0)) * warp);
    float band = sin(mu_mad(frequency, q.x,
                            mu_value_noise2(q) * vein * 8.0));
    return mu_saturate(band * 0.5 + 0.5);          /* 1 quarter-rate sin */
}

/* =========================================================================
 *  BRICK / TILE
 * =========================================================================
 *  Running-bond brick with optional offset rows and mortar.
 * Returns brick id (integer-ish float) and mortar mask in [0,1].
 * =========================================================================*/

/* Brick pattern: returns float2(brick_index, mortar). `brick_w`,`brick_h`
 * in UV units; `offset` shifts alternate rows by half a brick. */
float2 mu_brick(float2 p, float brick_w, float brick_h,
                float offset, float mortar)
{
    float2 st = p / float2(brick_w, brick_h);
    float2 r  = float2(floor(st.x), floor(st.y));
    float   row = r.y;
    /* Running bond: shift alternate rows by half a brick. */
    float   shift = mu_smooth01(frac(row * 0.5)) * offset;
    float2  cell = float2(floor(st.x + shift), r.y);
    float2  f = frac(float2(st.x + shift, st.y));

    /* Mortar: distance to cell borders. */
    float2 mortar = float2(
        mu_smooth01(f.x) * mu_smooth01(1.0 - f.x),
        mu_smooth01(f.y) * mu_smooth01(1.0 - f.y));
    float m = mu_saturate(1.0 - max(mortar.x, mortar.y) / max(mortar, MU_EPS));
    return float2(dot(cell, float2(1.0, 1000.0)), m);
}

/* =========================================================================
 *  FABRIC / WEAVE
 * =========================================================================
 *  Interlaced warp/weft threads: sin of x and y multiplied, thresholded.
 * Returns [0,1]: thread color. */
float mu_fabric(float2 p, float count, float thickness,
                float twist, uint seed)
{
    float2 q = p * count;
    /* rotation by twist*|p|^2; identity when twist == 0, so no branch needed */
    float2 sc = mu_sincos(twist * dot(p, p));           /* 2 quarter-rate */
    q = float2(sc.y * q.x - sc.x * q.y,
               sc.x * q.x + sc.y * q.y);
    float warp   = sin(mu_mad(MU_PI, q.x, 0.0));         /* 1 quarter-rate sin */
    float weft   = sin(mu_mad(MU_PI, q.y, 0.0));          /* 1 quarter-rate sin */
    float thread = mu_saturate(1.0 - abs(warp) / max(thickness, MU_EPS)) *
                   mu_saturate(1.0 - abs(weft) / max(thickness, MU_EPS));
    return thread;
}

/* =========================================================================
 *  LEATHER / GRAIN
 * =========================================================================
 *  Low-frequency wrinkles: fbm of |grad(value noise)| sharpened.
 * Returns [0,1]: grain intensity. */
float mu_leather(float2 p, float freq, float warp,
                 uint seed)
{
    float2 q = mu_domain_warp2(p, warp);
    float  n = mu_value_noise2(q);
    /* Approximate gradient magnitude via central difference (2 extra evals). */
    float e = 0.01;
    float gx = (mu_value_noise2(q + float2(e, 0.0)) -
                mu_value_noise2(q - float2(e, 0.0))) * (0.5 / e);
    float gy = (mu_value_noise2(q + float2(0.0, e)) -
                mu_value_noise2(q - float2(0.0, e))) * (0.5 / e);
    float grain = sqrt(max(gx * gx + gy * gy, MU_EPS_SQ));  /* 1 quarter-rate sqrt */
    return mu_saturate(mu_fbm4(q * freq, 2.0, 0.5) * 0.5 + grain * 0.5);
}

/* =========================================================================
 *  GRANITE / STONE
 * =========================================================================
 *  Speckled stone: fbm + worley cell borders, multiplied for a mottled look.
 * Returns [0,1]. */
float mu_granite(float2 p, float freq, float speck, uint seed)
{
    float2 q = p * freq;
    float  base = mu_fbm4(q, 2.0, 0.5);
    float2 ff   = mu_worley2_f1f2(q * 3.0, 1.0);
    float  specks = mu_saturate((ff.y - ff.x) * speck);
    return mu_saturate(base * 0.6 + specks * 0.4);
}

/* =========================================================================
 *  WATER / CAUSTICS
 * =========================================================================
 *  Caustic ripples: worley edge + domain warp + fbm shimmer.
 * Returns [0,1]: caustic intensity (bright = focused light). */
float mu_caustic(float2 p, float time, uint seed)
{
    float2 q = mu_domain_warp2(p + float2(time * 0.1, -time * 0.07), 0.4);
    float2 ff = mu_worley2_f1f2(q * 2.5, 1.0);
    float ridge = 1.0 - mu_saturate(ff.y - ff.x);
    float shimmer = mu_fbm4(q * 6.0 + float2(time * 0.5, 0.0),
                            2.0, 0.5);
    return mu_saturate(ridge * ridge * 0.85 + shimmer * 0.15);
}

/* =========================================================================
 *  FIRE / SMOKE
 * =========================================================================
 *  Turbulence-distorted noise, remapped to a flame gradient.
 * Returns [0,1]: flame intensity. */
float mu_fire(float2 p, float time, float freq, uint seed)
{
    float2 q = p * freq + float2(time * 0.5, 0.0);
    float  t = mu_turbulence4(mu_domain_warp2(q, 0.3), 2.0, 0.5);
    float  flame = mu_saturate(t * 1.5 - 0.3);
    /* Hot core: brighter center. */
    float  core = mu_saturate(1.0 - length(p) * 2.0);    /* 1 quarter-rate sqrt */
    return mu_saturate(flame * 0.7 + core * 0.3);
}

/* Smoke: billow noise, soft and diffuse. Returns [0,1] density. */
float mu_smoke(float2 p, float time, float freq, uint seed)
{
    float2 q = p * freq + float2(time * 0.2, 0.0);
    float  b = mu_ridged4(q, 2.0, 0.5);
    return mu_saturate(b * 0.8 + 0.2);
}

/* =========================================================================
 *  GALACTIC / NEBULA
 * =========================================================================
 *  Multi-octave curl-distorted noise: spiral arms + diffuse clouds.
 * Returns [0,1]: nebula intensity. */
float mu_nebula(float2 p, float time, uint seed)
{
    float2 q = p + float2(cos(time * 0.3), sin(time * 0.3)) * 2.0;  /* 2 quarter-rate */
    float2 curl = mu_curl_noise2(q * 0.8, 0.01);                    /* 4 value-noise evals */
    float2 r = q + curl * 0.5;
    float  n  = mu_fbm4(r * 1.5, 2.0, 0.5);
    float  arm = sin(mu_mad(3.0, mu_atan2_fast(r.y, r.x),
                            time * 0.5)) * 0.5 + 0.5; /* 1 atan2 + 1 sin */
    return mu_saturate(n * 0.6 + arm * 0.4);
}

#endif /* MU_PROC_TEXTURE_SLANG */