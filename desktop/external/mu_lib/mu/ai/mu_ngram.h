#pragma once

/*
===============================================================================

mu_ngram.h — N-gram count model with add-k smoothing.

Game AI Pro: "Implementing N-grams for Player Prediction, Procedural
Generation, and Stylized AI".

- Counts (n-1)-context -> next-token frequencies from a training sequence.
- Tokens are u32 ids (hashed words/events/player actions).
- Stores counts in open-addressing maps keyed by context; contexts are
  packed u64 (up to 8 tokens of 8 bits each, or fewer larger tokens).
- Predict: probability distribution over next tokens for a context; sample
  or pick max.

Flat arrays; allocation only at train/predict time via the count maps.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

#ifndef MU_NGRAM_MAX_ORDER
#  define MU_NGRAM_MAX_ORDER 8u   /* max tokens in a context */
#endif

/* Pack a context (array of u32 tokens) into a u64 key. Tokens beyond
 * MU_NGRAM_MAX_ORDER are truncated; each token takes up to 8 bits, so this
 * works well for small token alphabets. For larger alphabets the caller
 * should hash tokens down to 8 bits before calling. */
MU_INLINE uint64_t mu_ngram_pack_context(const uint32_t* tokens,
                                         uint32_t count)
{
    uint64_t key = 0;
    uint32_t n = count < MU_NGRAM_MAX_ORDER ? count : MU_NGRAM_MAX_ORDER;
    for(uint32_t i = 0; i < n; ++i)
    {
        uint64_t t = (uint64_t)(tokens[i] & 0xFFu);
        key |= t << (i * 8u);
    }
    return key;
}

typedef struct mu_ngram_count_entry
{
    uint64_t context;
    uint32_t token;
    uint32_t count;
    uint8_t  used;
} mu_ngram_count_entry;

typedef struct mu_ngram
{
    uint32_t order;             /* N: predicts token n given n-1 context    */
    uint32_t vocab_size;        /* number of distinct tokens seen           */

    /* count map: context -> list of (token, count) entries stored inline */
    mu_ngram_count_entry* entries;
    uint32_t              entry_count;
    uint32_t              entry_cap;

    /* total counts per context (for normalization) */
    uint64_t*   context_keys;
    uint32_t*   context_totals;
    uint32_t    context_count;
    uint32_t    context_cap;
} mu_ngram;

MU_INLINE bool mu_ngram_init(mu_ngram* n, uint32_t order)
{
    MU_ASSERT(n != NULL);
    MU_ASSERT(order > 0u && order <= MU_NGRAM_MAX_ORDER);

    n->order = order;
    n->vocab_size = 0;

    n->entries = (mu_ngram_count_entry*)MU_CALLOC(1024u,
        sizeof(mu_ngram_count_entry));
    if(!n->entries)
        return false;
    n->entry_count = 0;
    n->entry_cap = 1024u;

    n->context_keys = (uint64_t*)MU_CALLOC(256u, sizeof(uint64_t));
    n->context_totals = (uint32_t*)MU_CALLOC(256u, sizeof(uint32_t));
    if(!n->context_keys || !n->context_totals)
    {
        MU_FREE(n->entries);
        MU_FREE(n->context_keys);
        MU_FREE(n->context_totals);
        n->entries = NULL;
        n->context_keys = NULL;
        n->context_totals = NULL;
        return false;
    }
    n->context_count = 0;
    n->context_cap = 256u;
    return true;
}

MU_INLINE void mu_ngram_destroy(mu_ngram* n)
{
    if(n == NULL)
        return;
    MU_FREE(n->entries);
    MU_FREE(n->context_keys);
    MU_FREE(n->context_totals);
    n->entries = NULL;
    n->context_keys = NULL;
    n->context_totals = NULL;
    n->entry_count = n->entry_cap = 0;
    n->context_count = n->context_cap = 0;
    n->order = 0;
    n->vocab_size = 0;
}

MU_INLINE void mu_ngram_clear(mu_ngram* n)
{
    MU_ASSERT(n != NULL);
    n->entry_count = 0;
    n->context_count = 0;
    n->vocab_size = 0;
}

/* ---------- training ---------- */

/* Observe a sequence of tokens; update all (order-1)-context -> next counts.
 * Tokens are u32 ids; the model tracks the max id as vocab_size. */
MU_INLINE void mu_ngram_train(mu_ngram* n, const uint32_t* tokens,
                              uint32_t count)
{
    MU_ASSERT(n != NULL);
    MU_ASSERT(tokens != NULL);
    if(count < n->order)
        return;

    uint32_t ctx_len = n->order - 1u;

    for(uint32_t i = 0; i + n->order <= count; ++i)
    {
        /* context = tokens[i .. i+ctx_len) */
        uint64_t ckey = mu_ngram_pack_context(tokens + i, ctx_len);
        uint32_t next = tokens[i + ctx_len];

        /* track vocab */
        if(next + 1u > n->vocab_size)
            n->vocab_size = next + 1u;

        /* find or create context total */
        uint32_t cidx = MU_INVALID_INDEX;
        for(uint32_t c = 0; c < n->context_count; ++c)
        {
            if(n->context_keys[c] == ckey)
            {
                cidx = c;
                break;
            }
        }
        if(cidx == MU_INVALID_INDEX)
        {
            if(n->context_count >= n->context_cap)
            {
                uint32_t nc = n->context_cap * 2u;
                uint64_t* ck = (uint64_t*)MU_REALLOC(
                    n->context_keys, sizeof(uint64_t) * nc);
                uint32_t* ct = (uint32_t*)MU_REALLOC(
                    n->context_totals, sizeof(uint32_t) * nc);
                if(!ck || !ct)
                    return;
                n->context_keys = ck;
                n->context_totals = ct;
                n->context_cap = nc;
            }
            cidx = n->context_count++;
            n->context_keys[cidx] = ckey;
            n->context_totals[cidx] = 0;
        }
        ++n->context_totals[cidx];

        /* find or create (context, token) entry */
        uint32_t eidx = MU_INVALID_INDEX;
        for(uint32_t e = 0; e < n->entry_count; ++e)
        {
            if(n->entries[e].used && n->entries[e].context == ckey &&
               n->entries[e].token == next)
            {
                eidx = e;
                break;
            }
        }
        if(eidx == MU_INVALID_INDEX)
        {
            if(n->entry_count >= n->entry_cap)
            {
                uint32_t nc = n->entry_cap * 2u;
                mu_ngram_count_entry* ne = (mu_ngram_count_entry*)MU_REALLOC(
                    n->entries, sizeof(mu_ngram_count_entry) * nc);
                if(!ne)
                    return;
                n->entries = ne;
                n->entry_cap = nc;
            }
            eidx = n->entry_count++;
            n->entries[eidx].used = 1u;
            n->entries[eidx].context = ckey;
            n->entries[eidx].token = next;
            n->entries[eidx].count = 0;
        }
        ++n->entries[eidx].count;
    }
}

/* ---------- prediction ---------- */

/* Total count for a context. */
static uint32_t mu_ngram_context_total(const mu_ngram* n, uint64_t context)
{
    for(uint32_t c = 0; c < n->context_count; ++c)
    {
        if(n->context_keys[c] == context)
            return n->context_totals[c];
    }
    return 0u;
}

/* Count of `token` following `context`. */
static uint32_t mu_ngram_count(const mu_ngram* n, uint64_t context,
                               uint32_t token)
{
    for(uint32_t e = 0; e < n->entry_count; ++e)
    {
        if(n->entries[e].used && n->entries[e].context == context &&
           n->entries[e].token == token)
            return n->entries[e].count;
    }
    return 0u;
}

/* Probability of `token` given `context` with add-k smoothing (k=1 by
 * default). Returns [0,1]. */
MU_INLINE float mu_ngram_prob(const mu_ngram* n, uint64_t context,
                              uint32_t token, float k)
{
    MU_ASSERT(n != NULL);
    if(n->vocab_size == 0u)
        return 0.0f;
    uint32_t c = mu_ngram_count(n, context, token);
    uint32_t total = mu_ngram_context_total(n, context);
    float num = (float)c + k;
    float den = (float)total + k * (float)n->vocab_size;
    if(den <= 0.0f)
        return 1.0f / (float)n->vocab_size;
    return num / den;
}

/* Copy the full distribution over [0, vocab_size) for `context` into
 * `out` (caller-owned, size >= vocab_size). Returns vocab_size. */
MU_INLINE uint32_t mu_ngram_distribution(const mu_ngram* n, uint64_t context,
                                         float* out, float k)
{
    MU_ASSERT(n != NULL);
    MU_ASSERT(out != NULL);
    uint32_t v = n->vocab_size;
    if(v == 0u)
        return 0u;
    float total_count = (float)mu_ngram_context_total(n, context);
    float den = total_count + k * (float)v;
    if(den <= 0.0f)
    {
        float uniform = 1.0f / (float)v;
        for(uint32_t i = 0; i < v; ++i)
            out[i] = uniform;
        return v;
    }
    for(uint32_t i = 0; i < v; ++i)
    {
        uint32_t c = mu_ngram_count(n, context, i);
        out[i] = ((float)c + k) / den;
    }
    return v;
}

/* Sample a token from the distribution using `u` in [0,1). Returns the token
 * id. Requires mu_ngram_distribution to have been called first into `out`. */
MU_INLINE uint32_t mu_ngram_sample(const float* dist, uint32_t vocab_size,
                                   float u)
{
    MU_ASSERT(dist != NULL);
    MU_ASSERT(vocab_size > 0u);
    if(u < 0.0f) u = 0.0f;
    if(u > 1.0f) u = 1.0f;
    float acc = 0.0f;
    for(uint32_t i = 0; i < vocab_size; ++i)
    {
        acc += dist[i];
        if(u < acc)
            return i;
    }
    return vocab_size - 1u;
}

/* Pack the last (order-1) tokens of `recent` as the prediction context. */
MU_INLINE uint64_t mu_ngram_recent_context(const mu_ngram* n,
                                           const uint32_t* recent,
                                           uint32_t count)
{
    uint32_t ctx_len = n->order - 1u;
    const uint32_t* start = recent;
    if(count > ctx_len)
        start = recent + (count - ctx_len);
    uint32_t available = count > ctx_len ? ctx_len : count;
    /* right-align: prepend zeros if fewer than ctx_len */
    uint32_t padded[MU_NGRAM_MAX_ORDER];
    for(uint32_t i = 0; i < ctx_len; ++i)
        padded[i] = 0u;
    for(uint32_t i = 0; i < available; ++i)
        padded[ctx_len - available + i] = start[i];
    return mu_ngram_pack_context(padded, ctx_len);
}

MU_END_EXTERN_C
