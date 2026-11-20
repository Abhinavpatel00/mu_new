#pragma once

/*
===============================================================================

mu_blackboard.h — typed key→value store for AI systems (behavior trees,
utility scoring, FSMs, planners).

Flat, index-based storage: keys are u32 ids (hashed strings or user tags),
values live in a dense slot array with a per-slot type tag. Lookup is via an
open-addressing map from key → slot index. No raw pointers cross tick
boundaries; callers read values by key each tick.

Value types: mu_bb_float, mu_bb_int, mu_bb_bool, mu_bb_handle (u32), mu_bb_str
(string id into a caller-owned string table — the board only stores the u32).

Canonical surface: init / init_static / destroy / clear, set / get by key,
iteration over all slots.

===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

typedef enum mu_bb_type
{
    MU_BB_FLOAT,
    MU_BB_INT,
    MU_BB_BOOL,
    MU_BB_HANDLE,
    MU_BB_STR,
} mu_bb_type;

typedef union mu_bb_value
{
    float    f;
    int32_t  i;
    bool     b;
    uint32_t h;      /* handle / string id */
} mu_bb_value;

typedef struct mu_bb_slot
{
    uint32_t key;
    mu_bb_value value;
    mu_bb_type type;
    uint8_t  used;   /* 1 = slot in use */
} mu_bb_slot;

typedef struct mu_blackboard
{
    mu_bb_slot* slots;      /* dense slot array */
    uint32_t    slot_count;
    uint32_t    slot_cap;

    /* open-addressing map: key -> slot index */
    uint64_t*   map_keys;
    int32_t*    map_vals;   /* slot index, -1 = empty */
    uint32_t    map_cap;
    uint32_t    map_count;
} mu_blackboard;

MU_INLINE uint64_t mu_bb_hash_key(uint32_t key)
{
    return (uint64_t)key * 0x9E3779B97F4A7C15ull;
}

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_blackboard_init(mu_blackboard* b, uint32_t max_entries)
{
    MU_ASSERT(b != NULL);
    MU_ASSERT(max_entries > 0u);

    b->slots = (mu_bb_slot*)MU_MALLOC(sizeof(mu_bb_slot) * max_entries);
    if(!b->slots)
        return false;

    /* map capacity: next pow2 >= max_entries * 2 (load factor < 0.5) */
    uint32_t cap = 8u;
    while(cap < max_entries * 2u)
        cap <<= 1;

    b->map_keys = (uint64_t*)MU_MALLOC(sizeof(uint64_t) * cap);
    b->map_vals = (int32_t*)MU_MALLOC(sizeof(int32_t) * cap);

    bool ok = b->map_keys && b->map_vals;
    if(!ok)
    {
        MU_FREE(b->slots);
        MU_FREE(b->map_keys);
        MU_FREE(b->map_vals);
        b->slots = NULL;
        b->map_keys = NULL;
        b->map_vals = NULL;
        return false;
    }

    MU_MEMSET(b->map_vals, 0xFF, sizeof(int32_t) * cap);
    MU_MEMSET(b->slots, 0, sizeof(mu_bb_slot) * max_entries);

    b->slot_count = 0;
    b->slot_cap = max_entries;
    b->map_cap = cap;
    b->map_count = 0;
    return true;
}

/* Static variant: caller supplies all storage, zero malloc. */
MU_INLINE bool mu_blackboard_init_static(mu_blackboard* b, uint32_t max_entries,
                                         mu_bb_slot* slots,
                                         uint64_t* map_keys, int32_t* map_vals,
                                         uint32_t map_cap)
{
    MU_ASSERT(b != NULL);
    MU_ASSERT(max_entries > 0u && map_cap > 0u);

    b->slots = slots;
    b->map_keys = map_keys;
    b->map_vals = map_vals;
    b->slot_count = 0;
    b->slot_cap = max_entries;
    b->map_cap = map_cap;
    b->map_count = 0;
    MU_MEMSET(b->map_vals, 0xFF, sizeof(int32_t) * map_cap);
    MU_MEMSET(b->slots, 0, sizeof(mu_bb_slot) * max_entries);
    return true;
}

MU_INLINE void mu_blackboard_destroy(mu_blackboard* b)
{
    if(b == NULL)
        return;
    MU_FREE(b->slots);
    MU_FREE(b->map_keys);
    MU_FREE(b->map_vals);
    b->slots = NULL;
    b->map_keys = NULL;
    b->map_vals = NULL;
    b->slot_count = b->slot_cap = 0;
    b->map_cap = b->map_count = 0;
}

MU_INLINE void mu_blackboard_clear(mu_blackboard* b)
{
    MU_ASSERT(b != NULL);
    b->slot_count = 0;
    b->map_count = 0;
    MU_MEMSET(b->map_vals, 0xFF, sizeof(int32_t) * b->map_cap);
    MU_MEMSET(b->slots, 0, sizeof(mu_bb_slot) * b->slot_cap);
}

MU_INLINE uint32_t mu_blackboard_count(const mu_blackboard* b) { return b->slot_count; }

/* ---------- internal map helpers ---------- */

static int32_t mu_bb_find_slot(const mu_blackboard* b, uint32_t key)
{
    uint64_t hk = mu_bb_hash_key(key);
    uint32_t cap = b->map_cap;
    uint32_t mask = cap - 1u;
    uint32_t h = (uint32_t)(hk >> 32);
    for(uint32_t probe = 0; probe < cap; ++probe)
    {
        uint32_t idx = (h + probe) & mask;
        if(b->map_vals[idx] < 0)
            return -1;
        if(b->map_keys[idx] == hk)
            return b->map_vals[idx];
    }
    return -1;
}

static int32_t mu_bb_alloc_slot(mu_blackboard* b, uint32_t key)
{
    if(b->slot_count >= b->slot_cap)
        return -1;
    uint32_t slot = b->slot_count++;
    b->slots[slot].key = key;
    b->slots[slot].used = 1u;
    return (int32_t)slot;
}

static void mu_bb_map_insert(mu_blackboard* b, uint32_t key, int32_t slot)
{
    uint64_t hk = mu_bb_hash_key(key);
    uint32_t cap = b->map_cap;
    uint32_t mask = cap - 1u;
    uint32_t h = (uint32_t)(hk >> 32);
    for(uint32_t probe = 0;; ++probe)
    {
        uint32_t idx = (h + probe) & mask;
        if(b->map_vals[idx] < 0 || b->map_keys[idx] == hk)
        {
            b->map_keys[idx] = hk;
            b->map_vals[idx] = slot;
            b->map_count++;
            return;
        }
    }
}

/* ---------- set / get ---------- */

MU_INLINE bool mu_blackboard_set_float(mu_blackboard* b, uint32_t key, float v)
{
    MU_ASSERT(b != NULL);
    int32_t slot = mu_bb_find_slot(b, key);
    if(slot < 0)
    {
        slot = mu_bb_alloc_slot(b, key);
        if(slot < 0)
            return false;
        mu_bb_map_insert(b, key, slot);
    }
    b->slots[(uint32_t)slot].type = MU_BB_FLOAT;
    b->slots[(uint32_t)slot].value.f = v;
    return true;
}

MU_INLINE bool mu_blackboard_set_int(mu_blackboard* b, uint32_t key, int32_t v)
{
    MU_ASSERT(b != NULL);
    int32_t slot = mu_bb_find_slot(b, key);
    if(slot < 0)
    {
        slot = mu_bb_alloc_slot(b, key);
        if(slot < 0)
            return false;
        mu_bb_map_insert(b, key, slot);
    }
    b->slots[(uint32_t)slot].type = MU_BB_INT;
    b->slots[(uint32_t)slot].value.i = v;
    return true;
}

MU_INLINE bool mu_blackboard_set_bool(mu_blackboard* b, uint32_t key, bool v)
{
    MU_ASSERT(b != NULL);
    int32_t slot = mu_bb_find_slot(b, key);
    if(slot < 0)
    {
        slot = mu_bb_alloc_slot(b, key);
        if(slot < 0)
            return false;
        mu_bb_map_insert(b, key, slot);
    }
    b->slots[(uint32_t)slot].type = MU_BB_BOOL;
    b->slots[(uint32_t)slot].value.b = v;
    return true;
}

MU_INLINE bool mu_blackboard_set_handle(mu_blackboard* b, uint32_t key, uint32_t v)
{
    MU_ASSERT(b != NULL);
    int32_t slot = mu_bb_find_slot(b, key);
    if(slot < 0)
    {
        slot = mu_bb_alloc_slot(b, key);
        if(slot < 0)
            return false;
        mu_bb_map_insert(b, key, slot);
    }
    b->slots[(uint32_t)slot].type = MU_BB_HANDLE;
    b->slots[(uint32_t)slot].value.h = v;
    return true;
}

MU_INLINE bool mu_blackboard_set_str(mu_blackboard* b, uint32_t key, uint32_t str_id)
{
    MU_ASSERT(b != NULL);
    int32_t slot = mu_bb_find_slot(b, key);
    if(slot < 0)
    {
        slot = mu_bb_alloc_slot(b, key);
        if(slot < 0)
            return false;
        mu_bb_map_insert(b, key, slot);
    }
    b->slots[(uint32_t)slot].type = MU_BB_STR;
    b->slots[(uint32_t)slot].value.h = str_id;
    return true;
}

/* Get by key with type check. Returns false if missing or type mismatch. */
MU_INLINE bool mu_blackboard_get_float(const mu_blackboard* b, uint32_t key,
                                       float* out)
{
    MU_ASSERT(b != NULL);
    int32_t slot = mu_bb_find_slot(b, key);
    if(slot < 0)
        return false;
    const mu_bb_slot* s = &b->slots[(uint32_t)slot];
    if(s->type != MU_BB_FLOAT)
        return false;
    *out = s->value.f;
    return true;
}

MU_INLINE bool mu_blackboard_get_int(const mu_blackboard* b, uint32_t key,
                                     int32_t* out)
{
    MU_ASSERT(b != NULL);
    int32_t slot = mu_bb_find_slot(b, key);
    if(slot < 0)
        return false;
    const mu_bb_slot* s = &b->slots[(uint32_t)slot];
    if(s->type != MU_BB_INT)
        return false;
    *out = s->value.i;
    return true;
}

MU_INLINE bool mu_blackboard_get_bool(const mu_blackboard* b, uint32_t key,
                                      bool* out)
{
    MU_ASSERT(b != NULL);
    int32_t slot = mu_bb_find_slot(b, key);
    if(slot < 0)
        return false;
    const mu_bb_slot* s = &b->slots[(uint32_t)slot];
    if(s->type != MU_BB_BOOL)
        return false;
    *out = s->value.b;
    return true;
}

MU_INLINE bool mu_blackboard_get_handle(const mu_blackboard* b, uint32_t key,
                                        uint32_t* out)
{
    MU_ASSERT(b != NULL);
    int32_t slot = mu_bb_find_slot(b, key);
    if(slot < 0)
        return false;
    const mu_bb_slot* s = &b->slots[(uint32_t)slot];
    if(s->type != MU_BB_HANDLE && s->type != MU_BB_STR)
        return false;
    *out = s->value.h;
    return true;
}

MU_INLINE bool mu_blackboard_has(const mu_blackboard* b, uint32_t key)
{
    MU_ASSERT(b != NULL);
    return mu_bb_find_slot(b, key) >= 0;
}

/* ---------- iteration ---------- */

/* Span-like iteration: fills out_keys/out_types with the used slots in
 * insertion order. Returns the number written (clamped to out_cap). */
MU_INLINE uint32_t mu_blackboard_foreach(const mu_blackboard* b,
                                         uint32_t* out_keys,
                                         mu_bb_type* out_types,
                                         uint32_t out_cap)
{
    MU_ASSERT(b != NULL);
    uint32_t n = 0;
    for(uint32_t i = 0; i < b->slot_count && n < out_cap; ++i)
    {
        if(b->slots[i].used)
        {
            if(out_keys)   out_keys[n] = b->slots[i].key;
            if(out_types)  out_types[n] = b->slots[i].type;
            ++n;
        }
    }
    return n;
}

MU_END_EXTERN_C
