#pragma once

/*
===============================================================================

mu_goap.h — operator-based planner (GOAP/STRIPS, the F.E.A.R. lineage).

Where mu_htn composes authored methods and mu_production matches rules,
mu_goap *searches*: operators with preconditions/effects over a bitset world
state, A* from start to goal.

- World state: 4 x u64 = 256 facts (MU_GOAP_WORDS; raise if you need more).
- Operators: precondition / add / delete bitsets + cost. First matching rule
  semantics do NOT apply — all applicable ops are successors.
- A* over states; heuristic = popcount of unsatisfied goal facts (admissible:
  each unsatisfied fact needs at least one operator).
- Fixed-size open/closed sets (MU_GOAP_MAX_NODES) — small-world planner, no
  allocation. Honest loss: for >~10^4 reachable states, hash the closed set
  (mu_hash_table) instead of the linear scan used here.

The plan output is a sequence of operator indices. Validate with
mu_goap_check_plan in tests.

===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

#define MU_GOAP_WORDS     4    /* 256 facts                     */
#define MU_GOAP_MAX_OPS   64
#define MU_GOAP_MAX_NODES 128
#define MU_GOAP_MAX_PLAN  32

typedef struct mu_goap_state
{
    uint64_t bits[MU_GOAP_WORDS];
} mu_goap_state;

typedef struct mu_goap_op
{
    mu_goap_state pre;   /* all bits must be set in the world        */
    mu_goap_state add;   /* bits to set                              */
    mu_goap_state del;   /* bits to clear                            */
    float cost;          /* >= 0; 0-cost ops are tie-broken FIFO     */
} mu_goap_op;

MU_INLINE void mu_goap_state_clear(mu_goap_state* s)
{
    int i;
    for (i = 0; i < MU_GOAP_WORDS; ++i) s->bits[i] = 0;
}

MU_INLINE void mu_goap_state_set(mu_goap_state* s, int fact)
{
    if (fact < 0 || fact >= MU_GOAP_WORDS * 64) return;
    s->bits[fact >> 6] |= (uint64_t)1 << (fact & 63);
}

MU_INLINE int mu_goap_state_get(const mu_goap_state* s, int fact)
{
    if (fact < 0 || fact >= MU_GOAP_WORDS * 64) return 0;
    return (s->bits[fact >> 6] >> (fact & 63)) & 1u ? 1 : 0;
}

/* All bits of `required` present in `s`? */
MU_INLINE int mu_goap_satisfies(const mu_goap_state* s,
                                const mu_goap_state* required)
{
    int i;
    for (i = 0; i < MU_GOAP_WORDS; ++i)
        if ((s->bits[i] & required->bits[i]) != required->bits[i]) return 0;
    return 1;
}

MU_INLINE int mu_goap_equals(const mu_goap_state* a, const mu_goap_state* b)
{
    int i;
    for (i = 0; i < MU_GOAP_WORDS; ++i)
        if (a->bits[i] != b->bits[i]) return 0;
    return 1;
}

MU_INLINE mu_goap_state mu_goap_apply(const mu_goap_state* s,
                                      const mu_goap_op* op)
{
    mu_goap_state r;
    int i;
    for (i = 0; i < MU_GOAP_WORDS; ++i)
        r.bits[i] = (s->bits[i] | op->add.bits[i]) & ~op->del.bits[i];
    return r;
}

/* Heuristic: unsatisfied goal facts. */
MU_INLINE int mu_goap_h(const mu_goap_state* s, const mu_goap_state* goal)
{
    int i, count = 0;
    for (i = 0; i < MU_GOAP_WORDS; ++i)
    {
        uint64_t missing = goal->bits[i] & ~s->bits[i];
        while (missing)
        {
            missing &= missing - 1; /* clear lowest set bit */
            ++count;
        }
    }
    return count;
}

typedef struct mu_goap_node
{
    mu_goap_state state;
    float g, f;
    int parent;   /* node index, -1 root */
    int op;       /* operator that led here, -1 root */
    int closed;   /* 1 once expanded */
} mu_goap_node;

/* Plan from `start` to any state satisfying `goal`.
 * Writes operator indices to out_ops (in order). Returns plan length,
 * or -1 on failure (unreachable, or MU_GOAP_MAX_NODES exhausted). */
MU_INLINE int mu_goap_plan(const mu_goap_op* ops, int n_ops,
                           const mu_goap_state* start,
                           const mu_goap_state* goal,
                           int* out_ops, int max_out)
{
    mu_goap_node nodes[MU_GOAP_MAX_NODES];
    int n_nodes = 0;
    int closed_count = 0;
    int i;

    if (n_ops > MU_GOAP_MAX_OPS) return -1;
    if (mu_goap_satisfies(start, goal)) return 0;

    nodes[0].state = *start;
    nodes[0].g = 0.0f;
    nodes[0].f = (float)mu_goap_h(start, goal);
    nodes[0].parent = -1;
    nodes[0].op = -1;
    nodes[0].closed = 0;
    n_nodes = 1;

    while (closed_count < n_nodes)
    {
        /* pick open node with min f */
        int best = -1;
        float best_f = 1e30f;
        for (i = 0; i < n_nodes; ++i)
        {
            if (nodes[i].closed) continue;
            if (nodes[i].f < best_f) { best_f = nodes[i].f; best = i; }
        }
        if (best < 0) break;
        if (mu_goap_satisfies(&nodes[best].state, goal))
        {
            /* reconstruct */
            int len = 0;
            int cur = best;
            int rev[MU_GOAP_MAX_PLAN];
            while (nodes[cur].parent >= 0 && len < MU_GOAP_MAX_PLAN)
            {
                rev[len++] = nodes[cur].op;
                cur = nodes[cur].parent;
            }
            if (len > max_out) return -1;
            for (i = 0; i < len; ++i)
                out_ops[i] = rev[len - 1 - i];
            return len;
        }
        nodes[best].closed = 1;
        ++closed_count;

        for (i = 0; i < n_ops; ++i)
        {
            mu_goap_state next;
            float g;
            int j, dup = 0;
            if (!mu_goap_satisfies(&nodes[best].state, &ops[i].pre)) continue;
            next = mu_goap_apply(&nodes[best].state, &ops[i]);
            g = nodes[best].g + ops[i].cost;
            /* dedup against open nodes */
            for (j = 0; j < n_nodes; ++j)
            {
                if (nodes[j].closed) continue;
                if (mu_goap_equals(&nodes[j].state, &next))
                {
                    dup = 1;
                    if (g < nodes[j].g)
                    {
                        nodes[j].g = g;
                        nodes[j].f = g + (float)mu_goap_h(&next, goal);
                        nodes[j].parent = best;
                        nodes[j].op = i;
                    }
                    break;
                }
            }
            if (dup) continue;
            if (n_nodes >= MU_GOAP_MAX_NODES) return -1;
            nodes[n_nodes].state = next;
            nodes[n_nodes].g = g;
            nodes[n_nodes].f = g + (float)mu_goap_h(&next, goal);
            nodes[n_nodes].parent = best;
            nodes[n_nodes].op = i;
            nodes[n_nodes].closed = 0;
            ++n_nodes;
        }
    }
    return -1;
}

/* Validate a plan: every precondition satisfied along the chain. */
MU_INLINE int mu_goap_check_plan(const mu_goap_op* ops,
                                 const mu_goap_state* start,
                                 const int* plan, int len,
                                 const mu_goap_state* goal)
{
    mu_goap_state s = *start;
    int i;
    for (i = 0; i < len; ++i)
    {
        if (plan[i] < 0) return 0;
        if (!mu_goap_satisfies(&s, &ops[plan[i]].pre)) return 0;
        s = mu_goap_apply(&s, &ops[plan[i]]);
    }
    return mu_goap_satisfies(&s, goal);
}

MU_END_EXTERN_C
