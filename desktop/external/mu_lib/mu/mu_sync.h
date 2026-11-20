#pragma once

/*
===============================================================================

mu_sync.h — synchronization primitives built on mu/mu_atomic.h:
cache-line-padded spinlock, reader-writer lock and seqlock.

Per mu/mu_atomic.h's design rule, this header includes mu_atomic.h and adds
NO new atomic primitives — it only composes locks from them.

No allocation. All functions MU_INLINE.

===============================================================================
*/

#include "mu/mu_atomic.h"

#include <stddef.h>
#include <stdint.h>

#ifndef MU_CACHE_LINE_SIZE
#  define MU_CACHE_LINE_SIZE 64u
#endif

MU_BEGIN_EXTERN_C

/* ------------------------------------------------------------------
 * Cache-line padded spinlock (test-and-set with pause backoff).
 * ------------------------------------------------------------------ */

typedef struct mu_spinlock
{
    mu_atomic32_t locked;   /* 0 = free, 1 = held */
    uint8_t       pad[MU_CACHE_LINE_SIZE - sizeof(mu_atomic32_t)];
} mu_spinlock;

MU_INLINE void mu_spinlock_init(mu_spinlock* lk)
{
    MU_ASSERT(lk != NULL);
    mu_atomic32_init(&lk->locked, 0u);
}

MU_INLINE void mu_spinlock_lock(mu_spinlock* lk)
{
    MU_ASSERT(lk != NULL);
    uint32_t spins = 0u;
    for(;;)
    {
        uint32_t expected = 0u;
        if(mu_atomic32_compare_exchange(&lk->locked, &expected, 1u))
            return; /* acquired */
        expected = 0u;
        /* pause backoff, then yield periodically */
        if(++spins > 64u)
        {
            mu_cpu_relax();
            spins = 0u;
        }
        else
        {
            mu_cpu_relax();
        }
    }
}

MU_INLINE void mu_spinlock_unlock(mu_spinlock* lk)
{
    MU_ASSERT(lk != NULL);
    mu_atomic32_store_release(&lk->locked, 0u);
}

/* ------------------------------------------------------------------
 * Reader-writer lock (writer-preferred once a writer is waiting).
 * state: low16 = reader count, high16 = writers waiting.
 * ------------------------------------------------------------------ */

typedef struct mu_rwlock
{
    mu_atomic32_t state;
    uint8_t       pad[MU_CACHE_LINE_SIZE - sizeof(mu_atomic32_t)];
} mu_rwlock;

MU_INLINE void mu_rwlock_init(mu_rwlock* rw)
{
    MU_ASSERT(rw != NULL);
    mu_atomic32_init(&rw->state, 0u);
}

MU_INLINE void mu_rwlock_rdlock(mu_rwlock* rw)
{
    MU_ASSERT(rw != NULL);
    for(;;)
    {
        uint32_t s = mu_atomic32_load_acquire(&rw->state);
        uint32_t readers = s & 0xFFFFu;
        uint32_t writers = s >> 16;
        if(writers > 0u || readers == 0xFFFFu)
        {
            mu_cpu_relax();
            continue;
        }
        if(mu_atomic32_compare_exchange(&rw->state, &s, s + 1u))
            return;
    }
}

MU_INLINE void mu_rwlock_rdunlock(mu_rwlock* rw)
{
    MU_ASSERT(rw != NULL);
    mu_atomic32_fetch_add(&rw->state, 0xFFFFFFFFu); /* -1 readers */
}

MU_INLINE void mu_rwlock_wrlock(mu_rwlock* rw)
{
    MU_ASSERT(rw != NULL);
    /* announce writer intent (increment writers waiting) */
    mu_atomic32_fetch_add(&rw->state, 1u << 16);
    for(;;)
    {
        uint32_t s = mu_atomic32_load_acquire(&rw->state);
        uint32_t readers = s & 0xFFFFu;
        uint32_t writers = s >> 16;
        if(readers == 0u && writers == 1u)
            return; /* acquired: no readers, we're the only waiter */
        mu_cpu_relax();
    }
}

MU_INLINE void mu_rwlock_wrunlock(mu_rwlock* rw)
{
    MU_ASSERT(rw != NULL);
    mu_atomic32_fetch_add(&rw->state, (uint32_t)(-(int32_t)(1u << 16)));
}

/* ------------------------------------------------------------------
 * Seqlock: consistent reader-side snapshot without blocking writers.
 * ------------------------------------------------------------------ */

typedef struct mu_seqlock
{
    mu_atomic32_t seq;   /* even = unlocked, odd = write in progress */
    uint8_t       pad[MU_CACHE_LINE_SIZE - sizeof(mu_atomic32_t)];
} mu_seqlock;

MU_INLINE void mu_seqlock_init(mu_seqlock* sq)
{
    MU_ASSERT(sq != NULL);
    mu_atomic32_init(&sq->seq, 0u);
}

/* Writer: wait until seq is even, then mark odd (write in progress).
 * Returns the seq value to write under; call mu_seqlock_write_end when done. */
MU_INLINE uint32_t mu_seqlock_write_begin(mu_seqlock* sq)
{
    MU_ASSERT(sq != NULL);
    uint32_t s = mu_atomic32_load_acquire(&sq->seq);
    while(s & 1u)
    {
        mu_cpu_relax();
        s = mu_atomic32_load_acquire(&sq->seq);
    }
    mu_atomic32_store_release(&sq->seq, s + 1u);
    return s;
}

MU_INLINE void mu_seqlock_write_end(mu_seqlock* sq)
{
    MU_ASSERT(sq != NULL);
    /* increment to even: releases lock and bumps version */
    mu_atomic32_store_release(&sq->seq,
                              mu_atomic32_load_acquire(&sq->seq) + 1u);
}

/* Reader: snapshot seq at start; caller retries until mu_seqlock_read_retry
 * returns false. */
MU_INLINE uint32_t mu_seqlock_read_begin(const mu_seqlock* sq)
{
    MU_ASSERT(sq != NULL);
    return mu_atomic32_load_acquire(&sq->seq);
}

MU_INLINE bool mu_seqlock_read_retry(const mu_seqlock* sq, uint32_t start)
{
    MU_ASSERT(sq != NULL);
    uint32_t end = mu_atomic32_load_acquire(&sq->seq);
    return (start != end) || (start & 1u);
}

MU_END_EXTERN_C
