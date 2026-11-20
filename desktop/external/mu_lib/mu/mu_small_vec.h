#pragma once

/*
===============================================================================

mu_small_vec.h — small-buffer vector: inline storage + heap overflow.

The common case ("usually < 8 elements") allocates nothing; only when the
inline capacity is exceeded does it spill to the heap. Static variant takes
caller-provided storage (heap spill still malloc'd, or disabled).

DOD-friendly: elements are contiguous, iteration is a straight pointer walk,
and the common path touches one cache line.

Canonical surface: init / init_static / destroy / clear, push / pop / back /
front, count / capacity / empty, data span, operator-like at().

===============================================================================
*/

#include "mu_common.h"

#ifndef MU_SMALLVEC_INLINE_CAP
#  define MU_SMALLVEC_INLINE_CAP 4u
#endif

MU_BEGIN_EXTERN_C

typedef struct mu_small_vec
{
    void*   data;      /* points at inline_buf or heap */
    uint32_t count;
    uint32_t cap;
    uint32_t elem_size;
    uint8_t  inline_buf[(MU_SMALLVEC_INLINE_CAP) * sizeof(void*)];
} mu_small_vec;

/* Compile-time sanity: inline buffer must be able to hold inline_cap elements
 * of the largest type we realistically store (void* sized is the floor). */
MU_STATIC_ASSERT(MU_SMALLVEC_INLINE_CAP > 0u, "smallvec inline cap > 0");

MU_INLINE bool mu_small_vec_init(mu_small_vec* v, uint32_t elem_size,
                                 uint32_t inline_cap)
{
    MU_ASSERT(v != NULL);
    MU_ASSERT(elem_size > 0u);

    v->elem_size = elem_size;
    v->count = 0;

    if(inline_cap > 0u && inline_cap <= MU_SMALLVEC_INLINE_CAP)
    {
        v->data = v->inline_buf;
        v->cap = inline_cap;
        return true;
    }

    /* caller asked for more than inline fits -> pure heap */
    void* heap = MU_MALLOC((size_t)inline_cap * (size_t)elem_size);
    if(!heap)
        return false;
    v->data = heap;
    v->cap = inline_cap;
    return true;
}

/* Static variant: caller provides `inline_storage` (at least inline_cap *
 * elem_size bytes). No heap for the inline part; spill still malloc'd. */
MU_INLINE bool mu_small_vec_init_static(mu_small_vec* v, uint32_t elem_size,
                                        uint32_t inline_cap,
                                        void* inline_storage)
{
    MU_ASSERT(v != NULL);
    MU_ASSERT(elem_size > 0u);
    MU_ASSERT(inline_cap > 0u && inline_storage != NULL);

    v->elem_size = elem_size;
    v->count = 0;
    v->data = inline_storage;
    v->cap = inline_cap;
    return true;
}

MU_INLINE void mu_small_vec_destroy(mu_small_vec* v)
{
    if(v == NULL)
        return;
    if(v->data != v->inline_buf)
        MU_FREE(v->data);
    v->data = NULL;
    v->count = 0;
    v->cap = 0;
    v->elem_size = 0;
    MU_MEMSET(v->inline_buf, 0, sizeof(v->inline_buf));
}

MU_INLINE void mu_small_vec_clear(mu_small_vec* v)
{
    MU_ASSERT(v != NULL);
    v->count = 0;
}

MU_INLINE uint32_t mu_small_vec_count(const mu_small_vec* v) { return v->count; }
MU_INLINE uint32_t mu_small_vec_capacity(const mu_small_vec* v) { return v->cap; }
MU_INLINE bool mu_small_vec_empty(const mu_small_vec* v) { return v->count == 0u; }

MU_INLINE void* mu_small_vec_data(mu_small_vec* v) { return v->data; }
MU_INLINE const void* mu_small_vec_data_c(const mu_small_vec* v) { return v->data; }

MU_INLINE void* mu_small_vec_at(mu_small_vec* v, uint32_t i)
{
    MU_ASSERT(i < v->count);
    return (uint8_t*)v->data + (size_t)i * (size_t)v->elem_size;
}

MU_INLINE const void* mu_small_vec_at_c(const mu_small_vec* v, uint32_t i)
{
    MU_ASSERT(i < v->count);
    return (const uint8_t*)v->data + (size_t)i * (size_t)v->elem_size;
}

MU_INLINE void* mu_small_vec_front(mu_small_vec* v)
{
    MU_ASSERT(v->count > 0u);
    return v->data;
}

MU_INLINE void* mu_small_vec_back(mu_small_vec* v)
{
    MU_ASSERT(v->count > 0u);
    return (uint8_t*)v->data + (size_t)(v->count - 1u) * (size_t)v->elem_size;
}

/* Push a copy of `elem`. Returns false on OOM. */
MU_INLINE bool mu_small_vec_push(mu_small_vec* v, const void* elem)
{
    MU_ASSERT(v != NULL);
    MU_ASSERT(elem != NULL);

    if(v->count == v->cap)
    {
        uint32_t new_cap = v->cap ? v->cap * 2u : 8u;
        /* if currently on inline buffer, move to heap */
        uint8_t* old_data = (uint8_t*)v->data;
        bool was_inline = (old_data == v->inline_buf);
        size_t old_bytes = (size_t)v->cap * (size_t)v->elem_size;

        uint8_t* heap = (uint8_t*)MU_MALLOC((size_t)new_cap * (size_t)v->elem_size);
        if(!heap)
            return false;
        if(old_bytes > 0u)
            MU_MEMCPY(heap, old_data, old_bytes);
        v->data = heap;
        v->cap = new_cap;
        if(was_inline)
            MU_MEMSET(v->inline_buf, 0, sizeof(v->inline_buf));
    }
    MU_MEMCPY((uint8_t*)v->data + (size_t)v->count * (size_t)v->elem_size,
              elem, (size_t)v->elem_size);
    ++v->count;
    return true;
}

/* Pop the last element into `out` (may be NULL). Returns false if empty. */
MU_INLINE bool mu_small_vec_pop(mu_small_vec* v, void* out)
{
    MU_ASSERT(v != NULL);
    if(v->count == 0u)
        return false;
    --v->count;
    if(out)
        MU_MEMCPY(out, (uint8_t*)v->data + (size_t)v->count * (size_t)v->elem_size,
                  (size_t)v->elem_size);
    return true;
}

MU_END_EXTERN_C
