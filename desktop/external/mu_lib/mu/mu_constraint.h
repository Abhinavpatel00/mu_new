#pragma once

/*
===============================================================================

mu_constraint.h — lightweight constraint solver (CSP).

Game AI Pro: "Constraint Solving for Game AI", "Loadout and Squad Formation
via Constraint Satisfaction", "Dialogue Choice Constraints".

- Variables each carry a finite domain of uint32_t values.
- Binary constraints between pairs of variables, expressed as a user
  predicate check(value_a, value_b, ctx) -> bool.
- Solving: backtracking search with minimum-remaining-values (MRV) variable
  ordering and forward checking (arc consistency on assignment).
- All flat arrays; domains are copied during search so the original domains
  are never mutated.

===============================================================================
*/

#include "mu_common.h"

#include <stdlib.h>
#include <string.h>

MU_BEGIN_EXTERN_C

/* Binary constraint predicate: true iff value_a on var_a and value_b on
 * var_b may coexist. */
typedef bool (*mu_csp_binary_check)(uint32_t value_a, uint32_t value_b,
                                    void* ctx);

/* One variable with its domain of legal values. */
typedef struct mu_csp_var
{
    uint32_t* values;   /* domain values (sorted, unique) */
    uint32_t  count;
    uint32_t  cap;
} mu_csp_var;

/* One binary constraint between var_a and var_b. */
typedef struct mu_csp_constraint
{
    uint32_t var_a;
    uint32_t var_b;
    mu_csp_binary_check check;
    void*    ctx;
} mu_csp_constraint;

typedef struct mu_csp
{
    mu_csp_var*       vars;
    uint32_t          var_count;
    uint32_t          var_cap;

    mu_csp_constraint* constraints;
    uint32_t           constraint_count;
    uint32_t           constraint_cap;

    /* adjacency: for each var, list of constraint indices touching it */
    uint32_t*          var_constraints;   /* [var_count * max_degree] */
    uint32_t*          var_constraint_n;  /* [var_count] */
    uint32_t           max_degree;
} mu_csp;

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_csp_init(mu_csp* csp, uint32_t max_vars)
{
    MU_ASSERT(csp != NULL);
    MU_ASSERT(max_vars > 0u);

    csp->vars = (mu_csp_var*)MU_CALLOC(max_vars, sizeof(mu_csp_var));
    csp->constraints = (mu_csp_constraint*)MU_CALLOC(
        max_vars * 4u, sizeof(mu_csp_constraint)); /* heuristic cap */
    csp->var_constraints = (uint32_t*)MU_CALLOC(
        (size_t)max_vars * (size_t)max_vars, sizeof(uint32_t));
    csp->var_constraint_n = (uint32_t*)MU_CALLOC(max_vars, sizeof(uint32_t));

    bool ok = csp->vars && csp->constraints && csp->var_constraints &&
              csp->var_constraint_n;
    if(!ok)
    {
        MU_FREE(csp->vars);
        MU_FREE(csp->constraints);
        MU_FREE(csp->var_constraints);
        MU_FREE(csp->var_constraint_n);
        csp->vars = NULL;
        csp->constraints = NULL;
        csp->var_constraints = NULL;
        csp->var_constraint_n = NULL;
        return false;
    }

    csp->var_count = 0;
    csp->var_cap = max_vars;
    csp->constraint_count = 0;
    csp->constraint_cap = max_vars * 4u;
    csp->max_degree = max_vars;
    return true;
}

MU_INLINE void mu_csp_destroy(mu_csp* csp)
{
    if(csp == NULL)
        return;
    for(uint32_t i = 0; i < csp->var_count; ++i)
        MU_FREE(csp->vars[i].values);
    MU_FREE(csp->vars);
    MU_FREE(csp->constraints);
    MU_FREE(csp->var_constraints);
    MU_FREE(csp->var_constraint_n);
    csp->vars = NULL;
    csp->constraints = NULL;
    csp->var_constraints = NULL;
    csp->var_constraint_n = NULL;
    csp->var_count = csp->var_cap = 0;
    csp->constraint_count = csp->constraint_cap = 0;
    csp->max_degree = 0;
}

MU_INLINE uint32_t mu_csp_var_count(const mu_csp* csp) { return csp->var_count; }

/* ---------- variables ---------- */

/* Add a variable whose domain is `values[0..count)`. Returns the variable
 * index in *out_var. The domain is copied. */
MU_INLINE bool mu_csp_add_var(mu_csp* csp, const uint32_t* values,
                              uint32_t count, uint32_t* out_var)
{
    MU_ASSERT(csp != NULL);
    MU_ASSERT(values != NULL || count == 0u);
    MU_ASSERT(out_var != NULL);
    if(csp->var_count >= csp->var_cap)
        return false;
    uint32_t vi = csp->var_count++;
    mu_csp_var* v = &csp->vars[vi];
    v->values = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * count);
    v->count = count;
    v->cap = count;
    if(count > 0u && v->values == NULL)
        return false;
    if(count > 0u)
        MU_MEMCPY(v->values, values, sizeof(uint32_t) * count);
    *out_var = vi;
    return true;
}

/* ---------- constraints ---------- */

/* Add a binary constraint between var_a and var_b. */
MU_INLINE bool mu_csp_add_constraint(mu_csp* csp, uint32_t var_a,
                                     uint32_t var_b,
                                     mu_csp_binary_check check, void* ctx)
{
    MU_ASSERT(csp != NULL);
    MU_ASSERT(var_a < csp->var_count && var_b < csp->var_count);
    MU_ASSERT(check != NULL);
    if(csp->constraint_count >= csp->constraint_cap)
    {
        uint32_t nc = csp->constraint_cap * 2u;
        mu_csp_constraint* nc_arr = (mu_csp_constraint*)MU_REALLOC(
            csp->constraints, sizeof(mu_csp_constraint) * nc);
        if(!nc_arr)
            return false;
        csp->constraints = nc_arr;
        csp->constraint_cap = nc;
    }
    uint32_t ci = csp->constraint_count++;
    csp->constraints[ci].var_a = var_a;
    csp->constraints[ci].var_b = var_b;
    csp->constraints[ci].check = check;
    csp->constraints[ci].ctx = ctx;

    /* record adjacency for both endpoints (avoid duplicate degree bloat) */
    if(var_a != var_b)
    {
        if(csp->var_constraint_n[var_a] < csp->max_degree)
            csp->var_constraints[(size_t)var_a * (size_t)csp->max_degree +
                                 csp->var_constraint_n[var_a]++] = ci;
        if(csp->var_constraint_n[var_b] < csp->max_degree)
            csp->var_constraints[(size_t)var_b * (size_t)csp->max_degree +
                                 csp->var_constraint_n[var_b]++] = ci;
    }
    else
    {
        if(csp->var_constraint_n[var_a] < csp->max_degree)
            csp->var_constraints[(size_t)var_a * (size_t)csp->max_degree +
                                 csp->var_constraint_n[var_a]++] = ci;
    }
    return true;
}

/* ---------- solving ---------- */

/* Working state during one solve call. */
typedef struct mu_csp_search
{
    const mu_csp* csp;
    /* current domain of each var during search (values still possible) */
    uint32_t**  domains;
    uint32_t*   domain_n;
    /* assigned value per var, or MU_INVALID_INDEX if unassigned */
    uint32_t*   assignment;
    /* trail of (var, value) removed by forward checking, to undo */
    uint32_t*   trail_var;
    uint32_t*   trail_val;
    uint32_t    trail_n;
    uint32_t    trail_cap;
    uint32_t    nodes;       /* search nodes visited */
    uint32_t    max_nodes;   /* node budget to avoid runaway search */
} mu_csp_search;

/* Forward check: after assigning var `v` to value `val`, remove from each
 * unassigned neighbor's domain any value inconsistent with `val`.
 * Returns false if any domain becomes empty (dead end). */
static bool mu_csp_forward_check(mu_csp_search* s, uint32_t v, uint32_t val)
{
    const mu_csp* csp = s->csp;
    uint32_t deg = s->csp->var_constraint_n[v];
    for(uint32_t d = 0; d < deg; ++d)
    {
        uint32_t ci =
            s->csp->var_constraints[(size_t)v * (size_t)s->csp->max_degree + d];
        const mu_csp_constraint* con = &csp->constraints[ci];
        uint32_t other = (con->var_a == v) ? con->var_b : con->var_a;
        if(s->assignment[other] != MU_INVALID_INDEX)
            continue; /* already assigned; consistency checked at assign time */

        /* filter other's domain */
        uint32_t* dom = s->domains[other];
        uint32_t n = s->domain_n[other];
        uint32_t write = 0;
        for(uint32_t i = 0; i < n; ++i)
        {
            uint32_t ov = dom[i];
            /* Directional check: `val` is the value of `v`, `ov` is the
             * value of `other`. Place each on the correct side of the
             * predicate. */
            bool ok;
            if(con->var_a == v)
                ok = con->check(val, ov, con->ctx);
            else
                ok = con->check(ov, val, con->ctx);
            if(ok)
            {
                dom[write++] = ov;
            }
            else
            {
                /* trail the removal for undo */
                if(s->trail_n == s->trail_cap)
                {
                    uint32_t nc = s->trail_cap ? s->trail_cap * 2u : 64u;
                    uint32_t* tv = (uint32_t*)MU_REALLOC(
                        s->trail_var, sizeof(uint32_t) * nc);
                    uint32_t* tv2 = (uint32_t*)MU_REALLOC(
                        s->trail_val, sizeof(uint32_t) * nc);
                    if(!tv || !tv2)
                        return false;
                    s->trail_var = tv;
                    s->trail_val = tv2;
                    s->trail_cap = nc;
                }
                s->trail_var[s->trail_n] = other;
                s->trail_val[s->trail_n] = ov;
                ++s->trail_n;
            }
        }
        s->domain_n[other] = write;
        if(write == 0u)
            return false; /* domain wiped out */
    }
    return true;
}

/* Undo forward-checking removals back to a given trail length. */
static void mu_csp_undo(mu_csp_search* s, uint32_t trail_mark)
{
    while(s->trail_n > trail_mark)
    {
        --s->trail_n;
        uint32_t v = s->trail_var[s->trail_n];
        uint32_t val = s->trail_val[s->trail_n];
        /* re-append val to v's domain (domain order need not be sorted) */
        uint32_t n = s->domain_n[v];
        s->domains[v][n] = val;
        s->domain_n[v] = n + 1u;
    }
}

/* Select the unassigned variable with the smallest domain (MRV). Ties broken
 * by lowest index. Returns MU_INVALID_INDEX if all assigned. */
static uint32_t mu_csp_select_var(mu_csp_search* s)
{
    uint32_t best = MU_INVALID_INDEX;
    uint32_t best_n = 0xffffffffu;
    for(uint32_t v = 0; v < s->csp->var_count; ++v)
    {
        if(s->assignment[v] != MU_INVALID_INDEX)
            continue;
        uint32_t n = s->domain_n[v];
        if(n < best_n)
        {
            best_n = n;
            best = v;
            if(n == 1u)
                break; /* can't do better */
        }
    }
    return best;
}

/* Recursive backtracking search. */
static bool mu_csp_search_rec(mu_csp_search* s)
{
    ++s->nodes;
    if(s->nodes > s->max_nodes)
        return false; /* budget exhausted */

    uint32_t v = mu_csp_select_var(s);
    if(v == MU_INVALID_INDEX)
        return true; /* all assigned -> solution */

    const mu_csp* csp = s->csp;
    uint32_t max_deg = csp->max_degree;
    uint32_t* dom = s->domains[v];
    uint32_t n = s->domain_n[v];

    /* Try each value in the current domain. */
    for(uint32_t i = 0; i < n; ++i)
    {
        uint32_t val = dom[i];

        /* Consistency with already-assigned neighbors (arc in). */
        bool consistent = true;
        uint32_t deg = csp->var_constraint_n[v];
        for(uint32_t d = 0; d < deg && consistent; ++d)
        {
            uint32_t ci =
                csp->var_constraints[(size_t)v * (size_t)max_deg + d];
            const mu_csp_constraint* con = &csp->constraints[ci];
            uint32_t other = (con->var_a == v) ? con->var_b : con->var_a;
            uint32_t oval = s->assignment[other];
            if(oval == MU_INVALID_INDEX)
                continue; /* not assigned yet */
            bool ok;
            if(con->var_a == v)
                ok = con->check(val, oval, con->ctx);
            else
                ok = con->check(oval, val, con->ctx);
            if(!ok)
                consistent = false;
        }
        if(!consistent)
            continue;

        /* Assign. */
        s->assignment[v] = val;

        /* Forward check neighbors. */
        uint32_t trail_mark = s->trail_n;
        bool ok = mu_csp_forward_check(s, v, val);
        if(ok)
        {
            if(mu_csp_search_rec(s))
                return true;
        }
        /* Undo assignment and forward-check removals. */
        mu_csp_undo(s, trail_mark);
        s->assignment[v] = MU_INVALID_INDEX;
    }
    return false;
}

/* Solve the CSP. On success, fills assignment[var] = chosen value and
 * returns true. On failure (no solution or budget exhausted) returns false.
 * `max_nodes` bounds search effort; pass UINT32_MAX for unbounded. */
MU_INLINE bool mu_csp_solve(mu_csp* csp, uint32_t* assignment,
                            uint32_t max_nodes)
{
    MU_ASSERT(csp != NULL);
    MU_ASSERT(assignment != NULL);

    uint32_t nv = csp->var_count;
    if(nv == 0u)
        return true;

    /* Build working domains (copies). */
    uint32_t** domains = (uint32_t**)MU_MALLOC(sizeof(uint32_t*) * nv);
    uint32_t* domain_n = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * nv);
    uint32_t* asgn = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * nv);
    if(!domains || !domain_n || !asgn)
    {
        MU_FREE(domains);
        MU_FREE(domain_n);
        MU_FREE(asgn);
        return false;
    }
    for(uint32_t v = 0; v < nv; ++v)
    {
        domain_n[v] = csp->vars[v].count;
        domains[v] = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * domain_n[v]);
        if(!domains[v])
        {
            /* cleanup */
            for(uint32_t k = 0; k < v; ++k)
                MU_FREE(domains[k]);
            MU_FREE(domains);
            MU_FREE(domain_n);
            MU_FREE(asgn);
            return false;
        }
        MU_MEMCPY(domains[v], csp->vars[v].values,
                  sizeof(uint32_t) * domain_n[v]);
        asgn[v] = MU_INVALID_INDEX;
    }

    mu_csp_search s;
    s.csp = csp;
    s.domains = domains;
    s.domain_n = domain_n;
    s.assignment = asgn;
    s.trail_var = NULL;
    s.trail_val = NULL;
    s.trail_n = 0;
    s.trail_cap = 0;
    s.nodes = 0;
    s.max_nodes = max_nodes;

    bool ok = mu_csp_search_rec(&s);

    if(ok)
    {
        for(uint32_t v = 0; v < nv; ++v)
            assignment[v] = asgn[v];
    }

    /* cleanup */
    for(uint32_t v = 0; v < nv; ++v)
        MU_FREE(domains[v]);
    MU_FREE(domains);
    MU_FREE(domain_n);
    MU_FREE(asgn);
    MU_FREE(s.trail_var);
    MU_FREE(s.trail_val);
    return ok;
}

MU_END_EXTERN_C
