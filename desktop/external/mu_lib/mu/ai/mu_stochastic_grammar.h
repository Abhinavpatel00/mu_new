#pragma once

/*
===============================================================================

mu_stochastic_grammar.h — stochastic (probabilistic) grammar / PCFG.

Game AI Pro: "Stochastic Grammars for Procedural Content", "Dialogue
Generation with Probabilistic Context-Free Grammars", "Behavior Tree
Synthesis via Grammar".

- A set of non-terminal symbols, each with one or more productions.
- A production: lhs non-terminal -> sequence of symbol IDs (terminals or
  non-terminals), with a probability. Probabilities for all productions of
  the same lhs should sum to 1 (normalize() enforces this).
- Derivation: start from the start symbol; repeatedly expand a non-terminal
  by probabilistically picking one of its productions (weighted by
  probability) until only terminals remain.
- Depth bound prevents runaway recursion from recursive grammars.
- A simple xorshift32 RNG is used internally, seeded by the caller.

All flat arrays; no per-derivation allocation beyond a scratch buffer.

===============================================================================
*/

#include "mu_common.h"

#include <stdlib.h>
#include <string.h>

MU_BEGIN_EXTERN_C

/* One production: lhs non-terminal -> rhs symbol sequence, with weight. */
typedef struct mu_grammar_production
{
    uint32_t* rhs;         /* rhs symbol IDs (terminals or non-terminals) */
    uint32_t  rhs_count;
    uint32_t  rhs_cap;
    float     weight;      /* raw weight; normalized per lhs */
    float     cumulative;  /* cumulative weight for selection */
} mu_grammar_production;

/* One non-terminal symbol and its productions. */
typedef struct mu_grammar_symbol
{
    uint32_t                  id;          /* non-terminal symbol ID */
    mu_grammar_production*    prods;       /* its productions */
    uint32_t                  prod_count;
    uint32_t                  prod_cap;
    float                     total_weight;/* sum of prod weights */
} mu_grammar_symbol;

typedef struct mu_grammar
{
    mu_grammar_symbol* symbols;   /* all non-terminals */
    uint32_t           symbol_count;
    uint32_t           symbol_cap;
    uint32_t           start_symbol;   /* symbol ID derivation begins at */
    bool               dirty;          /* weights changed; re-normalize */
} mu_grammar;

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_grammar_init(mu_grammar* g)
{
    MU_ASSERT(g != NULL);
    g->symbols = (mu_grammar_symbol*)MU_CALLOC(16u, sizeof(mu_grammar_symbol));
    if(!g->symbols)
        return false;
    g->symbol_count = 0;
    g->symbol_cap = 16u;
    g->start_symbol = 0u;
    g->dirty = false;
    return true;
}

MU_INLINE void mu_grammar_destroy(mu_grammar* g)
{
    if(g == NULL)
        return;
    for(uint32_t i = 0; i < g->symbol_count; ++i)
    {
        for(uint32_t p = 0; p < g->symbols[i].prod_count; ++p)
            MU_FREE(g->symbols[i].prods[p].rhs);
        MU_FREE(g->symbols[i].prods);
    }
    MU_FREE(g->symbols);
    g->symbols = NULL;
    g->symbol_count = g->symbol_cap = 0;
    g->start_symbol = 0u;
    g->dirty = false;
}

/* ---------- symbols / productions ---------- */

/* Find the symbol index for `id`, or MU_INVALID_INDEX if absent. */
MU_INLINE uint32_t mu_grammar_find_symbol(const mu_grammar* g, uint32_t id)
{
    for(uint32_t i = 0; i < g->symbol_count; ++i)
        if(g->symbols[i].id == id)
            return i;
    return MU_INVALID_INDEX;
}

/* Ensure a non-terminal symbol exists (creating it if needed). Returns its
 * index in the symbols array. */
static uint32_t mu_grammar_symbol_index(mu_grammar* g, uint32_t id)
{
    uint32_t idx = mu_grammar_find_symbol(g, id);
    if(idx != MU_INVALID_INDEX)
        return idx;
    if(g->symbol_count == g->symbol_cap)
    {
        uint32_t nc = g->symbol_cap * 2u;
        mu_grammar_symbol* ns = (mu_grammar_symbol*)MU_REALLOC(
            g->symbols, sizeof(mu_grammar_symbol) * nc);
        if(!ns)
            return MU_INVALID_INDEX;
        g->symbols = ns;
        g->symbol_cap = nc;
    }
    idx = g->symbol_count++;
    g->symbols[idx].id = id;
    g->symbols[idx].prods = NULL;
    g->symbols[idx].prod_count = 0;
    g->symbols[idx].prod_cap = 0;
    g->symbols[idx].total_weight = 0.0f;
    g->dirty = true;
    return idx;
}

/* Set the start symbol (created if absent). */
MU_INLINE void mu_grammar_set_start(mu_grammar* g, uint32_t id)
{
    MU_ASSERT(g != NULL);
    uint32_t idx = mu_grammar_symbol_index(g, id);
    if(idx != MU_INVALID_INDEX)
        g->start_symbol = id;
}

/* Add a production: `lhs` -> `rhs[0..rhs_count)` with `weight`.
 * Returns false on allocation failure. Weights for a given lhs are
 * normalized by mu_grammar_normalize() (or lazily at derive time). */
MU_INLINE bool mu_grammar_add_production(mu_grammar* g, uint32_t lhs,
                                         const uint32_t* rhs,
                                         uint32_t rhs_count, float weight)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(rhs != NULL || rhs_count == 0u);
    MU_ASSERT(weight >= 0.0f);
    uint32_t li = mu_grammar_symbol_index(g, lhs);
    if(li == MU_INVALID_INDEX)
        return false;
    mu_grammar_symbol* s = &g->symbols[li];
    if(s->prod_count == s->prod_cap)
    {
        uint32_t nc = s->prod_cap ? s->prod_cap * 2u : 4u;
        mu_grammar_production* np = (mu_grammar_production*)MU_REALLOC(
            s->prods, sizeof(mu_grammar_production) * nc);
        if(!np)
            return false;
        s->prods = np;
        s->prod_cap = nc;
    }
    uint32_t pi = s->prod_count++;
    s->prods[pi].rhs = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * rhs_count);
    if(rhs_count > 0u && s->prods[pi].rhs == NULL)
        return false;
    if(rhs_count > 0u)
        MU_MEMCPY(s->prods[pi].rhs, rhs, sizeof(uint32_t) * rhs_count);
    s->prods[pi].rhs_count = rhs_count;
    s->prods[pi].rhs_cap = rhs_count;
    s->prods[pi].weight = weight;
    s->prods[pi].cumulative = 0.0f;
    s->total_weight += weight;
    g->dirty = true;
    return true;
}

/* Normalize weights so that, for each non-terminal, the production weights
 * sum to 1, and compute cumulative weights for selection. Call after adding
 * all productions (derive() also does this lazily). */
MU_INLINE void mu_grammar_normalize(mu_grammar* g)
{
    MU_ASSERT(g != NULL);
    if(!g->dirty)
        return;
    for(uint32_t i = 0; i < g->symbol_count; ++i)
    {
        mu_grammar_symbol* s = &g->symbols[i];
        float total = s->total_weight;
        if(total <= 0.0f)
            total = 1.0f; /* avoid div by zero; uniform-ish */
        float cum = 0.0f;
        for(uint32_t p = 0; p < s->prod_count; ++p)
        {
            s->prods[p].weight /= total;
            cum += s->prods[p].weight;
            s->prods[p].cumulative = cum;
        }
        /* clamp last cumulative to exactly 1 to avoid float drift */
        if(s->prod_count > 0u)
            s->prods[s->prod_count - 1u].cumulative = 1.0f;
    }
    g->dirty = false;
}

/* ---------- tiny xorshift32 RNG (no external dependency) ---------- */

typedef struct mu_grammar_rng
{
    uint32_t state;
} mu_grammar_rng;

MU_INLINE void mu_grammar_rng_seed(mu_grammar_rng* rng, uint32_t seed)
{
    rng->state = seed ? seed : 0x9E3779B9u;
}

MU_INLINE uint32_t mu_grammar_rng_next(mu_grammar_rng* rng)
{
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

MU_INLINE float mu_grammar_rng_float(mu_grammar_rng* rng)
{
    /* [0, 1) */
    return (mu_grammar_rng_next(rng) & 0x00FFFFFFu) / 16777216.0f;
}

/* ---------- derivation ---------- */

/* Derive a terminal symbol sequence from the start symbol.
 *
 * Recursively expand non-terminals by weighted random choice until only
 * terminals (symbols with no productions) remain, up to `max_depth`
 * expansion steps. Terminals are appended to `out`; *out_count is the
 * number written (capped at `out_cap`). Returns the number of terminals
 * emitted, or 0 if derivation failed (e.g. depth exceeded, missing
 * production, or output full).
 *
 * `rng` provides the randomness; seed it via mu_grammar_rng_seed(). */
/* Recursive derivation helper (static, single TU per include). */
static uint32_t mu_grammar_derive_rec(mu_grammar* g, mu_grammar_rng* rng,
                                      uint32_t sym, uint32_t depth,
                                      uint32_t max_depth, uint32_t* out,
                                      uint32_t* out_i, uint32_t out_cap)
{
    if(*out_i >= out_cap)
        return *out_i; /* output full */
    if(depth > max_depth)
        return *out_i; /* depth budget exhausted: stop emitting */

    uint32_t idx = mu_grammar_find_symbol(g, sym);
    if(idx == MU_INVALID_INDEX)
    {
        /* Symbol is a terminal (no productions defined): emit it. */
        out[(*out_i)++] = sym;
        return *out_i;
    }

    mu_grammar_symbol* s = &g->symbols[idx];
    if(s->prod_count == 0u)
    {
        /* Non-terminal declared but no productions: treat as terminal. */
        out[(*out_i)++] = sym;
        return *out_i;
    }

    /* Weighted random choice of a production. */
    float r = mu_grammar_rng_float(rng);
    uint32_t pick = s->prod_count - 1u; /* fallback: last */
    for(uint32_t p = 0; p < s->prod_count; ++p)
    {
        if(r < s->prods[p].cumulative)
        {
            pick = p;
            break;
        }
    }

    const mu_grammar_production* pr = &s->prods[pick];
    /* Expand rhs symbols left-to-right. */
    for(uint32_t k = 0; k < pr->rhs_count; ++k)
    {
        if(*out_i >= out_cap)
            break;
        mu_grammar_derive_rec(g, rng, pr->rhs[k], depth + 1u, max_depth,
                              out, out_i, out_cap);
    }
    return *out_i;
}

/* Public derivation entry. */
MU_INLINE uint32_t mu_grammar_derive(mu_grammar* g, mu_grammar_rng* rng,
                                     uint32_t max_depth,
                                     uint32_t* out, uint32_t out_cap)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(rng != NULL);
    MU_ASSERT(out != NULL || out_cap == 0u);

    mu_grammar_normalize(g);
    if(mu_grammar_find_symbol(g, g->start_symbol) == MU_INVALID_INDEX)
    {
        /* start symbol unknown: nothing to derive */
        return 0u;
    }

    uint32_t out_i = 0u;
    mu_grammar_derive_rec(g, rng, g->start_symbol, 0u, max_depth,
                          out, &out_i, out_cap);
    return out_i;
}

MU_END_EXTERN_C
