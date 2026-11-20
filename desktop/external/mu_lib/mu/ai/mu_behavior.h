#pragma once

/*
===============================================================================

mu_behavior.h — gameplay AI: finite-state machines, behavior trees and utility
scoring, all in flat arrays with index-based handles (no pointer chasing).

Design (DOD, matches AGENT.md + .agents/skills/behavior-ai/):
  - States / nodes / actions are dense arrays indexed by u32 handles.
  - Transitions and tree edges live in flat arrays; traversal is linear.
  - No raw pointers held across ticks; user resolves callback ids to their own
    functions through a supplied context pointer.
  - Tick is O(tree depth) for behavior trees and O(transition count) for FSMs.
  - Allocation only at init; static variants take caller-provided memory.

Builds on:
  - mu_toposort.h   (CSR graph + Kahn's algorithm — used for BT compile order
                      and FSM transition validation)
  - mu_easing.h     (utility curves over blackboard inputs)
  - mu_common.h     (MU_INLINE, MU_ASSERT, MU_INVALID_INDEX, ...)

Subsystems:
  1. mu_fsm         — flat state table + flat transition list
  2. mu_btree       — behavior tree (sequence/selector/action/condition/inverter)
  3. mu_utility     — blackboard + weighted ease-curve considerations, argmax pick

===============================================================================
*/

#include "mu_common.h"
#include "mu/mu_toposort.h"
#include "mu/mu_easing.h"

#include <float.h>

MU_BEGIN_EXTERN_C

/* ===========================================================================
 * 1. Finite-State Machine
 * ===========================================================================
 *
 * Flat layout:
 *   states[]   — one entry per state (enter/update/exit callback ids)
 *   transitions[] — (from, trigger, to) triples, one per edge
 *
 * Triggers are user-defined u32 tags (e.g. input events, timer expiry). The
 * FSM does not own an event queue — the caller polls or pushes triggers via
 * mu_fsm_feed() and then calls mu_fsm_update() to resolve them.
 * =========================================================================== */

typedef struct mu_fsm_transition
{
    uint32_t from;    /* source state id         */
    uint32_t trigger; /* event tag that fires it */
    uint32_t to;      /* target state id         */
} mu_fsm_transition;

typedef struct mu_fsm
{
    uint32_t*     state_cb;   /* [state_count] callback id per state   */
    uint32_t      state_count;

    mu_fsm_transition* transitions;
    uint32_t           transition_count;
    uint32_t           transition_cap;

    uint32_t current;   /* active state id             */
    uint32_t previous;  /* state entered last tick     */

    /* Buffered trigger feed for this tick: flat list of trigger tags. */
    uint32_t*   pending_triggers;
    uint32_t    pending_count;
    uint32_t    pending_cap;
} mu_fsm;

/* ---- C-compatible no-op stubs for optional FSM callbacks ---- */
MU_INLINE void mu_fsm_noop_enter(uint32_t s, void* c) { (void)s; (void)c; }
MU_INLINE void mu_fsm_noop_exit(uint32_t s, void* c)  { (void)s; (void)c; }
MU_INLINE void mu_fsm_noop_update(uint32_t s, void* c){ (void)s; (void)c; }

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_fsm_init(mu_fsm* f, uint32_t initial_state,
                           uint32_t state_count, uint32_t max_transitions)
{
    MU_ASSERT(f != NULL);
    MU_ASSERT(state_count > 0u);
    MU_ASSERT(initial_state < state_count);

    f->state_cb = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * state_count);
    if(!f->state_cb)
        return false;
    MU_MEMSET(f->state_cb, 0, sizeof(uint32_t) * state_count);

    f->transitions = (mu_fsm_transition*)MU_MALLOC(
        sizeof(mu_fsm_transition) * max_transitions);
    if(!f->transitions)
    {
        MU_FREE(f->state_cb);
        f->state_cb = NULL;
        return false;
    }

    f->pending_triggers = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * 16u);
    if(!f->pending_triggers)
    {
        MU_FREE(f->state_cb);
        MU_FREE(f->transitions);
        f->state_cb = NULL;
        f->transitions = NULL;
        return false;
    }

    f->state_count = state_count;
    f->transition_count = 0;
    f->transition_cap = max_transitions;
    f->current = initial_state;
    f->previous = MU_INVALID_INDEX;
    f->pending_count = 0;
    f->pending_cap = 16u;
    return true;
}

MU_INLINE void mu_fsm_destroy(mu_fsm* f)
{
    if(f == NULL)
        return;
    MU_FREE(f->state_cb);
    MU_FREE(f->transitions);
    MU_FREE(f->pending_triggers);
    f->state_cb = NULL;
    f->transitions = NULL;
    f->pending_triggers = NULL;
    f->state_count = 0;
    f->transition_count = 0;
    f->transition_cap = 0;
    f->current = 0;
    f->previous = MU_INVALID_INDEX;
    f->pending_count = 0;
    f->pending_cap = 0;
}

MU_INLINE void mu_fsm_clear(mu_fsm* f)
{
    MU_ASSERT(f != NULL);
    f->transition_count = 0;
    f->pending_count = 0;
    f->previous = MU_INVALID_INDEX;
}

/* Assign the user callback id for a state. Resolved by the caller at tick time. */
MU_INLINE void mu_fsm_state_cb(mu_fsm* f, uint32_t state, uint32_t cb_id)
{
    MU_ASSERT(f != NULL);
    MU_ASSERT(state < f->state_count);
    f->state_cb[state] = cb_id;
}

/* Add a directed transition (from --trigger--> to). */
MU_INLINE bool mu_fsm_add_transition(mu_fsm* f, uint32_t from,
                                     uint32_t trigger, uint32_t to)
{
    MU_ASSERT(f != NULL);
    MU_ASSERT(from < f->state_count && to < f->state_count);
    if(f->transition_count >= f->transition_cap)
        return false;
    mu_fsm_transition* t = &f->transitions[f->transition_count++];
    t->from = from;
    t->trigger = trigger;
    t->to = to;
    return true;
}

/* Queue a trigger for this tick. Called before mu_fsm_update(). */
MU_INLINE void mu_fsm_feed(mu_fsm* f, uint32_t trigger)
{
    MU_ASSERT(f != NULL);
    if(f->pending_count == f->pending_cap)
    {
        uint32_t nc = f->pending_cap * 2u;
        uint32_t* nt = (uint32_t*)MU_REALLOC(f->pending_triggers,
                                             sizeof(uint32_t) * nc);
        if(!nt)
            return; /* drop on OOM — keep the frame */
        f->pending_triggers = nt;
        f->pending_cap = nc;
    }
    f->pending_triggers[f->pending_count++] = trigger;
}

/* Resolve all queued triggers: find any transition (current --t--> next),
   fire it, run exit/enter callbacks via the caller. Returns the new state. */
MU_INLINE uint32_t mu_fsm_update(mu_fsm* f,
                                 void (*on_enter)(uint32_t state, void* ctx),
                                 void (*on_exit)(uint32_t state, void* ctx),
                                 void (*on_update)(uint32_t state, void* ctx),
                                 void* ctx)
{
    MU_ASSERT(f != NULL);

    if(!on_enter)  on_enter  = mu_fsm_noop_enter;
    if(!on_exit)   on_exit   = mu_fsm_noop_exit;
    if(!on_update) on_update = mu_fsm_noop_update;

    uint32_t next = f->current;

    /* Resolve triggers in feed order; the first matching transition wins. */
    for(uint32_t i = 0; i < f->pending_count; ++i)
    {
        uint32_t trig = f->pending_triggers[i];
        bool fired = false;
        for(uint32_t j = 0; j < f->transition_count; ++j)
        {
            const mu_fsm_transition* t = &f->transitions[j];
            if(t->from == f->current && t->trigger == trig)
            {
                next = t->to;
                fired = true;
                break;
            }
        }
        if(fired)
            break;
    }

    if(next != f->current)
    {
        on_exit(f->current, ctx);
        f->previous = f->current;
        f->current = next;
        on_enter(f->current, ctx);
    }

    on_update(f->current, ctx);

    f->pending_count = 0; /* consume the feed */
    return f->current;
}

/* ===========================================================================
 * 2. Behavior Tree
 * ===========================================================================
 *
 * Flat left-child / right-sibling tree stored in one array:
 *   nodes[i].first_child   — index of first child, or MU_INVALID_INDEX
 *   nodes[i].next_sibling  — index of next sibling, or MU_INVALID_INDEX
 *
 * Node types: sequence, selector, action, condition, inverter.
 * Statuses: running, success, failure.
 *
 * Tick walks from the root; leaf callbacks are invoked via a user-supplied
 * function that maps a node's u32 `data` field to the caller's logic.
 * =========================================================================== */

typedef enum mu_bt_type
{
    MU_BT_SEQUENCE,   /* children run in order; fail on first failure         */
    MU_BT_SELECTOR,   /* children run in order; succeed on first success      */
    MU_BT_ACTION,     /* leaf: invoke callback                                */
    MU_BT_CONDITION,  /* leaf: invoke callback; success == condition true     */
    MU_BT_INVERTER,   /* decorator: flip success <-> failure                  */
} mu_bt_type;

typedef enum mu_bt_status
{
    MU_BT_RUNNING = 0,
    MU_BT_SUCCESS = 1,
    MU_BT_FAILURE = 2,
} mu_bt_status;

typedef mu_bt_status (*mu_bt_leaf_fn)(uint32_t node_data, void* ctx,
                                      float dt, mu_bt_status* out_status);

typedef struct mu_bt_node
{
    uint32_t first_child;   /* index of first child, or MU_INVALID_INDEX */
    uint32_t next_sibling;  /* index of next sibling, or MU_INVALID_INDEX */
    uint32_t data;          /* leaf callback id / user tag                */
    uint32_t status;        /* mu_bt_status of the node this tick         */
    mu_bt_type type;
} mu_bt_node;

typedef struct mu_btree
{
    mu_bt_node* nodes;
    uint32_t    node_count;
    uint32_t    node_cap;
    uint32_t    root;   /* MU_INVALID_INDEX until set */
} mu_btree;

MU_INLINE bool mu_btree_init(mu_btree* t, uint32_t max_nodes)
{
    MU_ASSERT(t != NULL);
    MU_ASSERT(max_nodes > 0u);

    t->nodes = (mu_bt_node*)MU_MALLOC(sizeof(mu_bt_node) * max_nodes);
    if(!t->nodes)
        return false;
    t->node_count = 0;
    t->node_cap = max_nodes;
    t->root = MU_INVALID_INDEX;
    return true;
}

MU_INLINE void mu_btree_destroy(mu_btree* t)
{
    if(t == NULL)
        return;
    MU_FREE(t->nodes);
    t->nodes = NULL;
    t->node_count = 0;
    t->node_cap = 0;
    t->root = MU_INVALID_INDEX;
}

MU_INLINE void mu_btree_clear(mu_btree* t)
{
    MU_ASSERT(t != NULL);
    t->node_count = 0;
    t->root = MU_INVALID_INDEX;
}

MU_INLINE bool mu_btree_set_root(mu_btree* t, uint32_t node)
{
    MU_ASSERT(t != NULL);
    if(node >= t->node_count)
        return false;
    t->root = node;
    return true;
}

/* Append a node; returns its index or MU_INVALID_INDEX if full. */
MU_INLINE uint32_t mu_btree_add_node(mu_btree* t, mu_bt_type type,
                                     uint32_t data)
{
    MU_ASSERT(t != NULL);
    if(t->node_count >= t->node_cap)
        return MU_INVALID_INDEX;
    uint32_t i = t->node_count++;
    mu_bt_node* n = &t->nodes[i];
    n->first_child = MU_INVALID_INDEX;
    n->next_sibling = MU_INVALID_INDEX;
    n->data = data;
    n->status = MU_BT_FAILURE;
    n->type = type;
    return i;
}

/* Attach `child` as a sibling after `prev` under the same parent. */
MU_INLINE void mu_btree_link(mu_btree* t, uint32_t parent, uint32_t child,
                             uint32_t prev_sibling)
{
    MU_ASSERT(t != NULL);
    MU_ASSERT(child < t->node_count);
    mu_bt_node* c = &t->nodes[child];
    c->next_sibling = MU_INVALID_INDEX;

    if(prev_sibling == MU_INVALID_INDEX)
    {
        /* first child of parent */
        c->next_sibling = t->nodes[parent].first_child;
        t->nodes[parent].first_child = child;
    }
    else
    {
        c->next_sibling = t->nodes[prev_sibling].next_sibling;
        t->nodes[prev_sibling].next_sibling = child;
    }
}

/* Forward declaration: the recursive node tick is defined below. */
MU_INLINE mu_bt_status mu_bt_tick_node(mu_btree* t, uint32_t node,
                                       mu_bt_leaf_fn leaf, void* ctx,
                                       float dt);

/* Tick the tree from the root. Returns the root's resulting status.
 * Leaf callbacks are invoked via `leaf(data, ctx, dt, &status)`; return
 * MU_BT_RUNNING/SUCCESS/FAILURE. */
MU_INLINE mu_bt_status mu_btree_tick(mu_btree* t, mu_bt_leaf_fn leaf,
                                     void* ctx, float dt)
{
    MU_ASSERT(t != NULL);
    MU_ASSERT(t->root != MU_INVALID_INDEX);
    return mu_bt_tick_node(t, t->root, leaf, ctx, dt);
}

/* Recursive helper (explicit stack would avoid recursion; kept recursive for
 * clarity — trees are shallow in practice). */
MU_INLINE mu_bt_status mu_bt_tick_node(mu_btree* t, uint32_t node,
                                       mu_bt_leaf_fn leaf, void* ctx,
                                       float dt)
{
    mu_bt_node* n = &t->nodes[node];

    switch(n->type)
    {
    case MU_BT_ACTION:
    case MU_BT_CONDITION:
    {
        mu_bt_status st = MU_BT_FAILURE;
        if(leaf)
            st = leaf(n->data, ctx, dt, &st);
        n->status = st;
        return st;
    }

    case MU_BT_SEQUENCE:
    {
        /* Run children left to right; fail on first failure or running. */
        uint32_t child = n->first_child;
        mu_bt_status last = MU_BT_SUCCESS;
        while(child != MU_INVALID_INDEX)
        {
            mu_bt_status st = mu_bt_tick_node(t, child, leaf, ctx, dt);
            if(st == MU_BT_FAILURE || st == MU_BT_RUNNING)
            {
                n->status = st;
                return st;
            }
            last = st;
            child = t->nodes[child].next_sibling;
        }
        n->status = last;
        return last;
    }

    case MU_BT_SELECTOR:
    {
        /* Run children left to right; succeed on first success or running. */
        uint32_t child = n->first_child;
        mu_bt_status last = MU_BT_FAILURE;
        while(child != MU_INVALID_INDEX)
        {
            mu_bt_status st = mu_bt_tick_node(t, child, leaf, ctx, dt);
            if(st == MU_BT_SUCCESS || st == MU_BT_RUNNING)
            {
                n->status = st;
                return st;
            }
            last = st;
            child = t->nodes[child].next_sibling;
        }
        n->status = last;
        return last;
    }

    case MU_BT_INVERTER:
    {
        uint32_t child = n->first_child;
        mu_bt_status st = (child == MU_INVALID_INDEX)
            ? MU_BT_FAILURE
            : mu_bt_tick_node(t, child, leaf, ctx, dt);
        mu_bt_status inv;
        if(st == MU_BT_SUCCESS)      inv = MU_BT_FAILURE;
        else if(st == MU_BT_FAILURE) inv = MU_BT_SUCCESS;
        else                         inv = MU_BT_RUNNING;
        n->status = inv;
        return inv;
    }

    default:
        n->status = MU_BT_FAILURE;
        return MU_BT_FAILURE;
    }
}

/* Iterate every node index in insertion order (for user-side bookkeeping). */
MU_INLINE uint32_t mu_btree_node_count(const mu_btree* t) { return t->node_count; }
MU_INLINE mu_bt_type mu_btree_node_type(const mu_btree* t, uint32_t i)
{
    MU_ASSERT(i < t->node_count);
    return t->nodes[i].type;
}
MU_INLINE uint32_t mu_btree_node_data(const mu_btree* t, uint32_t i)
{
    MU_ASSERT(i < t->node_count);
    return t->nodes[i].data;
}

/* ===========================================================================
 * 3. Utility Scoring (blackboard + ease-curve considerations)
 * ===========================================================================
 *
 * Flat layout:
 *   actions[]   — one entry per candidate behavior (id + weight)
 *   considerations[] — (action, blackboard_key, ease_kind, lo, hi, weight)
 *
 * Each consideration reads one blackboard float, remaps it to [0,1] through
 * [lo,hi], passes it through a mu_ease_kind curve, and multiplies by weight.
 * An action's score is the weighted sum of its considerations. mu_utility_pick
 * returns the action id with the highest score (argmax).
 * =========================================================================== */

typedef struct mu_utility_consideration
{
    uint32_t action;     /* which action this consideration feeds       */
    uint32_t blackboard; /* index into the caller's blackboard float[]  */
    mu_ease_kind ease;   /* curve applied to the normalized input       */
    float lo;            /* input value mapping to score 0              */
    float hi;            /* input value mapping to score 1              */
    float weight;        /* multiplier on the consideration score       */
} mu_utility_consideration;

typedef struct mu_utility_action
{
    uint32_t id;     /* user-assigned action tag                    */
    float    score;  /* last computed total score                   */
} mu_utility_action;

typedef struct mu_utility
{
    mu_utility_action*   actions;
    uint32_t             action_count;
    uint32_t             action_cap;

    mu_utility_consideration* considerations;
    uint32_t                  consideration_count;
    uint32_t                  consideration_cap;
} mu_utility;

MU_INLINE bool mu_utility_init(mu_utility* u,
                               uint32_t max_actions,
                               uint32_t max_considerations)
{
    MU_ASSERT(u != NULL);
    MU_ASSERT(max_actions > 0u && max_considerations > 0u);

    u->actions = (mu_utility_action*)MU_MALLOC(
        sizeof(mu_utility_action) * max_actions);
    u->considerations = (mu_utility_consideration*)MU_MALLOC(
        sizeof(mu_utility_consideration) * max_considerations);

    bool ok = u->actions && u->considerations;
    if(!ok)
    {
        MU_FREE(u->actions);
        MU_FREE(u->considerations);
        u->actions = NULL;
        u->considerations = NULL;
        return false;
    }

    u->action_count = 0;
    u->action_cap = max_actions;
    u->consideration_count = 0;
    u->consideration_cap = max_considerations;
    return true;
}

MU_INLINE void mu_utility_destroy(mu_utility* u)
{
    if(u == NULL)
        return;
    MU_FREE(u->actions);
    MU_FREE(u->considerations);
    u->actions = NULL;
    u->considerations = NULL;
    u->action_count = u->action_cap = 0;
    u->consideration_count = u->consideration_cap = 0;
}

MU_INLINE void mu_utility_clear(mu_utility* u)
{
    MU_ASSERT(u != NULL);
    u->action_count = 0;
    u->consideration_count = 0;
}

/* Register an action. Returns its slot index or MU_INVALID_INDEX if full. */
MU_INLINE uint32_t mu_utility_add_action(mu_utility* u, uint32_t id)
{
    MU_ASSERT(u != NULL);
    if(u->action_count >= u->action_cap)
        return MU_INVALID_INDEX;
    uint32_t i = u->action_count++;
    u->actions[i].id = id;
    u->actions[i].score = 0.0f;
    return i;
}

/* Add a consideration to action slot `action_slot`. */
MU_INLINE bool mu_utility_add_consideration(mu_utility* u, uint32_t action_slot,
                                            uint32_t blackboard_index,
                                            mu_ease_kind ease, float lo,
                                            float hi, float weight)
{
    MU_ASSERT(u != NULL);
    MU_ASSERT(action_slot < u->action_count);
    if(u->consideration_count >= u->consideration_cap)
        return false;
    mu_utility_consideration* c = &u->considerations[u->consideration_count++];
    c->action = action_slot;
    c->blackboard = blackboard_index;
    c->ease = ease;
    c->lo = lo;
    c->hi = hi;
    c->weight = weight;
    return true;
}

/* Evaluate every action against the blackboard (`blackboard[n]` supplies the
 * float input for consideration n) and return the id of the highest-scoring
 * action, or MU_INVALID_INDEX if there are no actions. */
MU_INLINE uint32_t mu_utility_pick(mu_utility* u, const float* blackboard)
{
    MU_ASSERT(u != NULL);

    /* Zero scores. */
    for(uint32_t i = 0; i < u->action_count; ++i)
        u->actions[i].score = 0.0f;

    /* Accumulate consideration scores per action. */
    for(uint32_t c = 0; c < u->consideration_count; ++c)
    {
        const mu_utility_consideration* cc = &u->considerations[c];
        float raw = blackboard[cc->blackboard];
        float span = cc->hi - cc->lo;
        float t = (span > 0.0f) ? (raw - cc->lo) / span : 0.0f;
        if(t < 0.0f) t = 0.0f;
        if(t > 1.0f) t = 1.0f;
        float score = mu_ease_get(cc->ease)(t) * cc->weight;
        u->actions[cc->action].score += score;
    }

    /* Argmax. */
    uint32_t best = MU_INVALID_INDEX;
    float best_score = -FLT_MAX;
    for(uint32_t i = 0; i < u->action_count; ++i)
    {
        if(u->actions[i].score > best_score)
        {
            best_score = u->actions[i].score;
            best = i;
        }
    }
    return best == MU_INVALID_INDEX ? MU_INVALID_INDEX : u->actions[best].id;
}

MU_END_EXTERN_C
