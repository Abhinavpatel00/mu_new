/* mu/proc_model.slang — procedural modeling on the GPU.
 * ============================================================================
 *  L-system turtle interpretation, Gray-Scott reaction-diffusion step,
 *  implicit surfaces (metaballs / soft objects), superformula (supershapes),
 *  phyllotaxis point placement, and space-colonization branching.
 *
 *  Sources: Prusinkiewicz & Lindenmayer (Algorithmic Botany),
 *  Gray & Scott (reaction-diffusion / Turing patterns),
 *  Ebert et al. "Texturing & Modeling" (implicit surfaces, superformula),
 *  Inigo Quilez (SDFs, domain warping, repetition).
 *
 *  Rules: leaf-only, branch-free, select() instead of ?:,
 *  mad() for FMA, explicit quarter-rate-op comments.
 *
 *  Note: L-system *string expansion* runs on the CPU (mu/mu_lsystem.h);
 *  this file interprets the final string on the GPU into segments or a
 *  signed-distance field. Reaction-diffusion and CA run as compute
 *  passes ping-ponging two RWTexture2D/RWStructuredBuffers.
 * ==========================================================================*/
#ifndef MU_PROC_MODEL_SLANG
#define MU_PROC_MODEL_SLANG

#include "noise.slang"

/* =========================================================================
 *  L-SYSTEM TURTLE INTERPRETATION  (GPU side)
 * =========================================================================
 *  Interpret an expanded L-system string into 2D line segments.
 *  `mu_lsystem_interpret_tex` reads symbols from a Texture2D<uint> (one
 *  byte per symbol) and writes segments into g_ls_segments.
 *  `mu_lsystem_sdf2` rasterizes the string into a signed-distance field
 *  (good for signed-distance marching / extrusion).
 *
 *  Symbols:
 *   F = forward and draw   f = forward without drawing
 *   + = turn left  - = turn right   [ = push state  ] = pop state
 *   All other symbols are no-ops (reserved for leaves/flowers/etc.).
 * =========================================================================*/

struct LSSeg { float2 p0, p1; };

RWStructuredBuffer<LSSeg> g_ls_segments;

/* Interpret a null-terminated string exposed via a Texture2D
 * of R8Uint (one byte per symbol). Returns segment count, writes segments.
 * `tex` dimensions: (len, 1). `sampler` is point-clamp. */
int mu_lsystem_interpret_tex(Texture2D<uint> tex, SamplerState samp,
                             int len, float step, float angle,
                             int max_segments)
{
    float2 p = float2(0.0);
    float  a = 0.0;
    float2 sx[32], sy[32]; float sa[32]; int sp = 0;
    int n = 0;
    [ForceUnroll]
    for (int i = 0; i < len && n < max_segments; ++i)
    {
        uint b = tex.Load(int3(i, 0, 0)).x;
        /* ASCII: 'F'=70 'f'=102 '+'=43 '-'=45 '['=91 ']'=93 */
        if (b == 70u)                                         /* 'F' */
        {
            float2 np = p + step * float2(cos(a), sin(a));
            int    idx = min(n, max_segments - 1);            /* clamp into range */
            g_ls_segments[idx] = LSSeg{p, np};                /* harmless overwrite when full */
            n += (n < max_segments) ? 1 : 0;                  /* only count valid writes */
            p = np;
        }
        else if (b == 102u)                                   /* 'f' */
            p += step * float2(cos(a), sin(a));
        else if (b == 43u)                                    /* '+' */
            a += angle;
        else if (b == 45u)                                    /* '-' */
            a -= angle;
        else if (b == 91u && sp < 32)                         /* '[' */
        {
            sx[sp] = p.x; sy[sp] = p.y; sa[sp] = a; ++sp;
        }
        else if (b == 93u && sp > 0)                          /* ']' */
        {
            --sp; p = float2(sx[sp], sy[sp]); a = sa[sp];
        }
    }
    return n;
}

/* Signed distance from point `p` to the L-system polyline defined by the
 * texture. Renders the system as an implicit curve: thin where far,
 * thick near. Use `thickness` to control stroke width. */
float mu_lsystem_sdf2(Texture2D<uint> tex, SamplerState samp,
                      int len, float2 p, float step, float angle,
                      float thickness)
{
    /* Replay the turtle, tracking minimum distance to any drawn segment. */
    float2 pos = float2(0.0);
    float  a   = 0.0;
    float2 sx[32], sy[32]; float sa[32]; int sp = 0;
    float  best = 1e3;
    [ForceUnroll]
    for (int i = 0; i < len; ++i)
    {
        uint b = tex.Load(int3(i, 0, 0)).x;
        /* ASCII: 'F'=70 'f'=102 '+'=43 '-'=45 '['=91 ']'=93 */
        if (b == 70u)                                         /* 'F' */
        {
            float2 np = pos + step * float2(cos(a), sin(a));
            /* Distance from p to segment pos->np. */
            float2 d  = np - pos;
            float   t = dot(p - pos, d) / max(dot(d, d), MU_EPS_SQ);
            t = mu_saturate(t);
            float2 q  = pos + d * t;
            float   d2 = dot(p - q, p - q);
            best = min(best, d2);
            pos = np;
        }
        else if (b == 102u)                                   /* 'f' */
            pos += step * float2(cos(a), sin(a));
        else if (b == 43u)                                    /* '+' */
            a += angle;
        else if (b == 45u)                                    /* '-' */
            a -= angle;
        else if (b == 91u && sp < 32)                         /* '[' */
        {
            sx[sp] = pos.x; sy[sp] = pos.y; sa[sp] = a; ++sp;
        }
        else if (b == 93u && sp > 0)                          /* ']' */
        {
            --sp; pos = float2(sx[sp], sy[sp]); a = sa[sp];
        }
    }
    return sqrt(max(best, 0.0)) - thickness;
}

/* =========================================================================
 *  GRAY-SCOTT REACTION-DIFFUSION STEP  (compute pass)
 * =========================================================================
 *  One explicit-Euler step of the Gray-Scott system on an RWTexture2D
 *  (RG32F: R=U, G=V). Call ping-pong between two textures.
 *
 *   du/dt = Du*lap(U) - U*V^2 + F*(1-U)
 *   dv/dt = Dv*lap(V) + U*V^2 - (F+k)*V
 *
 *  Toroidal (wrap) boundaries via modulo on the fetch coords.
 * =========================================================================*/

struct RDParams { float Du, Dv, F, k, dt; };

/* Fetch with toroidal wrap. */
float2 mu_rd_fetch(Texture2D<float2> tex, int2 dims, int2 uv)
{
    int2 w = uv % dims;                    /* HLSL %: trunc toward zero */
    w = select(w < 0, w + dims, w);        /* wrap negatives */
    return tex.Load(int3(w, 0)).xy;
}

void mu_rd_step(Texture2D<float2> src, RWTexture2D<float2> dst,
                int2 dims, RDParams prm)
{
    int2 g = int2(DispatchThreadID.xy);
    if (g.x >= dims.x || g.y >= dims.y) return;

    float  Du = prm.Du * prm.dt;
    float  Dv = prm.Dv * prm.dt;
    float   F = prm.F   * prm.dt;
    float   k = prm.k   * prm.dt;

    float2 c = mu_rd_fetch(src, dims, g);
    float  u = c.x, v = c.y;

    float lapU = 0.0, lapV = 0.0;
    [ForceUnroll]
    for (int dy = -1; dy <= 1; ++dy)
    [ForceUnroll]
    for (int dx = -1; dx <= 1; ++dx)
    {
        float2 n = mu_rd_fetch(src, dims, g + int2(dx, dy));
        lapU += n.x;
        lapV += n.y;
    }
    lapU -= 4.0 * u;
    lapV -= 4.0 * v;

    float uv2 = u * v * v;
    float un = u + Du * lapU - uv2 + F * (1.0 - u);
    float vn = v + Dv * lapV + uv2 - (F + k) * v;

    /* Guard against drift outside [0,1]. */
    un = mu_saturate(un);
    vn = mu_saturate(vn);

    dst[g] = float2(un, vn);
}

/* Seed: U=1, V=0 with a V-rich square spot at center. Call once before
 * stepping. `spot` = spot radius in cells. */
void mu_rd_seed(RWTexture2D<float2> tex, int2 dims, float spot)
{
    int2 g = int2(DispatchThreadID.xy);
    if (g.x >= dims.x || g.y >= dims.y) return;
    int2 c = dims / 2;
    float2 d = float2(g - c) / max(float(min(dims.x, dims.y)), 1.0);
    float r2 = dot(d, d);
    float spot_r = spot / max(float(min(dims.x, dims.y)), 1.0);
    float v = mu_saturate(1.0 - r2 / (spot_r * spot_r)) * 0.25;
    tex[g] = float2(0.5 + 0.5 * mu_saturate(spot_r * spot_r - r2), v);
}

/* =========================================================================
 *  IMPLICIT SURFACES / METABALLS
 * =========================================================================
 *  Soft objects (Blinn): sum of radial kernels, thresholded. Use for
 *  organic blobs, liquid, wax. `mu_metaball_field` returns the summed
 *  field; `mu_metaball_sdf` returns signed distance to the isosurface.
 * =========================================================================*/

struct MetaBall { float2 p; float r, s; };   /* position, radius, strength */

/* Blinn kernel: (1 - r^2/R^2)^2 for r<R, else 0. Quarter-rate sqrt. */
float mu_meta_kernel(float2 d, float R)
{
    float r = sqrt(max(dot(d, d), MU_EPS_SQ));    /* 1 quarter-rate sqrt */
    float t = mu_saturate(1.0 - r / max(R, MU_EPS));
    return t * t;
}

/* Summed metaball field at `p`. `balls` exposed as a StructuredBuffer.
 * Returns field value; surface is where field == threshold. */
float mu_metaball_field(StructuredBuffer<MetaBall> balls, int count,
                        float2 p)
{
    float sum = 0.0;
    [ForceUnroll]
    for (int i = 0; i < 16; ++i)                  /* hard cap 16 balls */
    {
        int   active = (i < count) ? 1 : 0;
        MetaBall b   = balls[i];                  /* OOB load -> 0, masked by active */
        float  k     = mu_meta_kernel(p - b.p, b.r) * b.s;
        sum += k * float(active);
    }
    return sum;
}

/* Signed distance to the metaball isosurface (approx via field gradient).
 * `threshold` is the isovalue (e.g. 1.0). */
float mu_metaball_sdf(StructuredBuffer<MetaBall> balls, int count,
                      float2 p, float threshold, float eps)
{
    float f = mu_metaball_field(balls, count, p);
    float inv = 0.5 / eps;
    float fx = (mu_metaball_field(balls, count, p + float2(eps, 0.0)) -
                mu_metaball_field(balls, count, p - float2(eps, 0.0))) * inv;
    float fy = (mu_metaball_field(balls, count, p + float2(0.0, eps)) -
                mu_metaball_field(balls, count, p - float2(0.0, eps))) * inv;
    float grad = sqrt(max(dot(float2(fx, fy), float2(fx, fy)), MU_EPS_SQ));
    return (f - threshold) / max(grad, MU_EPS);     /* 1 quarter-rate sqrt */
}

/* =========================================================================
 *  SUPERFORMULA / SUPERSHAPES  (Gielis)
 * =========================================================================
 *  r(phi) = ( |cos(m*phi/4)/a|^n2 + |sin(m*phi/4)/b|^n3 )^(-1/n1)
 *  One shape parameter set produces stars, flowers, spirals, polygons.
 *  Two quarter-rate ops (pow). Return radius at angle `phi`; the caller
 *  samples p = r(phi) * (cos phi, sin phi).
 * =========================================================================*/

float mu_superformula_r(float phi, float m, float a, float b,
                        float n1, float n2, float n3)
{
    float term1 = pow(abs(cos(m * phi * 0.25) / max(a, MU_EPS)), n2);
    float term2 = pow(abs(sin(m * phi * 0.25) / max(b, MU_EPS)), n3);
    return pow(term1 + term2, -1.0 / max(n1, MU_EPS));  /* 3 quarter-rate pow */
}

/* Superformula point in 2D at angle `phi`. */
float2 mu_superformula2(float phi, float m, float a, float b,
                        float n1, float n2, float n3)
{
    float r = mu_superformula_r(phi, m, a, b, n1, n2, n3);
    float2 sc = mu_sincos(phi);
    return r * sc;
}

/* Signed distance to a superformula curve (approx: radial distance minus
 * radius at that angle). Good enough for marching / extrusion. */
float mu_superformula_sdf2(float2 p, float m, float a, float b,
                           float n1, float n2, float n3)
{
    float phi = mu_atan2_fast(p.y, p.x);
    float r   = mu_superformula_r(phi, m, a, b, n1, n2, n3);
    return length(p) - r;                            /* 1 quarter-rate sqrt (length) */
}

/* =========================================================================
 *  PHYLLOTAXIS POINT PLACEMENT
 * =========================================================================
 *  Golden-angle spiral on the disk (and on a sphere via cylindrical
 *  equal-area). Even area coverage, no centre clustering — seeds, berries,
 *  flower heads, scales.
 * =========================================================================*/

/* Disk: index i of n points. Radius ~ sqrt(i/n), angle = golden angle. */
float2 mu_phyllotaxis_disk(uint i, uint n)
{
    float inv_n = 1.0 / max(float(n), 1.0);
    float r  = sqrt(float(i) * inv_n);                    /* 1 quarter-rate sqrt */
    float th = float(i) * MU_GOLDEN_ANG;
    float2 sc = mu_sincos(th);                            /* 2 quarter-rate */
    return r * sc;
}

/* Sphere via cylindrical equal-area: z = 1 - 2*i/n, radius = sqrt(1-z^2). */
float3 mu_phyllotaxis_sphere(uint i, uint n)
{
    float inv_n = 1.0 / max(float(n), 1.0);
    float z   = 1.0 - 2.0 * float(i) * inv_n;
    float r   = sqrt(max(1.0 - z * z, 0.0));              /* 1 quarter-rate sqrt */
    float th  = float(i) * MU_GOLDEN_ANG;
    float2 sc = mu_sincos(th);                            /* 2 quarter-rate */
    return float3(r * sc.x, r * sc.y, z);
}

/* =========================================================================
 *  SPACE COLONIZATION (simplified GPU step)
 * =========================================================================
 *  One growth iteration: for each leaf, find the nearest branch tip within
 *  `kill_dist` and attract it; tips beyond `kill_dist` are removed.
 *  Exposed as per-tip helpers so the caller loops over a StructuredBuffer.
 * =========================================================================*/

struct ColonTip  { float2 p; float2 dir; int alive; };
struct ColonLeaf { float2 p; int taken; };

/* Attraction force from tip to leaf; returns normalized direction toward
 * the leaf if within kill_dist, else zero. */
float2 mu_colonize_attract(float2 tip, float2 leaf, float kill_dist)
{
    float2 d = leaf - tip;
    float  d2 = dot(d, d);
    float  kd2 = kill_dist * kill_dist;
    float  w = mu_saturate(1.0 - d2 / kd2);
    return mu_safe_normalize(d) * w;
}

/* Mark a leaf as taken by the nearest tip within kill_dist. Returns 1 if
 * a tip claimed this leaf. `tips` is a StructuredBuffer so the probe can
 * drive it without a raw pointer. */
int mu_colonize_claim(float2 leaf, StructuredBuffer<ColonTip> tips,
                      int tip_count, float kill_dist, int2 my_id)
{
    int claimed = 0;
    float best = 1e3;
    [ForceUnroll]
    for (int i = 0; i < 16; ++i)
    {
        int    active = (i < tip_count && tips[i].alive) ? 1 : 0;
        float2 d      = leaf - tips[i].p;
        float  d2     = dot(d, d);
        float  kd2    = kill_dist * kill_dist;
        int    closer = (active && d2 < best && d2 < kd2) ? 1 : 0;
        best  = select(closer, d2, best);
        claimed = select(closer, 1, claimed);
        /* my_id kept for future multi-tip arbitration; unused here. */
        (void)my_id;
    }
    return claimed;
}

#endif /* MU_PROC_MODEL_SLANG */