#pragma once

/*
===============================================================================

mu_petri.h — Petri net: places, transitions, pre/post arcs.

- Places hold token counts (u32).
- Transitions have input arcs (place -> count consumed) and output arcs
  (place -> count produced).
- A transition is enabled when every input place has enough tokens.
- Firing consumes input tokens and produces output tokens.
- Reachability: can a target marking be reached (bounded search)?

Flat arrays; canonical surface init/destroy/add_place/add_transition/
add_arc/enabled/fire/reachable.

===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

typedef struct mu_petri_arc
{
    uint32_t place;   /* place index */
    uint32_t count;   /* token count */
} mu_petri_arc;

typedef struct mu_petri_transition
{
    mu_petri_arc* pre;     /* input arcs  */
    uint32_t      pre_count;
    uint32_t      pre_cap;

    mu_petri_arc* post;    /* output arcs */
    uint32_t      post_count;
    uint32_t      post_cap;
} mu_petri_transition;

typedef struct mu_petri_net
{
    uint32_t*         marking;   /* [place_count] token counts */
    uint32_t          place_count;

    mu_petri_transition* transitions;
    uint32_t             transition_count;
    uint32_t             transition_cap;
} mu_petri_net;

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_petri_init(mu_petri_net* net, uint32_t place_count,
                             uint32_t max_transitions)
{
    MU_ASSERT(net != NULL);
    MU_ASSERT(place_count > 0u && max_transitions > 0u);

    net->marking = (uint32_t*)MU_CALLOC(place_count, sizeof(uint32_t));
    if(!net->marking)
        return false;

    net->transitions = (mu_petri_transition*)MU_CALLOC(
        max_transitions, sizeof(mu_petri_transition));
    if(!net->transitions)
    {
        MU_FREE(net->marking);
        net->marking = NULL;
        return false;
    }

    net->place_count = place_count;
    net->transition_count = 0;
    net->transition_cap = max_transitions;
    return true;
}

MU_INLINE void mu_petri_destroy(mu_petri_net* net)
{
    if(net == NULL)
        return;
    for(uint32_t i = 0; i < net->transition_count; ++i)
    {
        MU_FREE(net->transitions[i].pre);
        MU_FREE(net->transitions[i].post);
    }
    MU_FREE(net->transitions);
    MU_FREE(net->marking);
    net->transitions = NULL;
    net->marking = NULL;
    net->place_count = 0;
    net->transition_count = net->transition_cap = 0;
}

MU_INLINE void mu_petri_clear(mu_petri_net* net)
{
    MU_ASSERT(net != NULL);
    MU_MEMSET(net->marking, 0, sizeof(uint32_t) * net->place_count);
    net->transition_count = 0;
}

MU_INLINE uint32_t mu_petri_place_count(const mu_petri_net* net)
{
    return net->place_count;
}

MU_INLINE uint32_t mu_petri_tokens(const mu_petri_net* net, uint32_t place)
{
    MU_ASSERT(place < net->place_count);
    return net->marking[place];
}

MU_INLINE void mu_petri_set_tokens(mu_petri_net* net, uint32_t place,
                                   uint32_t tokens)
{
    MU_ASSERT(place < net->place_count);
    net->marking[place] = tokens;
}

MU_INLINE void mu_petri_add_tokens(mu_petri_net* net, uint32_t place,
                                   uint32_t count)
{
    MU_ASSERT(place < net->place_count);
    net->marking[place] += count;
}

/* ---------- transitions / arcs ---------- */

MU_INLINE bool mu_petri_add_transition(mu_petri_net* net, uint32_t* out_index)
{
    MU_ASSERT(net != NULL);
    MU_ASSERT(out_index != NULL);
    if(net->transition_count >= net->transition_cap)
        return false;
    uint32_t i = net->transition_count++;
    mu_petri_transition* t = &net->transitions[i];
    t->pre = NULL;
    t->pre_count = 0;
    t->pre_cap = 0;
    t->post = NULL;
    t->post_count = 0;
    t->post_cap = 0;
    *out_index = i;
    return true;
}

static bool mu_petri_arc_push(mu_petri_arc** arr, uint32_t* count,
                              uint32_t* cap, uint32_t place, uint32_t cnt)
{
    if(*count == *cap)
    {
        uint32_t nc = *cap ? *cap * 2u : 4u;
        mu_petri_arc* na = (mu_petri_arc*)MU_REALLOC(*arr,
                                                     sizeof(mu_petri_arc) * nc);
        if(!na)
            return false;
        *arr = na;
        *cap = nc;
    }
    (*arr)[*count].place = place;
    (*arr)[*count].count = cnt;
    ++(*count);
    return true;
}

MU_INLINE bool mu_petri_add_pre_arc(mu_petri_net* net, uint32_t transition,
                                    uint32_t place, uint32_t count)
{
    MU_ASSERT(net != NULL);
    MU_ASSERT(transition < net->transition_count);
    MU_ASSERT(count > 0u);
    return mu_petri_arc_push(&net->transitions[transition].pre,
                             &net->transitions[transition].pre_count,
                             &net->transitions[transition].pre_cap,
                             place, count);
}

MU_INLINE bool mu_petri_add_post_arc(mu_petri_net* net, uint32_t transition,
                                     uint32_t place, uint32_t count)
{
    MU_ASSERT(net != NULL);
    MU_ASSERT(transition < net->transition_count);
    MU_ASSERT(count > 0u);
    return mu_petri_arc_push(&net->transitions[transition].post,
                             &net->transitions[transition].post_count,
                             &net->transitions[transition].post_cap,
                             place, count);
}

/* ---------- query / fire ---------- */

/* Return true if transition is enabled (all inputs have enough tokens). */
MU_INLINE bool mu_petri_enabled(const mu_petri_net* net, uint32_t transition)
{
    MU_ASSERT(net != NULL);
    MU_ASSERT(transition < net->transition_count);
    const mu_petri_transition* t = &net->transitions[transition];
    for(uint32_t i = 0; i < t->pre_count; ++i)
    {
        if(net->marking[t->pre[i].place] < t->pre[i].count)
            return false;
    }
    return true;
}

/* Fire transition if enabled. Returns false if not enabled. */
MU_INLINE bool mu_petri_fire(mu_petri_net* net, uint32_t transition)
{
    MU_ASSERT(net != NULL);
    MU_ASSERT(transition < net->transition_count);
    if(!mu_petri_enabled(net, transition))
        return false;

    mu_petri_transition* t = &net->transitions[transition];

    /* consume inputs */
    for(uint32_t i = 0; i < t->pre_count; ++i)
        net->marking[t->pre[i].place] -= t->pre[i].count;
    /* produce outputs */
    for(uint32_t i = 0; i < t->post_count; ++i)
        net->marking[t->post[i].place] += t->post[i].count;
    return true;
}

/* Fire any enabled transition (first found). Returns transition index or
 * MU_INVALID_INDEX if none enabled. */
MU_INLINE uint32_t mu_petri_fire_any(mu_petri_net* net)
{
    MU_ASSERT(net != NULL);
    for(uint32_t i = 0; i < net->transition_count; ++i)
    {
        if(mu_petri_enabled(net, i))
        {
            mu_petri_fire(net, i);
            return i;
        }
    }
    return MU_INVALID_INDEX;
}

/* ---------- reachability (bounded BFS over markings) ---------- */

/* Encode a marking into a flat u64 fingerprint for the visited set (places
 * packed 4 bits each; supports up to 16 places with counts 0..15). For
 * larger nets the caller should use a custom visited set. */
typedef struct mu_petri_reach
{
    uint64_t* visited;
    uint32_t  visited_count;
    uint32_t  visited_cap;
} mu_petri_reach;

MU_INLINE bool mu_petri_reachable(mu_petri_net* net,
                                  const uint32_t* target,
                                  uint32_t max_steps)
{
    MU_ASSERT(net != NULL);
    MU_ASSERT(target != NULL);

    /* Only support the fingerprint path for <= 16 places with small counts;
     * otherwise fall back to a simple step-limited fire-any search which is
     * not a true reachability check but bounds the work. */
    if(net->place_count > 16u)
    {
        /* step-limited exploration: fire any enabled transition up to
         * max_steps times, checking target each time. Not exhaustive. */
        uint32_t steps = 0;
        while(steps < max_steps)
        {
            bool match = true;
            for(uint32_t i = 0; i < net->place_count; ++i)
            {
                if(net->marking[i] != target[i])
                {
                    match = false;
                    break;
                }
            }
            if(match)
                return true;
            uint32_t fired = mu_petri_fire_any(net);
            if(fired == MU_INVALID_INDEX)
                break;
            ++steps;
        }
        bool match = true;
        for(uint32_t i = 0; i < net->place_count; ++i)
        {
            if(net->marking[i] != target[i])
            {
                match = false;
                break;
            }
        }
        return match;
    }

    /* BFS with fingerprint visited set. */
    /* Queue of markings stored as u64 fingerprints. */
    uint64_t* queue = (uint64_t*)MU_MALLOC(sizeof(uint64_t) * 4096u);
    if(!queue)
        return false;
    uint32_t head = 0, tail = 0;

    /* current marking fingerprint */
    {
        uint64_t fp = 0;
        for(uint32_t i = 0; i < net->place_count; ++i)
        {
            uint32_t t = net->marking[i];
            if(t > 15u) t = 15u; /* saturate */
            fp |= (uint64_t)t << (i * 4u);
        }
        queue[tail++] = fp;
    }

    /* visited set: simple open addressing over u64 */
    uint32_t vcap = 4096u;
    uint64_t* visited = (uint64_t*)MU_MALLOC(sizeof(uint64_t) * vcap);
    if(!visited)
    {
        MU_FREE(queue);
        return false;
    }
    uint32_t vcount = 0;

    /* target fingerprint */
    uint64_t target_fp = 0;
    for(uint32_t i = 0; i < net->place_count; ++i)
    {
        uint32_t t = target[i];
        if(t > 15u) t = 15u;
        target_fp |= (uint64_t)t << (i * 4u);
    }

    bool found = false;
    while(head < tail && !found)
    {
        uint64_t cur_fp = queue[head++];
        if(cur_fp == target_fp)
        {
            found = true;
            break;
        }
        /* membership check */
        bool seen = false;
        for(uint32_t v = 0; v < vcount; ++v)
        {
            if(visited[v] == cur_fp)
            {
                seen = true;
                break;
            }
        }
        if(seen)
            continue;
        if(vcount >= vcap)
        {
            /* grow */
            uint32_t nc = vcap * 2u;
            uint64_t* nv = (uint64_t*)MU_REALLOC(visited, sizeof(uint64_t) * nc);
            if(!nv) break;
            visited = nv;
            vcap = nc;
        }
        visited[vcount++] = cur_fp;

        /* restore marking from fingerprint */
        uint32_t saved[16];
        for(uint32_t i = 0; i < net->place_count; ++i)
            saved[i] = net->marking[i];
        for(uint32_t i = 0; i < net->place_count; ++i)
            net->marking[i] = (uint32_t)((cur_fp >> (i * 4u)) & 0xFu);

        /* expand: fire every enabled transition */
        for(uint32_t t = 0; t < net->transition_count && !found; ++t)
        {
            if(mu_petri_enabled(net, t))
            {
                uint32_t before[16];
                for(uint32_t i = 0; i < net->place_count; ++i)
                    before[i] = net->marking[i];
                mu_petri_fire(net, t);
                uint64_t nfp = 0;
                for(uint32_t i = 0; i < net->place_count; ++i)
                {
                    uint32_t v = net->marking[i];
                    if(v > 15u) v = 15u;
                    nfp |= (uint64_t)v << (i * 4u);
                }
                /* push if not visited */
                bool seen2 = false;
                for(uint32_t v = 0; v < vcount; ++v)
                {
                    if(visited[v] == nfp)
                    {
                        seen2 = true;
                        break;
                    }
                }
                if(!seen2 && tail < 4096u)
                    queue[tail++] = nfp;
                /* restore */
                for(uint32_t i = 0; i < net->place_count; ++i)
                    net->marking[i] = before[i];
            }
        }
        /* restore original marking */
        for(uint32_t i = 0; i < net->place_count; ++i)
            net->marking[i] = saved[i];
    }

    MU_FREE(queue);
    MU_FREE(visited);
    return found;
}

MU_END_EXTERN_C
