#pragma once

/*
===============================================================================

mu_production.h — forward-chaining production rule system.

Game AI Pro: "Production Systems: New Techniques in AAA Games",
"Production Rules Implementation in 1849".

- Rules: a set of conditions (world-state predicates) and an action.
- Working memory: flat set of u32 facts (tokens).
- Match: a rule fires when all its conditions are present in working memory.
- Conflict resolution: fire one rule per step (first match), then retract its
  conditions and assert its actions (add/delete semantics).
- Iterate until no rule fires or a step cap is reached.

All flat arrays; no per-rule allocation after init.

===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

/* ------------------------------------------------------------------
 * User interface: test a fact predicate, and execute an action.
 * ------------------------------------------------------------------ */

/* Return true if `fact` holds in `world`. */
typedef bool (*mu_prod_condition)(const void* world, uint32_t fact);

/* Perform `action` (a fact token) on `world`. */
typedef void (*mu_prod_action)(void* world, uint32_t fact);

typedef struct mu_prod_iface
{
    mu_prod_condition condition;
    mu_prod_action    action;
    void*             ctx;
} mu_prod_iface;

/* ------------------------------------------------------------------
 * Rule: conditions[] must all be present; actions[] are asserted,
 * conditions are retracted (delete semantics) when the rule fires.
 * ------------------------------------------------------------------ */

#define MU_PROD_MAX_CONDITIONS 8u
#define MU_PROD_MAX_ACTIONS 8u

typedef struct mu_prod_rule
{
    uint32_t id;
    uint32_t conditions[MU_PROD_MAX_CONDITIONS];
    uint32_t cond_count;
    uint32_t actions[MU_PROD_MAX_ACTIONS];
    uint32_t act_count;
    uint8_t  enabled;
} mu_prod_rule;

typedef struct mu_production
{
    const mu_prod_iface* iface;

    mu_prod_rule* rules;
    uint32_t      rule_count;
    uint32_t      rule_cap;

    /* working memory: set of u32 facts (open addressing for membership) */
    uint32_t*   facts;
    uint32_t    fact_count;
    uint32_t    fact_cap;
} mu_production;

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_production_init(mu_production* p, const mu_prod_iface* iface,
                                  uint32_t max_rules, uint32_t max_facts)
{
    MU_ASSERT(p != NULL);
    MU_ASSERT(iface != NULL);
    MU_ASSERT(max_rules > 0u && max_facts > 0u);

    p->iface = iface;

    p->rules = (mu_prod_rule*)MU_CALLOC(max_rules, sizeof(mu_prod_rule));
    if(!p->rules)
        return false;
    p->rule_count = 0;
    p->rule_cap = max_rules;

    /* fact membership via open addressing; cap pow2 */
    uint32_t cap = 16u;
    while(cap < max_facts * 2u)
        cap <<= 1;
    p->facts = (uint32_t*)MU_CALLOC(cap, sizeof(uint32_t));
    if(!p->facts)
    {
        MU_FREE(p->rules);
        p->rules = NULL;
        return false;
    }
    p->fact_count = 0;
    p->fact_cap = cap;
    return true;
}

MU_INLINE void mu_production_destroy(mu_production* p)
{
    if(p == NULL)
        return;
    MU_FREE(p->rules);
    MU_FREE(p->facts);
    p->rules = NULL;
    p->facts = NULL;
    p->rule_count = p->rule_cap = 0;
    p->fact_count = p->fact_cap = 0;
}

MU_INLINE void mu_production_clear(mu_production* p)
{
    MU_ASSERT(p != NULL);
    p->rule_count = 0;
    p->fact_count = 0;
}

MU_INLINE uint32_t mu_production_rule_count(const mu_production* p)
{
    return p->rule_count;
}

/* ---------- rules ---------- */

MU_INLINE bool mu_production_add_rule(mu_production* p, const mu_prod_rule* r)
{
    MU_ASSERT(p != NULL);
    MU_ASSERT(r != NULL);
    if(p->rule_count >= p->rule_cap)
        return false;
    p->rules[p->rule_count++] = *r;
    return true;
}

MU_INLINE void mu_production_enable_rule(mu_production* p, uint32_t index,
                                         bool enabled)
{
    MU_ASSERT(p != NULL);
    if(index < p->rule_count)
        p->rules[index].enabled = enabled ? 1u : 0u;
}

/* ---------- working memory ---------- */

static uint32_t mu_prod_hash_fact(uint32_t f)
{
    return f * 0x9E3779B9u;
}

static bool mu_prod_has_fact(const mu_production* p, uint32_t fact)
{
    uint32_t cap = p->fact_cap;
    uint32_t mask = cap - 1u;
    uint32_t h = mu_prod_hash_fact(fact) & mask;
    for(uint32_t probe = 0; probe < cap; ++probe)
    {
        uint32_t idx = (h + probe) & mask;
        if(p->facts[idx] == 0u)      /* 0 reserved as empty */
            return false;
        if(p->facts[idx] == fact)
            return true;
    }
    return false;
}

static bool mu_prod_add_fact(mu_production* p, uint32_t fact)
{
    MU_ASSERT(fact != 0u); /* 0 reserved */
    if(mu_prod_has_fact(p, fact))
        return true;
    if(p->fact_count * 2u >= p->fact_cap)
        return false; /* full */
    uint32_t cap = p->fact_cap;
    uint32_t mask = cap - 1u;
    uint32_t h = mu_prod_hash_fact(fact) & mask;
    for(uint32_t probe = 0;; ++probe)
    {
        uint32_t idx = (h + probe) & mask;
        if(p->facts[idx] == 0u)
        {
            p->facts[idx] = fact;
            ++p->fact_count;
            return true;
        }
    }
}

static void mu_prod_del_fact(mu_production* p, uint32_t fact)
{
    uint32_t cap = p->fact_cap;
    uint32_t mask = cap - 1u;
    uint32_t h = mu_prod_hash_fact(fact) & mask;
    for(uint32_t probe = 0; probe < cap; ++probe)
    {
        uint32_t idx = (h + probe) & mask;
        if(p->facts[idx] == 0u)
            return;
        if(p->facts[idx] == fact)
        {
            p->facts[idx] = 0u;
            --p->fact_count;
            return;
        }
    }
}

/* assert a fact into working memory */
MU_INLINE void mu_production_assert(mu_production* p, uint32_t fact)
{
    MU_ASSERT(p != NULL);
    mu_prod_add_fact(p, fact);
}

/* retract a fact */
MU_INLINE void mu_production_retract(mu_production* p, uint32_t fact)
{
    MU_ASSERT(p != NULL);
    mu_prod_del_fact(p, fact);
}

MU_INLINE bool mu_production_contains(mu_production* p, uint32_t fact)
{
    MU_ASSERT(p != NULL);
    return mu_prod_has_fact(p, fact);
}

/* ---------- match / fire ---------- */

/* Fire the first enabled rule whose conditions all hold. Returns the rule
 * index fired, or MU_INVALID_INDEX if none. On fire: conditions are
 * retracted, actions asserted (via the user action callback). */
MU_INLINE uint32_t mu_production_fire_one(mu_production* p)
{
    MU_ASSERT(p != NULL);
    for(uint32_t i = 0; i < p->rule_count; ++i)
    {
        mu_prod_rule* r = &p->rules[i];
        if(!r->enabled)
            continue;
        bool all = true;
        for(uint32_t c = 0; c < r->cond_count; ++c)
        {
            uint32_t fact = r->conditions[c];
            if(fact == 0u)
                continue;
            if(!mu_prod_has_fact(p, fact))
            {
                all = false;
                break;
            }
            if(p->iface && p->iface->condition &&
               !p->iface->condition(p->iface->ctx, fact))
            {
                all = false;
                break;
            }
        }
        if(!all)
            continue;

        /* fire: retract conditions, assert actions */
        for(uint32_t c = 0; c < r->cond_count; ++c)
            mu_prod_del_fact(p, r->conditions[c]);
        for(uint32_t a = 0; a < r->act_count; ++a)
        {
            uint32_t act = r->actions[a];
            if(act == 0u)
                continue;
            mu_prod_add_fact(p, act);
            if(p->iface && p->iface->action)
                p->iface->action(p->iface->ctx, act);
        }
        return i;
    }
    return MU_INVALID_INDEX;
}

/* Run until no rule fires or `max_steps` reached. Returns steps taken. */
MU_INLINE uint32_t mu_production_run(mu_production* p, uint32_t max_steps)
{
    MU_ASSERT(p != NULL);
    uint32_t steps = 0;
    while(steps < max_steps)
    {
        if(mu_production_fire_one(p) == MU_INVALID_INDEX)
            break;
        ++steps;
    }
    return steps;
}

MU_END_EXTERN_C
