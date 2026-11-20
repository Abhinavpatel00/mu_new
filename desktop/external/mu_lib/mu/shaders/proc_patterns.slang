/* mu/proc_patterns.slang — geometric & ornamental patterns.
 * ============================================================================
 *  Tiling / wallpaper, quasicrystals, interference & moiré, Islamic star
 *  grids, and Celtic-knot-style interlace. Built on proc_noise.slang.
 *
 *  Sources: wallpaper-group theory, Penrose / quasicrystal projection,
 *  Inigo Quilez (repetition, SDF primitives), and ornamental-geometry
 *  references (Kaplan & Salesin Islamic star patterns).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 * ==========================================================================*/
#ifndef MU_PROC_PATTERNS_SLANG
#define MU_PROC_PATTERNS_SLANG

#include "proc_noise.slang"

/* =========================================================================
 *  TILING / REPETITION
 * =========================================================================
 *  Cell-based repetition with hoisted inverse (from sdf.slang style):
 *  `mu_repeat_cell` folds `p` into a tile; `mu_repeat_mirror` mirrors.
 * =========================================================================*/

/* Fold p into [0, cell) repeatedly. Inverse precomputed outside. */
float2 mu_repeat_cell(float2 p, float2 cell, float2 inv_cell)
{
    return mod(p * inv_cell, float2(1.0)) * cell;
}

/* Mirror repeat: fold into [-cell/2, cell/2) symmetrically. */
float2 mu_repeat_mirror(float2 p, float2 cell)
{
    float2 c = mod(p + cell * 0.5, cell * 2.0) - cell;
    return abs(c) - cell * 0.5;
}

/* =========================================================================
 *  QUASICRYSTAL / PENROSE-STYLE PROJECTION
 * =========================================================================
 *  Sum of cosines at 5 evenly rotated angles (5-fold quasicrystal).
 *  The classic "quasicrystal" shader look (Projection method).
 *  10 quarter-rate ops (5 cos + 5 sin via sincos pairs).
 * =========================================================================*/

float mu_quasicrystal5(float2 p, float scale, float angle)
{
    float sum = 0.0;
    float2 sc = mu_sincos(angle);              /* 2 quarter-rate */
    float2x2 rot = float2x2(sc.y, -sc.x, sc.x, sc.y);
    float2 d = p;
    [ForceUnroll]
    for (int i = 0; i < 5; ++i)
    {
        sum += cos(dot(d, float2(1.0, 0.0)) * scale);  /* 1 quarter-rate cos */
        d = mul(rot, d);
    }
    return sum * 0.2;
}

/* 7-fold quasicrystal (heptagonal). */
float mu_quasicrystal7(float2 p, float scale, float angle)
{
    float sum = 0.0;
    float step = MU_2PI / 7.0;
    [ForceUnroll]
    for (int i = 0; i < 7; ++i)
    {
        float a = float(i) * step + angle;
        float2 sc = mu_sincos(a);              /* 2 quarter-rate per iter */
        sum += cos(dot(p, sc) * scale);        /* 1 quarter-rate cos */
    }
    return sum / 7.0;
}

/* =========================================================================
 *  INTERFERENCE / MOIRÉ
 * =========================================================================
 *  Two sinusoidal grids at slight angle/frequency difference produce
 *  moiré beat patterns. Good for fabric, scanned-screen, energy fields.
 *  2 quarter-rate ops (sin x2).
 * =========================================================================*/

float mu_moire2(float2 p, float freq1, float freq2,
                float angle, float phase)
{
    float2 sc = mu_sincos(angle);              /* 2 quarter-rate */
    float2x2 rot = float2x2(sc.y, -sc.x, sc.x, sc.y);
    float2 q = mul(rot, p);
    float  a = sin(mu_mad(freq1, p.x, phase));            /* 1 quarter-rate sin */
    float  b = sin(mu_mad(freq2, q.x, phase));            /* 1 quarter-rate sin */
    return mu_saturate(0.5 * (a + b) + 0.5);
}

/* Interference rings: concentric beats from two point sources. */
float mu_interference_rings(float2 p, float2 s1, float2 s2,
                            float freq, float phase)
{
    float d1 = length(p - s1);           /* 1 quarter-rate sqrt */
    float d2 = length(p - s2);           /* 1 quarter-rate sqrt */
    return mu_saturate(0.5 * sin(freq * (d1 - d2) + phase) + 0.5);  /* 1 sin */
}

/* =========================================================================
 *  ISLAMIC STAR-PATTERN GRID (simplified)
 * =========================================================================
 *  A contact-star grid: place n-pointed stars on a brick grid; each star
 *  is the superposition of rotated squared cosines. Control n (points),
 *  radius, and the rosette radius. Returns [0,1] star intensity.
 *  2*n quarter-rate ops (n sincos pairs) — keep n small (4..8).
 * =========================================================================*/

float mu_star2(float2 center, float2 p, int points,
               float radius, float inner)
{
    float2 d = p - center;
    float  r = length(d);                        /* 1 quarter-rate sqrt */
    float  phi = mu_atan2_fast(d.y, d.x);
    float  m   = float(points);
    /* Squared-cosine rosette: peaks at each star point. */
    float  petal = pow(mu_saturate(cos(m * phi * 0.5)), 4.0);
    /* Radial falloff: annulus between inner and outer radius. */
    float  rad = mu_saturate(1.0 - abs(r - radius) / max(inner, MU_EPS));
    return mu_saturate(petal * rad);             /* 1 quarter-rate pow */
}

/* Brick grid of stars with running-bond offset (like mu_brick). */
float mu_star_grid(float2 p, float cell, int points,
                   float radius, float inner, uint seed)
{
    float2 st = p / cell;
    float2 r  = float2(floor(st.x), floor(st.y));
    float   shift = mu_smooth01(frac(r.y * 0.5)) * 0.5;
    float2  cellp = float2(floor(st.x + shift), r.y);
    float2  f = frac(float2(st.x + shift, r.y));
    /* Distance to nearest cell centre. */
    float2 c = cellp * cell + cell * 0.5;
    float  d = length(p - c);                    /* 1 quarter-rate sqrt */
    /* Star at each cell centre, faded by distance to cell edge. */
    float star = mu_star2(c, p, points, radius, inner);
    float tile = mu_saturate(1.0 - d / (cell * 0.5));
    return star * tile;
}

/* =========================================================================
 *  CELTIC-KNOT-STYLE INTERLACE (approx)
 * =========================================================================
 *  Over-under weaving from two phase-shifted grids: where both grids are
 *  "on", the weave alternates. Returns [0,1] interlace intensity.
 * =========================================================================*/

float mu_interlace(float2 p, float count, float thickness,
                   float phase, uint seed)
{
    float2 q = p * count;
    float  a = sin(mu_mad(MU_PI, q.x, phase));          /* 1 quarter-rate sin */
    float  b = sin(mu_mad(MU_PI, q.y, phase + MU_PI));  /* 1 quarter-rate sin */
    float  wa = mu_saturate(1.0 - abs(a) / max(thickness, MU_EPS));
    float  wb = mu_saturate(1.0 - abs(b) / max(thickness, MU_EPS));
    /* Over-under: xor-like blend via min/max. */
    return mu_saturate(wa + wb - wa * wb);
}

#endif /* MU_PROC_PATTERNS_SLANG */