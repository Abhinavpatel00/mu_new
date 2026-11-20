#pragma once

/*
===============================================================================

mu_htn.h — simplified Hierarchical Task Network planner.

Game AI Pro: "Exploring HTN Planners through Example",
"Hierarchical Plan-Space Planning for Multi-unit Combat Maneuvers".

Model:
- Tasks are either compound (decomposed by methods) or primitive (executed
  directly by the user).
- A method matches a compound task + world-state conditions and produces an
  ordered list of subtasks.
- The planner depth-first decomposes the top task into primitive tasks,
  returning an executable task sequence.

All flat arrays; no recursion heap beyond the explicit stack.

===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

/* ------------------------------------------------------------------
 * User-provided world / task interface.
 * ------------------------------------------------------------------ */

/* World state is opaque to the planner; caller passes a pointer. */

/* Test whether a world-state condition holds. `arg` is a u32 argument
 * associated with the condition. Return true if satisfied. */
typedef bool (*mu_htn_condition)(const void* world, uint32_t arg);

/* Execute a primitive task. Return true on success. */
typedef bool (*mu_htn_execute)(void* world, uint32_t task_id,
                               const uint32_t* args, uint32_t arg_count);

/* Context pointer. */

typedef struct mu_htn_iface
{
    mu_htn_condition condition;
    mu_htn_execute   execute;
    void*            ctx;
} mu_htn_iface;

/* ------------------------------------------------------------------
 * Task representation.
 * ------------------------------------------------------------------ */

typedef enum mu_htn_task_type
{
    MU_HTN_COMPOUND,   /* decomposed by a method                    */
    MU_HTN_PRIMITIVE,  /* executed directly                         */
} mu_htn_task_type;

#define MU_HTN_MAX_ARGS 4u

typedef struct mu_htn_task
{
    mu_htn_task_type type;
    uint32_t         id;        /* task identifier                       */
    uint32_t         args[MU_HTN_MAX_ARGS];
    uint32_t         arg_count;
} mu_htn_task;

/* A method: (task_id, conditions[], subtasks[]) */
typedef struct mu_htn_method
{
    uint32_t         task_id;       /* compound task this method applies to  */
    uint32_t         cond_count;    /* number of preconditions               */
    uint32_t         cond_args[8];  /* condition argument                    */
    /* condition ids are implicit: the i-th condition uses
     * mu_htn_iface.condition(world, cond_args[i]) */
    uint32_t         subtask_count;
    uint32_t         subtask_ids[16];
    uint32_t         subtask_args[16][MU_HTN_MAX_ARGS];
    uint32_t         subtask_arg_counts[16];
} mu_htn_method;

/* ------------------------------------------------------------------
 * Planner handle.
 * ------------------------------------------------------------------ */

typedef struct mu_htn
{
    const mu_htn_iface* iface;

    mu_htn_method* methods;
    uint32_t       method_count;
    uint32_t       method_cap;

    /* explicit decomposition stack */
    mu_htn_task*   stack;
    uint32_t       stack_count;
    uint32_t       stack_cap;

    /* resulting plan */
    mu_htn_task*   plan;
    uint32_t       plan_count;
    uint32_t       plan_cap;

    uint32_t       max_depth;
} mu_htn;

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_htn_init(mu_htn* h, const mu_htn_iface* iface,
                           uint32_t max_methods, uint32_t max_depth)
{
    MU_ASSERT(h != NULL);
    MU_ASSERT(iface != NULL);
    MU_ASSERT(max_methods > 0u && max_depth > 0u);

    h->iface = iface;

    h->methods = (mu_htn_method*)MU_CALLOC(max_methods, sizeof(mu_htn_method));
    if(!h->methods)
        return false;
    h->method_count = 0;
    h->method_cap = max_methods;

    h->stack = (mu_htn_task*)MU_MALLOC(sizeof(mu_htn_task) *
                                       (size_t)max_depth * 16u);
    if(!h->stack)
    {
        MU_FREE(h->methods);
        h->methods = NULL;
        return false;
    }
    h->stack_count = 0;
    h->stack_cap = max_depth * 16u;

    h->plan = (mu_htn_task*)MU_MALLOC(sizeof(mu_htn_task) * 256u);
    if(!h->plan)
    {
        MU_FREE(h->methods);
        MU_FREE(h->stack);
        h->methods = NULL;
        h->stack = NULL;
        return false;
    }
    h->plan_count = 0;
    h->plan_cap = 256u;
    h->max_depth = max_depth;
    return true;
}

MU_INLINE void mu_htn_destroy(mu_htn* h)
{
    if(h == NULL)
        return;
    MU_FREE(h->methods);
    MU_FREE(h->stack);
    MU_FREE(h->plan);
    h->methods = NULL;
    h->stack = NULL;
    h->plan = NULL;
    h->method_count = h->method_cap = 0;
    h->stack_count = h->stack_cap = 0;
    h->plan_count = h->plan_cap = 0;
    h->max_depth = 0;
}

MU_INLINE void mu_htn_clear(mu_htn* h)
{
    MU_ASSERT(h != NULL);
    h->method_count = 0;
    h->plan_count = 0;
}

/* ---------- method registration ---------- */

/* Add a method. Returns false if at capacity. */
MU_INLINE bool mu_htn_add_method(mu_htn* h, const mu_htn_method* method)
{
    MU_ASSERT(h != NULL);
    MU_ASSERT(method != NULL);
    if(h->method_count >= h->method_cap)
        return false;
    h->methods[h->method_count++] = *method;
    return true;
}

/* ---------- planning ---------- */

/* Find a method matching `task` in the current world. Returns index or
 * MU_INVALID_INDEX. */
static int32_t mu_htn_find_method(const mu_htn* h, uint32_t task_id,
                                  const void* world)
{
    for(uint32_t i = 0; i < h->method_count; ++i)
    {
        const mu_htn_method* m = &h->methods[i];
        if(m->task_id != task_id)
            continue;
        bool ok = true;
        for(uint32_t c = 0; c < m->cond_count; ++c)
        {
            if(!h->iface->condition(world, m->cond_args[c]))
            {
                ok = false;
                break;
            }
        }
        if(ok)
            return (int32_t)i;
    }
    return MU_INVALID_INDEX;
}

/* Decompose `task` depth-first, appending primitive tasks to `plan`.
 * Returns true if a full decomposition to primitives was found. */
static bool mu_htn_decompose(mu_htn* h, const mu_htn_task* task,
                             void* world, uint32_t depth)
{
    if(depth > h->max_depth)
        return false;

    if(task->type == MU_HTN_PRIMITIVE)
    {
        /* append to plan */
        if(h->plan_count >= h->plan_cap)
        {
            uint32_t nc = h->plan_cap * 2u;
            mu_htn_task* np = (mu_htn_task*)MU_REALLOC(
                h->plan, sizeof(mu_htn_task) * nc);
            if(!np)
                return false;
            h->plan = np;
            h->plan_cap = nc;
        }
        h->plan[h->plan_count++] = *task;
        return true;
    }

    /* compound: find a matching method */
    int32_t mi = mu_htn_find_method(h, task->id, world);
    if(mi < 0)
        return false; /* no applicable method */

    const mu_htn_method* method = &h->methods[mi];

    /* apply subtasks in reverse order to a stack so they execute in order */
    /* We push subtasks onto the stack; the top is processed next. To preserve
     * order, push them in reverse. */
    uint32_t stack_mark = h->stack_count;
    for(uint32_t i = method->subtask_count; i > 0u; --i)
    {
        uint32_t si = i - 1u;
        if(h->stack_count >= h->stack_cap)
            return false;
        mu_htn_task st;
        st.type = MU_HTN_COMPOUND; /* will be resolved when popped */
        st.id = method->subtask_ids[si];
        st.arg_count = method->subtask_arg_counts[si];
        for(uint32_t a = 0; a < st.arg_count && a < MU_HTN_MAX_ARGS; ++a)
            st.args[a] = method->subtask_args[si][a];
        h->stack[h->stack_count++] = st;
    }

    bool ok = true;
    while(h->stack_count > stack_mark)
    {
        mu_htn_task cur = h->stack[--h->stack_count];
        /* resolve type: we don't know compound vs primitive until we check
         * methods; treat as compound if a method matches, else primitive. */
        int32_t m2 = mu_htn_find_method(h, cur.id, world);
        if(m2 >= 0)
        {
            /* re-push cur as a marker then expand method */
            /* Simpler: recurse */
            mu_htn_task ct = cur;
            if(!mu_htn_decompose(h, &ct, world, depth + 1u))
            {
                ok = false;
                break;
            }
        }
        else
        {
            /* primitive -> append to plan */
            if(h->plan_count >= h->plan_cap)
            {
                uint32_t nc = h->plan_cap * 2u;
                mu_htn_task* np = (mu_htn_task*)MU_REALLOC(
                    h->plan, sizeof(mu_htn_task) * nc);
                if(!np) { ok = false; break; }
                h->plan = np;
                h->plan_cap = nc;
            }
            h->plan[h->plan_count++] = cur;
        }
    }

    return ok;
}

/* Plan: decompose `top_task` into a primitive task sequence in `h->plan`.
 * Returns true on success; plan is in h->plan[0..plan_count). */
MU_INLINE bool mu_htn_plan(mu_htn* h, const mu_htn_task* top_task,
                           void* world)
{
    MU_ASSERT(h != NULL);
    MU_ASSERT(top_task != NULL);
    h->plan_count = 0;
    h->stack_count = 0;
    return mu_htn_decompose(h, top_task, world, 0u);
}

/* Execute the current plan through the user interface. Returns false if
 * any task fails. */
MU_INLINE bool mu_htn_execute_plan(mu_htn* h, void* world)
{
    MU_ASSERT(h != NULL);
    for(uint32_t i = 0; i < h->plan_count; ++i)
    {
        const mu_htn_task* t = &h->plan[i];
        if(!h->iface->execute(world, t->id, t->args, t->arg_count))
            return false;
    }
    return true;
}

MU_END_EXTERN_C
