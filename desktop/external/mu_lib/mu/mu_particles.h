#pragma once

/*
===============================================================================

mu_particles.h — deterministic, data-oriented particle pool.

Design (DOD):
  - Structure-of-Arrays (SoA) for the hot update path: position, velocity and
    life live in separate contiguous float arrays so a single pass touches only
    what it needs and vectorizes cleanly.
  - Particles are spawned into a dense array; kills use swap-with-last
    compaction (O(1), no holes). Indices are therefore not stable across kills,
    which is fine for short-lived visual effects.
  - Every particle owns a pcg32 stream seeded at spawn, so spawn-time jitter
    (velocity spread, size, colour) is deterministic and replayable given the
    same spawn sequence.
  - mu_easing curves drive per-particle attributes over normalized life
    (t = 1 - life/ttl), e.g. size fade-out or colour Lerp.
  - A stable depth sort produces a back-to-front render order without moving
    the SoA data.

No engine dependency. No hidden allocation after init. All functions MU_INLINE.

===============================================================================
*/

#include "mu_common.h"
#include "mu/mu_pcg.h"
#include "mu/mu_easing.h"

MU_BEGIN_EXTERN_C

typedef struct mu_particles
{
    /* ---- SoA hot attributes (updated every frame) ---- */
    float*  px;   /* position x */
    float*  py;   /* position y */
    float*  pz;   /* position z */
    float*  vx;   /* velocity x */
    float*  vy;   /* velocity y */
    float*  vz;   /* velocity z */
    float*  life; /* seconds remaining */
    float*  ttl;  /* total lifetime (seconds), for normalized life t */
    float*  size; /* current size */
    uint32_t* color; /* packed 0xRRGGBB */

    /* ---- per-particle deterministic RNG streams ---- */
    uint64_t* rng_state;
    uint64_t* rng_inc;

    /* ---- bookkeeping ---- */
    uint32_t count;
    uint32_t cap;
    bool     owns; /* true if this instance malloc'd its arrays */
} mu_particles;

/* Forwarded: defined below after mu_particles_init. */
MU_INLINE void mu_particles_destroy(mu_particles* p);

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_particles_init(mu_particles* p, uint32_t cap)
{
    MU_ASSERT(p != NULL);
    MU_ASSERT(cap > 0u);

    size_t n = (size_t)cap;

    p->px = (float*)MU_MALLOC(n * sizeof(float));
    p->py = (float*)MU_MALLOC(n * sizeof(float));
    p->pz = (float*)MU_MALLOC(n * sizeof(float));
    p->vx = (float*)MU_MALLOC(n * sizeof(float));
    p->vy = (float*)MU_MALLOC(n * sizeof(float));
    p->vz = (float*)MU_MALLOC(n * sizeof(float));
    p->life = (float*)MU_MALLOC(n * sizeof(float));
    p->ttl = (float*)MU_MALLOC(n * sizeof(float));
    p->size = (float*)MU_MALLOC(n * sizeof(float));
    p->color = (uint32_t*)MU_MALLOC(n * sizeof(uint32_t));
    p->rng_state = (uint64_t*)MU_MALLOC(n * sizeof(uint64_t));
    p->rng_inc = (uint64_t*)MU_MALLOC(n * sizeof(uint64_t));

    bool ok = p->px && p->py && p->pz && p->vx && p->vy && p->vz &&
              p->life && p->ttl && p->size && p->color &&
              p->rng_state && p->rng_inc;
    if(!ok)
    {
        mu_particles_destroy(p);
        return false;
    }

    p->count = 0;
    p->cap = cap;
    p->owns = true;
    return true;
}

/* Static variant: caller supplies all storage, zero malloc. */
MU_INLINE bool mu_particles_init_static(mu_particles* p, uint32_t cap,
                                        float* px, float* py, float* pz,
                                        float* vx, float* vy, float* vz,
                                        float* life, float* ttl, float* size,
                                        uint32_t* color,
                                        uint64_t* rng_state, uint64_t* rng_inc)
{
    MU_ASSERT(p != NULL);
    MU_ASSERT(cap > 0u);
    MU_ASSERT(px && py && pz && vx && vy && vz && life && ttl && size &&
              color && rng_state && rng_inc);

    p->px = px; p->py = py; p->pz = pz;
    p->vx = vx; p->vy = vy; p->vz = vz;
    p->life = life; p->ttl = ttl; p->size = size;
    p->color = color;
    p->rng_state = rng_state;
    p->rng_inc = rng_inc;
    p->count = 0;
    p->cap = cap;
    p->owns = false;
    return true;
}

MU_INLINE void mu_particles_destroy(mu_particles* p)
{
    if(p == NULL)
        return;
    if(p->owns)
    {
        MU_FREE(p->px);
        MU_FREE(p->py);
        MU_FREE(p->pz);
        MU_FREE(p->vx);
        MU_FREE(p->vy);
        MU_FREE(p->vz);
        MU_FREE(p->life);
        MU_FREE(p->ttl);
        MU_FREE(p->size);
        MU_FREE(p->color);
        MU_FREE(p->rng_state);
        MU_FREE(p->rng_inc);
    }
    p->px = p->py = p->pz = NULL;
    p->vx = p->vy = p->vz = NULL;
    p->life = p->ttl = p->size = NULL;
    p->color = NULL;
    p->rng_state = p->rng_inc = NULL;
    p->count = 0;
    p->cap = 0;
    p->owns = false;
}

MU_INLINE void mu_particles_clear(mu_particles* p)
{
    MU_ASSERT(p != NULL);
    p->count = 0;
}

MU_INLINE uint32_t mu_particles_count(const mu_particles* p)  { return p->count; }
MU_INLINE uint32_t mu_particles_capacity(const mu_particles* p) { return p->cap; }

/* ---------- spawn / kill ---------- */

/* Spawn a particle. Returns its index, or MU_INVALID_INDEX if the pool is full.
 * `seed` seeds this particle's pcg stream; the same seed + spawn sequence is
 * fully deterministic. */
MU_INLINE uint32_t mu_particles_spawn(mu_particles* p,
                                      float x, float y, float z,
                                      float vx, float vy, float vz,
                                      float ttl, float size, uint32_t color,
                                      uint64_t seed)
{
    MU_ASSERT(p != NULL);
    MU_ASSERT(ttl > 0.0f);
    if(p->count >= p->cap)
        return MU_INVALID_INDEX;

    uint32_t i = p->count++;
    p->px[i] = x; p->py[i] = y; p->pz[i] = z;
    p->vx[i] = vx; p->vy[i] = vy; p->vz[i] = vz;
    p->life[i] = ttl;
    p->ttl[i] = ttl;
    p->size[i] = size;
    p->color[i] = color;
    /* Initialize the per-particle pcg stream (deterministic jitter source). */
    {
        mu_pcg32 rng;
        mu_pcg32_init(&rng, seed, seed);
        p->rng_state[i] = rng.state;
        p->rng_inc[i] = rng.inc;
    }
    return i;
}

/* Kill particle at `index`: swap-with-last, O(1). No-op if index out of range.
 * After this call the particle formerly at the back now sits at `index`. */
MU_INLINE void mu_particles_kill(mu_particles* p, uint32_t index)
{
    MU_ASSERT(p != NULL);
    if(index >= p->count)
        return;
    uint32_t last = p->count - 1u;
    if(index != last)
    {
        p->px[index] = p->px[last];
        p->py[index] = p->py[last];
        p->pz[index] = p->pz[last];
        p->vx[index] = p->vx[last];
        p->vy[index] = p->vy[last];
        p->vz[index] = p->vz[last];
        p->life[index] = p->life[last];
        p->ttl[index] = p->ttl[last];
        p->size[index] = p->size[last];
        p->color[index] = p->color[last];
        p->rng_state[index] = p->rng_state[last];
        p->rng_inc[index] = p->rng_inc[last];
    }
    p->count = last;
}

/* ---------- attribute access ---------- */

MU_INLINE void mu_particles_position(const mu_particles* p, uint32_t i,
                                     float out[3])
{
    MU_ASSERT(i < p->count);
    out[0] = p->px[i];
    out[1] = p->py[i];
    out[2] = p->pz[i];
}

MU_INLINE void mu_particles_set_position(mu_particles* p, uint32_t i,
                                         float x, float y, float z)
{
    MU_ASSERT(i < p->count);
    p->px[i] = x;
    p->py[i] = y;
    p->pz[i] = z;
}

MU_INLINE float mu_particles_life(const mu_particles* p, uint32_t i)
{
    MU_ASSERT(i < p->count);
    return p->life[i];
}

MU_INLINE float mu_particles_normalized_life(const mu_particles* p, uint32_t i)
{
    MU_ASSERT(i < p->count);
    float ttl = p->ttl[i];
    if(ttl <= 0.0f)
        return 1.0f;
    float t = 1.0f - p->life[i] / ttl;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

/* Evaluate an easing curve over this particle's normalized life. */
MU_INLINE float mu_particles_curve(const mu_particles* p, uint32_t i,
                                   mu_ease_kind kind)
{
    return mu_ease_get(kind)(mu_particles_normalized_life(p, i));
}

/* ---------- update ---------- */

/* Advance every live particle by `dt` seconds:
 *   pos += vel * dt; vel += gravity * dt; vel *= drag; life -= dt.
 * Particles whose life reaches 0 are removed (swap-with-last compaction).
 * Returns the number of particles remaining.
 *
 * Fixed-step friendly: pass a constant dt and the result is bit-identical
 * regardless of frame slicing (integration is explicit Euler, deterministic). */
MU_INLINE uint32_t mu_particles_update(mu_particles* p, float dt,
                                       const float gravity[3], float drag)
{
    MU_ASSERT(p != NULL);
    MU_ASSERT(dt >= 0.0f);
    if(dt < 0.0f)
        dt = 0.0f;

    uint32_t w = 0; /* write index for compaction */
    for(uint32_t r = 0; r < p->count; ++r)
    {
        /* Integrate. */
        p->vx[r] += gravity[0] * dt;
        p->vy[r] += gravity[1] * dt;
        p->vz[r] += gravity[2] * dt;
        p->vx[r] *= drag;
        p->vy[r] *= drag;
        p->vz[r] *= drag;
        p->px[r] += p->vx[r] * dt;
        p->py[r] += p->vy[r] * dt;
        p->pz[r] += p->vz[r] * dt;

        p->life[r] -= dt;
        if(p->life[r] <= 0.0f)
            continue; /* expired: drop by not copying forward */

        if(w != r)
        {
            p->px[w] = p->px[r];
            p->py[w] = p->py[r];
            p->pz[w] = p->pz[r];
            p->vx[w] = p->vx[r];
            p->vy[w] = p->vy[r];
            p->vz[w] = p->vz[r];
            p->life[w] = p->life[r];
            p->ttl[w] = p->ttl[r];
            p->size[w] = p->size[r];
            p->color[w] = p->color[r];
            p->rng_state[w] = p->rng_state[r];
            p->rng_inc[w] = p->rng_inc[r];
        }
        ++w;
    }
    p->count = w;
    return w;
}

/* Per-particle jitter from this particle's pcg stream: returns a float in
 * [0,1). Advances the stream so repeated calls drift deterministically. */
MU_INLINE float mu_particles_jitter(mu_particles* p, uint32_t i)
{
    MU_ASSERT(i < p->count);
    mu_pcg32 rng;
    rng.state = p->rng_state[i];
    rng.inc = p->rng_inc[i];
    uint32_t v = mu_pcg32_next_u32(&rng);
    p->rng_state[i] = rng.state;
    p->rng_inc[i] = rng.inc;
    return (float)v / 4294967296.0f;
}

/* ---------- render-order helpers ---------- */

/* Squared distance from particle `i` to `point`. */
MU_INLINE float mu_particles_dist2(const mu_particles* p, uint32_t i,
                                   float x, float y, float z)
{
    float dx = p->px[i] - x;
    float dy = p->py[i] - y;
    float dz = p->pz[i] - z;
    return dx * dx + dy * dy + dz * dz;
}

/* Fill `out_order[count]` with particle indices sorted back-to-front (furthest
 * from `eye` first) using a stable insertion sort. The SoA data is not moved;
 * `out_order` is the render pass's iteration sequence. `out_order` must have
 * room for `mu_particles_count(p)` entries. */
MU_INLINE void mu_particles_sort_by_depth(const mu_particles* p,
                                          float ex, float ey, float ez,
                                          uint32_t* out_order)
{
    MU_ASSERT(p != NULL);
    MU_ASSERT(out_order != NULL);
    for(uint32_t i = 0; i < p->count; ++i)
        out_order[i] = i;
    /* Stable insertion sort by distance (furthest first). Ties keep original
     * spawn order, so overlapping equal-depth particles render consistently. */
    for(uint32_t i = 1u; i < p->count; ++i)
    {
        uint32_t key = out_order[i];
        float key_d = mu_particles_dist2(p, key, ex, ey, ez);
        int32_t j = (int32_t)i - 1;
        while(j >= 0 && mu_particles_dist2(p, out_order[(uint32_t)j], ex, ey, ez) < key_d)
        {
            out_order[(uint32_t)(j + 1)] = out_order[(uint32_t)j];
            --j;
        }
        out_order[(uint32_t)(j + 1)] = key;
    }
}

MU_END_EXTERN_C
