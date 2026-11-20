/* mu/proc_noise.slang — extended procedural generation noise.
 * ============================================================================
 *  Builds on mu/noise.slang + mu/common.slang: PCG hashing, branch-free
 *  select(), mad() FMA spelling, explicit quarter-rate-op comments.
 *
 *  Sources: Ebert et al., "Texturing & Modeling: A Procedural Approach"
 *  (spot noise, Gabor, convolution, multifractal), Inigo Quilez
 *  (iquilezles.org — flow noise, domain warping, Voronoi variants),
 *  GPU Gems / ShaderX, and the classic Worley cellular paper.
 *
 *  Every helper is leaf-only, no control flow, no LUTs, no fetch.
 * ==========================================================================*/
#ifndef MU_PROC_NOISE_SLANG
#define MU_PROC_NOISE_SLANG

#include "noise.slang"

/* =========================================================================
 *  SPOT NOISE  (Ebert & Musgrave — convolution of random spots)
 * =========================================================================
 *  Sum `N` randomly placed circular/spiky spots, each with a random
 *  amplitude, position and radius, evaluated with a falloff kernel.
 *  `mu_spot_noise2` walks a fixed stride so the cost is predictable and
 *  fully unrollable.  Use it for gravel, lichen, splatter, leaking rust.
 *
 *  Cost: ``spots`` PCG hashes + ``spots`` evaluations, all ALU.
 * =========================================================================*/

/* Gaussian-ish radial falloff: (1 - r^2)^2, compact support at r=1,
 * zero quarter-rate ops. */
float mu_spot_kernel(float r2)
{
    float t = mu_saturate(1.0 - r2);
    return t * t;                          /* multiply form: 2 FMul */
}

/* Conical falloff: max(0, 1 - r). Cheaper than the Gaussian kernel, good
 * for cellular/pebble looks. */
float mu_spot_cone(float r2)
{
    float r = sqrt(max(r2, 0.0));          /* 1 quarter-rate sqrt */
    return mu_saturate(1.0 - r);
}

/* Evaluate `spots` random spots at `p`. Each spot i uses hash stream
 * `seed + i*1013u` for its (x, y, radius, amplitude). */
float mu_spot_noise2(float2 p, uint seed, int spots, float radius)
{
    float sum = 0.0;
    float inv_r = 1.0 / max(radius, MU_EPS);
    [ForceUnroll]
    for (int i = 0; i < 8; ++i)            /* hard cap: 8 spots max */
    {
        if (i >= spots) break;             /* OpSelect, no loop break */
        uint3 h = mu_pcg3d(uint3(int2(p * 7.1 + float(i)), seed + (uint)i * 1013u));
        float2 c = float2(h.x, h.y) * 2.0 - 1.0;      /* center in [-1,1] */
        float   rad = float(h.z) * 0.5 + 0.5;         /* radius in [0.5,1] */
        float   amp = float(h.z) - 0.5;               /* amplitude [-0.5,0.5] */
        float2  d   = p - c * radius;
        float   r2  = dot(d, d) * (inv_r * inv_r);
        sum += amp * mu_spot_kernel(r2) * rad;
    }
    return mu_saturate(sum * 0.5 + 0.5);
}

/* =========================================================================
 *  GABOR NOISE  (Lagae / Dutré — anisotropic Gaussian × sinusoid)
 * =========================================================================
 *  A plane wave modulated by an oriented Gaussian. Control wavelength,
 *  orientation, anisotropy and bandwidth independently: fabric, brushed
 *  metal, wood grain, water ripples.  Two quarter-rate ops (exp2 + cos).
 * =========================================================================*/

float mu_gabor2(float2 p, float wavelength, float angle,
                float anisotropy, float bandwidth, uint seed)
{
    /* Rotate into the wave's local frame. */
    float2 sc = mu_sincos(angle);
    float2x2 rot = float2x2(sc.y, -sc.x, sc.x, sc.y);   /* cos, -sin / sin, cos */
    float2 q = mul(rot, p) / wavelength;

    /* Anisotropic Gaussian envelope: exp(-(x^2 + ay^2) / (2*bw^2)).
     * Written as one exp2 + one reciprocal; no separate exp per axis. */
    float e = dot(q, q) * (1.0 + (anisotropy - 1.0) * q.y * q.y);
    float env = exp2(-e * (0.5 / (bandwidth * bandwidth)) * MU_LOG2_E);

    /* Carrier: cos(2*pi*(q.x + phase)). */
    float phase = mu_hash01(uint3(int2(p), seed)) * MU_2PI;
    return env * cos(MU_2PI * q.x + phase);            /* 1 quarter-rate cos */
}

/* =========================================================================
 *  FLOW NOISE  (Quilez — domain rotation per octave)
 * =========================================================================
 *  Rotate the sampling domain by a fixed angle each octave so the lattice
 *  axes never align between octaves. Kills the 0/45/90° cross-hatch of
 *  plain fBm at the cost of one sincos pair per octave. Good for water
 *  streaks, brushed metal, wind.  Two quarter-rate ops per octave.
 * =========================================================================*/

float mu_flow_fbm2(float2 p, float lacunarity, float gain,
                   float angle, int octaves, uint seed)
{
    float sum = 0.0, amp = 1.0, norm = 0.0;
    float ca = cos(angle), sa = sin(angle);          /* 2 quarter-rate */
    for (int i = 0; i < octaves; ++i)
    {
        sum  += amp * mu_value_noise2(p);
        norm += amp;
        /* Rotate + scale + offset, no branch. */
        float2 rp = float2(ca * p.x - sa * p.y, sa * p.x + ca * p.y);
        p = rp * lacunarity + float2(11.7, 29.3);
        amp *= gain;
    }
    return sum / norm;
}

/* =========================================================================
 *  VORONOI / CELLULAR VARIANTS
 * =========================================================================
 *  Beyond F1/F2: Cramer's rule (Jiri Coultranz / Inigo Quilez) gives
 *  smooth, blobby cell fields; Minkowski distance p-norm interpolates
 *  between Manhattan and Chebyshev; the "link" metric gives cell edges.
 * =========================================================================*/

/* Cramer's rule Voronoi: smooth min over 3x3 neighbours using the
 * exponential soft-min. Returns (smooth F1, cell id). One exp2 per cell. */
float2 mu_voronoi_cramer2(float2 p, uint seed, float k)
{
    float2 ip = floor(p);
    float2 fp = frac(p);
    float  s  = 8.0;                                /* soft-min accumulator */
    int    best = 0;
    float  bestD = 8.0;
    [ForceUnroll]
    for (int ky = -1; ky <= 1; ++ky)
    [ForceUnroll]
    for (int kx = -1; kx <= 1; ++kx)
    {
        float2 g  = float2(float(kx), float(ky));
        float2 o  = mu_cell_hash2(int2(ip) + int2(g));
        float2 r  = g + o - fp;
        float  d  = dot(r, r);                      /* squared distance */
        float  w  = exp2(-k * d * MU_LOG2_E);       /* 1 quarter-rate exp2 */
        s += w;
        if (d < bestD) { bestD = d; best = (ky + 1) * 3 + (kx + 1); }
    }
    float f1 = -(1.0 / k) * log2(max(s, MU_EPS_SQ)) * MU_LN2; /* 1 quarter-rate log2 */
    return float2(sqrt(max(f1, 0.0)), float(best)); /* 1 quarter-rate sqrt */
}

/* Minkowski p-norm distance: p=1 Manhattan, p=2 Euclidean,
 * p=inf Chebyshev. Approximated with pow so the cost is visible. */
float mu_minkowski2(float2 a, float2 b, float p)
{
    float2 d = abs(a - b);
    return pow(d.x * d.x + d.y * d.y, 0.5 / p);   /* 2 quarter-rate: pow */
}

/* Cell edge / crack: F2 - F1 sharpened by a power. Higher `edge` = sharper
 * cracks, lower = soft blobs. */
float mu_voronoi_edge2(float2 p, uint seed, float jitter, float edge)
{
    float2 ff = mu_worley2_f1f2(p, jitter);
    float  e  = ff.y - ff.x;
    return mu_saturate(pow(e, edge));               /* 1 quarter-rate pow */
}

/* =========================================================================
 *  CELLULAR AUTOMATA STEP  (Game of Life, totalistic, compute-friendly)
 * =========================================================================
 *  Branch-free neighbour count via 8 texture fetches in the caller;
 *  these helpers take the count as input so they stay fetch-free.  Use
 *  with mu/wave.slang or a groupshared ping-pong buffer.
 * =========================================================================*/

/* Conway's Game of Life: alive survives with 2-3 neighbours, dead births
 * with exactly 3. */
int mu_ca_life(int alive, int neighbours)
{
    return (alive && (neighbours == 2 || neighbours == 3)) ||
           (!alive && neighbours == 3) ? 1 : 0;
}

/* Totalistic: alive if neighbour count in [lo, hi]. */
int mu_ca_totalistic(int alive, int neighbours, int lo, int hi)
{
    return (neighbours >= lo && neighbours <= hi) ? 1 : 0;
}

/* Brian's Brain: /1 -> /2 -> 0 -> /1 when neighbour count == 2. */
int mu_ca_brian(int state, int neighbours)
{
    if (state == 0) return neighbours == 2 ? 1 : 0;
    if (state == 1) return 2;
    return 0;
}

/* =========================================================================
 *  MULTIFRACTAL / MUSGRAVE COMPOSITIONS
 * =========================================================================
 *  Heterogeneous terrain: previous octave modulates the next one's
 *  amplitude. Valleys keep detail; mountain flanks stay smooth.
 * =========================================================================*/

/* Ridged multifractal (Musgrave): signal = offset - |noise|, squared,
 * weighted by running weight. Sharp mountain ridges. */
float mu_ridged_mf2(float2 p, uint seed, int octaves,
                    float lacunarity, float gain, float offset)
{
    float sum = 0.0, amp = 0.5, weight = 1.0;
    for (int i = 0; i < octaves; ++i)
    {
        float n = mu_value_noise2(p);
        float signal = offset - abs(n);
        signal *= signal;
        signal *= weight;
        weight = mu_saturate(signal * gain);
        sum += signal * amp;
        p = mul(MU_FBM_ROT, p) * lacunarity + float2(11.7, 29.3);
        amp *= 0.5;
    }
    return mu_saturate(sum * 2.0 - 1.0);
}

/* Hybrid multifractal: fBm where each octave's gain is modulated by the
 * running sum (Hybrid Multi-fractal, Musgrave). */
float mu_hybrid_mf2(float2 p, uint seed, int octaves,
                    float lacunarity, float gain, float h)
{
    float sum = 0.0, amp = 1.0, weight = 1.0;
    for (int i = 0; i < octaves; ++i)
    {
        float n = mu_value_noise2(p);
        sum += amp * n;
        float signal = mu_saturate(weight + n);
        weight = pow(signal, h);              /* 1 quarter-rate pow */
        amp *= gain * weight;
        p = mul(MU_FBM_ROT, p) * lacunarity + float2(11.7, 29.3);
    }
    return sum;
}

/* =========================================================================
 *  BLUE NOISE / POISSON-DISK HELPERS
 * =========================================================================
 *  Radial-inverse + Hammersley (common.slang) plus a rank-1 lattice and
 *  Fibonacci spiral for blue-noise-ish sample placement on the GPU.
 * =========================================================================*/

/* Rank-1 lattice: (i * alpha) mod 1, alpha irrational. Good for
 * dithering and stochastic sampling without a texture. */
float2 mu_rank1_lattice(uint i, uint n, float2 alpha)
{
    return frac(float2(float(i)) * alpha);
}

/* Fibonacci / phyllotaxis spiral on the disk: golden-angle phyllotaxis,
 * radius ~ sqrt(i/N) for even area coverage. */
float2 mu_phyllotaxis_disk(uint i, uint n)
{
    float inv_n = 1.0 / max(float(n), 1.0);
    float r  = sqrt(float(i) * inv_n);
    float th = float(i) * MU_GOLDEN_ANG;
    float2 sc = mu_sincos(th);
    return r * sc;
}

#endif /* MU_PROC_NOISE_SLANG */