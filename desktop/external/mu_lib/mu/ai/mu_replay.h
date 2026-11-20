#pragma once

/*
===============================================================================

mu_replay.h — rollback/lockstep substrate: input ring + state digests.

The netcode contract (QuakeWorld -> prediction/reconciliation; Gambetta;
Fiedler) in its smallest honest form. mu_fixed proves deterministic math,
mu_save persists runs, mu_bitserial packs frames — this is the third piece:
record what the player did, detect desync instantly, and rewind.

- Input ring: one u32 input frame per tick (pack more via mu_bitserial).
- Rolling digest: FNV-1a over input frames and caller state digests;
  any single-bit divergence in a replayed tick changes the digest.
- Rollback: query which inputs to replay after target tick; the caller
  restores its state (snapshot or mu_save) and re-simulates, verifying
  the digest at the boundary.

No allocation, no globals. The digest is content-addressing for simulation.

===============================================================================
*/

#include "mu_common.h"

#include <string.h>

MU_BEGIN_EXTERN_C

#define MU_REPLAY_RING 2048

typedef struct mu_replay
{
    uint32_t inputs[MU_REPLAY_RING];
    uint64_t ticks[MU_REPLAY_RING];   /* tick id per slot                */
    uint32_t head;                    /* next write slot                 */
    uint32_t count;                   /* valid entries, <= RING          */
    uint64_t digest;                  /* rolling FNV-1a                  */
    uint64_t digest_tick;             /* tick the digest is current to   */
} mu_replay;

#define MU_REPLAY_FNV_OFFSET 1469598103934665603ULL
#define MU_REPLAY_FNV_PRIME  1099511628211ULL

MU_INLINE uint64_t mu_replay_digest_bytes(uint64_t seed,
                                          const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    uint64_t h = seed;
    size_t i;
    for (i = 0; i < n; ++i)
    {
        h ^= (uint64_t)p[i];
        h *= MU_REPLAY_FNV_PRIME;
    }
    return h;
}

MU_INLINE uint64_t mu_replay_digest_u32(uint64_t seed, uint32_t v)
{
    return mu_replay_digest_bytes(seed, &v, sizeof v);
}

MU_INLINE uint64_t mu_replay_digest_u64(uint64_t seed, uint64_t v)
{
    return mu_replay_digest_bytes(seed, &v, sizeof v);
}

MU_INLINE void mu_replay_init(mu_replay* r)
{
    memset(r, 0, sizeof *r);
    r->digest = MU_REPLAY_FNV_OFFSET;
}

/* Record one input frame for `tick_id` (ticks must be non-decreasing). */
MU_INLINE void mu_replay_record(mu_replay* r, uint64_t tick_id, uint32_t input)
{
    uint32_t slot = r->head;
    r->inputs[slot] = input;
    r->ticks[slot] = tick_id;
    r->head = (r->head + 1u) % MU_REPLAY_RING;
    if (r->count < MU_REPLAY_RING) r->count += 1;
    r->digest = mu_replay_digest_u64(r->digest, tick_id);
    r->digest = mu_replay_digest_u32(r->digest, input);
    r->digest_tick = tick_id;
}

/* Fold a caller state digest at a tick boundary (call after simulating the
 * tick: digest_state(sim_state) then pass it here). */
MU_INLINE void mu_replay_fold_state(mu_replay* r, uint64_t state_digest)
{
    r->digest = mu_replay_digest_u64(r->digest, state_digest);
}

/* Collect inputs to replay after `target_tick` (exclusive) up to newest.
 * Writes up to max_out frames to out_inputs. Returns the count written,
 * or -1 if the ring no longer holds that far back. */
MU_INLINE int mu_replay_rollback(const mu_replay* r, uint64_t target_tick,
                                 uint32_t* out_inputs, int max_out)
{
    /* oldest entry index */
    uint32_t oldest = (r->head + MU_REPLAY_RING - r->count) % MU_REPLAY_RING;
    int n = 0;
    uint32_t i;
    int found = 0;
    for (i = 0; i < r->count; ++i)
    {
        uint32_t slot = (oldest + i) % MU_REPLAY_RING;
        if (r->ticks[slot] <= target_tick) { found = 1; continue; }
        if (!found) return -1; /* target older than the ring */
        if (n < max_out) out_inputs[n++] = r->inputs[slot];
    }
    return n;
}

/* Digest of the last `count` recorded input frames (for cross-machine
 * comparison). Recomputes from the ring — cheap at gate scale. */
MU_INLINE uint64_t mu_replay_input_digest(const mu_replay* r)
{
    uint32_t oldest = (r->head + MU_REPLAY_RING - r->count) % MU_REPLAY_RING;
    uint64_t h = MU_REPLAY_FNV_OFFSET;
    uint32_t i;
    for (i = 0; i < r->count; ++i)
    {
        uint32_t slot = (oldest + i) % MU_REPLAY_RING;
        h = mu_replay_digest_u64(h, r->ticks[slot]);
        h = mu_replay_digest_u32(h, r->inputs[slot]);
    }
    return h;
}

MU_END_EXTERN_C
