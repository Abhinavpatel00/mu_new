#pragma once

/*
===============================================================================

mu_mcts.h — Monte Carlo Tree Search (UCT / UCB1).

Generic MCTS over user-defined game states:
- Selection: UCB1 down the tree to a leaf.
- Expansion: add one child node.
- Simulation: user-provided rollout function plays to termination.
- Backpropagation: win/loss score flows back to the root.

Nodes live in a flat array (index-based, no pointer chasing). The caller
provides: state copy/compare, legal-action enumeration, apply-action, and a
rollout (simulate to end and return a score for the player to move).

Canonical surface: init / destroy / reset, mcts_search (runs N iterations
and returns the best root action), plus the user callback struct.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>
#include <stdlib.h>

MU_BEGIN_EXTERN_C

/* ------------------------------------------------------------------
 * User-provided game interface. The MCTS engine knows nothing about
 * the game; it calls these to explore.
 * ------------------------------------------------------------------ */

/* Copy `src` state into `dst`. */
typedef void  (*mu_mcts_copy_state)(void* dst, const void* src);

/* Return true if `a` and `b` are the same state (for transposition dedup;
 * may return false always if the caller doesn't want dedup). */
typedef bool  (*mu_mcts_same_state)(const void* a, const void* b);

/* Enumerate legal actions for `state` into `out_actions` (caller-owned),
 * returning the count written (clamped to out_cap). Action is an opaque u32. */
typedef uint32_t (*mu_mcts_legal_actions)(const void* state,
                                          uint32_t* out_actions,
                                          uint32_t out_cap);

/* Apply `action` to `state` in place. Returns false if illegal. */
typedef bool  (*mu_mcts_apply)(void* state, uint32_t action);

/* Rollout `state` (player `to_move` is about to act) to termination and
 * write a score in [0,1] for `to_move` into `out_score`. */
typedef bool  (*mu_mcts_rollout)(const void* state, uint32_t to_move,
                                 float* out_score, void* rollout_ctx);

/* Context pointer passed to every callback. */

typedef struct mu_mcts_game_iface
{
    mu_mcts_copy_state   copy_state;
    mu_mcts_same_state   same_state;
    mu_mcts_legal_actions legal_actions;
    mu_mcts_apply        apply;
    mu_mcts_rollout      rollout;
    void*                rollout_ctx;
    size_t               state_size;   /* bytes per state */
} mu_mcts_game_iface;

/* ------------------------------------------------------------------
 * Tree node (flat array).
 * ------------------------------------------------------------------ */

typedef struct mu_mcts_node
{
    uint32_t   parent;        /* parent node index, or MU_INVALID_INDEX */
    uint32_t   action_from_parent; /* action that led here */
    uint32_t   first_child;   /* index of first child, or MU_INVALID_INDEX */
    uint32_t   next_sibling;  /* index of next sibling */
    uint32_t   child_count;

    uint32_t   visits;        /* times this node was visited */
    float      total_value;   /* sum of rollout scores (from this node's POV) */
    uint32_t   untried_actions[8]; /* actions not yet expanded (inline) */
    uint32_t   untried_count;
    uint8_t    fully_expanded; /* all legal actions expanded */
} mu_mcts_node;

/* ------------------------------------------------------------------
 * Search configuration.
 * ------------------------------------------------------------------ */

typedef struct mu_mcts_config
{
    uint32_t  iterations;     /* number of search iterations to run       */
    float     ucb_c;          /* exploration constant (sqrt(2) default)   */
    uint32_t  max_depth;      /* safety cap on selection depth            */
    uint32_t  rollout_max_steps; /* safety cap on rollout length          */
} mu_mcts_config;

/* ------------------------------------------------------------------
 * Search handle.
 * ------------------------------------------------------------------ */

typedef struct mu_mcts
{
    const mu_mcts_game_iface* iface;
    const mu_mcts_config*     cfg;

    mu_mcts_node* nodes;
    uint32_t      node_count;
    uint32_t      node_cap;

    uint32_t root;            /* root node index */

    /* scratch action buffer */
    uint32_t* actions;
    uint32_t  actions_cap;

    void*   state_scratch;    /* one state copy for rollout */
} mu_mcts;

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_mcts_init(mu_mcts* m, const mu_mcts_game_iface* iface,
                            const mu_mcts_config* cfg, uint32_t max_nodes)
{
    MU_ASSERT(m != NULL);
    MU_ASSERT(iface != NULL);
    MU_ASSERT(cfg != NULL);
    MU_ASSERT(max_nodes > 0u);

    m->iface = iface;
    m->cfg = cfg;
    m->nodes = (mu_mcts_node*)MU_CALLOC(max_nodes, sizeof(mu_mcts_node));
    if(!m->nodes)
        return false;
    m->actions = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * 256u);
    if(!m->actions)
    {
        MU_FREE(m->nodes);
        m->nodes = NULL;
        return false;
    }
    m->state_scratch = MU_MALLOC(iface->state_size);
    if(!m->state_scratch)
    {
        MU_FREE(m->nodes);
        MU_FREE(m->actions);
        m->nodes = NULL;
        m->actions = NULL;
        return false;
    }

    m->node_count = 0;
    m->node_cap = max_nodes;
    m->root = MU_INVALID_INDEX;
    m->actions_cap = 256u;
    return true;
}

MU_INLINE void mu_mcts_destroy(mu_mcts* m)
{
    if(m == NULL)
        return;
    MU_FREE(m->nodes);
    MU_FREE(m->actions);
    MU_FREE(m->state_scratch);
    m->nodes = NULL;
    m->actions = NULL;
    m->state_scratch = NULL;
    m->node_count = m->node_cap = 0;
    m->root = MU_INVALID_INDEX;
    m->actions_cap = 0;
}

MU_INLINE void mu_mcts_reset(mu_mcts* m)
{
    MU_ASSERT(m != NULL);
    m->node_count = 0;
    m->root = MU_INVALID_INDEX;
}

/* ---------- node helpers ---------- */

static uint32_t mu_mcts_add_node(mu_mcts* m, uint32_t parent,
                                 uint32_t action_from_parent)
{
    if(m->node_count >= m->node_cap)
        return MU_INVALID_INDEX;
    uint32_t i = m->node_count++;
    mu_mcts_node* n = &m->nodes[i];
    n->parent = parent;
    n->action_from_parent = action_from_parent;
    n->first_child = MU_INVALID_INDEX;
    n->next_sibling = MU_INVALID_INDEX;
    n->child_count = 0;
    n->visits = 0;
    n->total_value = 0.0f;
    n->untried_count = 0;
    n->fully_expanded = 0;
    return i;
}

static void mu_mcts_add_child(mu_mcts* m, uint32_t parent_idx,
                              uint32_t child_idx)
{
    mu_mcts_node* p = &m->nodes[parent_idx];
    mu_mcts_node* c = &m->nodes[child_idx];
    c->next_sibling = p->first_child;
    p->first_child = child_idx;
    ++p->child_count;
}

static float mu_mcts_ucb1(const mu_mcts_node* parent,
                          const mu_mcts_node* child, float c)
{
    if(child->visits == 0u)
        return INFINITY;
    float exploit = child->total_value / (float)child->visits;
    float explore = c * sqrtf(logf((float)parent->visits) /
                              (float)child->visits);
    return exploit + explore;
}

/* ---------- search ---------- */

/* Run `iterations` MCTS iterations from `root_state`. Returns the best
 * action at the root (most-visited child's action). */
MU_INLINE uint32_t mu_mcts_search(mu_mcts* m, void* root_state,
                                  uint32_t player_to_move)
{
    MU_ASSERT(m != NULL);
    MU_ASSERT(m->iface != NULL);
    MU_ASSERT(root_state != NULL);

    const mu_mcts_config* cfg = m->cfg;
    const mu_mcts_game_iface* iface = m->iface;

    /* (re)build root */
    m->node_count = 0;
    m->root = mu_mcts_add_node(m, MU_INVALID_INDEX, MU_INVALID_INDEX);
    if(m->root == MU_INVALID_INDEX)
        return MU_INVALID_INDEX;

    /* copy root state into scratch (not strictly needed here; caller keeps
       root_state, but we keep a scratch for rollouts) */
    iface->copy_state(m->state_scratch, root_state);

    /* initialize root's untried actions */
    uint32_t n_actions = iface->legal_actions(root_state,
                                              m->actions, m->actions_cap);
    mu_mcts_node* root_node = &m->nodes[m->root];
    root_node->untried_count = 0;
    for(uint32_t i = 0; i < n_actions && i < 8u; ++i)
        root_node->untried_actions[i] = m->actions[i];
    root_node->untried_count = (n_actions < 8u) ? n_actions : 8u;
    root_node->fully_expanded = (root_node->untried_count == 0u) ? 1u : 0u;

    for(uint32_t it = 0; it < cfg->iterations; ++it)
    {
        /* ---- selection ---- */
        iface->copy_state(m->state_scratch, root_state);
        uint32_t node = m->root;
        uint32_t depth = 0;
        uint32_t current_player = player_to_move;

        while(node != MU_INVALID_INDEX &&
              m->nodes[node].fully_expanded &&
              m->nodes[node].child_count > 0u &&
              depth < cfg->max_depth)
        {
            /* select best child by UCB1 */
            float best_val = -INFINITY;
            uint32_t best_child = MU_INVALID_INDEX;
            uint32_t child = m->nodes[node].first_child;
            while(child != MU_INVALID_INDEX)
            {
                float val = mu_mcts_ucb1(&m->nodes[node], &m->nodes[child],
                                         cfg->ucb_c);
                if(val > best_val)
                {
                    best_val = val;
                    best_child = child;
                }
                child = m->nodes[child].next_sibling;
            }
            if(best_child == MU_INVALID_INDEX)
                break;

            /* apply best child's action to state */
            uint32_t act = m->nodes[best_child].action_from_parent;
            if(!iface->apply(m->state_scratch, act))
                break;

            current_player = (current_player + 1u) & 1u; /* two-player */
            node = best_child;
            ++depth;
        }

        /* ---- expansion ---- */
        mu_mcts_node* leaf = &m->nodes[node];
        if(!leaf->fully_expanded && leaf->untried_count > 0u)
        {
            /* pick an untried action */
            uint32_t act = leaf->untried_actions[--leaf->untried_count];
            if(leaf->untried_count == 0u)
                leaf->fully_expanded = 1u;

            if(!iface->apply(m->state_scratch, act))
            {
                /* illegal (shouldn't happen); skip expansion */
                node = MU_INVALID_INDEX; /* force backprop at leaf? */
            }
            else
            {
                uint32_t child = mu_mcts_add_node(m, node, act);
                if(child != MU_INVALID_INDEX)
                {
                    mu_mcts_add_child(m, node, child);
                    /* initialize child untried actions */
                    uint32_t ca = iface->legal_actions(m->state_scratch,
                                                       m->actions,
                                                       m->actions_cap);
                    mu_mcts_node* cn = &m->nodes[child];
                    cn->untried_count = 0;
                    for(uint32_t i = 0; i < ca && i < 8u; ++i)
                        cn->untried_actions[i] = m->actions[i];
                    cn->untried_count = (ca < 8u) ? ca : 8u;
                    cn->fully_expanded = (cn->untried_count == 0u) ? 1u : 0u;
                    node = child;
                }
                current_player = (current_player + 1u) & 1u;
            }
        }

        if(node == MU_INVALID_INDEX)
            node = leaf - m->nodes; /* fallback: backprop at leaf */

        /* ---- simulation (rollout) ---- */
        float score = 0.0f;
        if(iface->rollout(m->state_scratch, current_player, &score,
                          iface->rollout_ctx))
        {
            /* ---- backpropagation ---- */
            uint32_t cur = (uint32_t)(node - (uint32_t)0); /* node index */
            while(cur != MU_INVALID_INDEX)
            {
                mu_mcts_node* n = &m->nodes[cur];
                ++n->visits;
                /* score is from the perspective of current_player at this
                 * node; alternate sign each level up for zero-sum two-player */
                n->total_value += score;
                score = 1.0f - score;
                cur = n->parent;
            }
        }
    }

    /* ---- pick best root action (most visits) ---- */
    uint32_t best_action = MU_INVALID_INDEX;
    uint32_t best_visits = 0;
    uint32_t child = m->nodes[m->root].first_child;
    while(child != MU_INVALID_INDEX)
    {
        if(m->nodes[child].visits > best_visits)
        {
            best_visits = m->nodes[child].visits;
            best_action = m->nodes[child].action_from_parent;
        }
        child = m->nodes[child].next_sibling;
    }
    return best_action;
}

MU_END_EXTERN_C
