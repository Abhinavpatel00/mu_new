#pragma once

/*
===============================================================================

mu_director.h — pacing and tension: the AI that manages the *experience*.

Left 4 Dead's AI Director (GDC 2009), Alien: Isolation's menace gauge, and
RimWorld's storytellers are the same machine: watch the player's state, hold
tension on a rising/falling curve, fire events at the peaks, and never let
either the valley or the grind last too long.

- Pacing formula (documented L4D rule): next event in
      t = (a * k + b) / (tension + c)
  where k is a difficulty scalar; larger tension => sooner events.
- Watchers: feed wellbeing/threat scalars; tension tracks them with a
  low-pass and drifts toward a plateau (the menace-gauge medium band).
- Event selection: weighted picks with per-event cooldowns; `relief` events
  are biased up when tension is at/above max_stress (the peak -> vent valve),
  `threat` events are biased up in the valleys (progress beats).
- Presets: Cassandra (paced cycles), Randy (high weight chaos), Phoebe
  (long valleys) — storyteller personalities as parameter sets.

Deterministic: rng state is caller-owned xorshift32.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

#define MU_DIRECTOR_MAX_EVENTS 16

typedef enum mu_director_event_kind
{
    MU_DIRECTOR_THREAT = 0,   /* pressure: raids, monsters, snipers  */
    MU_DIRECTOR_RELIEF,       /* release: loot, safe room, allies    */
    MU_DIRECTOR_AMBIENT       /* flavor: scenery, sound beats        */
} mu_director_event_kind;

typedef struct mu_director_event
{
    float weight;            /* base pick weight                          */
    float cooldown;          /* seconds between fires                     */
    float since_fire;        /* internal timer                            */
    mu_director_event_kind kind;
    float tension_delta;     /* applied on fire: threat +, relief -       */
} mu_director_event;

typedef struct mu_director
{
    float tension;           /* [0,1]                                     */
    float plateau;           /* target tension the curve drifts toward    */
    float a, b, c;           /* pacing formula constants                  */
    float k;                 /* difficulty scalar                         */
    float min_rest;          /* hard floor between any two events (s)     */
    float max_stress;        /* at/above: relief bias                     */
    float since_event;       /* seconds since the last event fired        */
    float watcher_alpha;     /* low-pass for watcher feeds                */
    mu_director_event events[MU_DIRECTOR_MAX_EVENTS];
    int   n_events;
    uint32_t rng_state;
} mu_director;

MU_INLINE uint32_t mu_director_rand32(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x9E3779B9u;
    return *s;
}

MU_INLINE float mu_director_randf(uint32_t* s)
{
    return (float)(mu_director_rand32(s) >> 8) * (1.0f / 16777216.0f);
}

/* ---------- presets (storyteller personalities) ---------- */

MU_INLINE void mu_director_init_preset(mu_director* d, int personality,
                                       uint32_t seed)
{
    d->tension = 0.3f;
    d->plateau = 0.5f;
    d->k = 1.0f;
    d->min_rest = 20.0f;
    d->max_stress = 0.8f;
    d->since_event = 0.0f;
    d->watcher_alpha = 0.1f;
    d->n_events = 0;
    d->rng_state = seed ? seed : 0xA341316Cu;
    switch (personality)
    {
    case 1: /* Randy Random: loud, frequent, low structure */
        d->a = 40.0f; d->b = 10.0f; d->c = 0.15f;
        d->plateau = 0.55f;
        d->min_rest = 8.0f;
        break;
    case 2: /* Phoebe Chillax: long valleys for builders */
        d->a = 120.0f; d->b = 60.0f; d->c = 0.3f;
        d->plateau = 0.35f;
        d->min_rest = 45.0f;
        break;
    case 0:
    default: /* Cassandra Classic: paced cycles */
        d->a = 70.0f; d->b = 25.0f; d->c = 0.25f;
        break;
    }
}

MU_INLINE int mu_director_add_event(mu_director* d, float weight,
                                    float cooldown,
                                    mu_director_event_kind kind,
                                    float tension_delta)
{
    mu_director_event* e;
    if (d->n_events >= MU_DIRECTOR_MAX_EVENTS) return -1;
    e = &d->events[d->n_events++];
    e->weight = weight;
    e->cooldown = cooldown;
    e->since_fire = 1e9f; /* ready immediately */
    e->kind = kind;
    e->tension_delta = tension_delta;
    return d->n_events - 1;
}

/* ---------- watchers ---------- */

/* Feed player-state scalars in [0,1]. wellbeing: 1 = thriving, 0 = dying. */
MU_INLINE void mu_director_feed(mu_director* d, float wellbeing,
                                float threat, float dt)
{
    float target = (1.0f - wellbeing) * 0.7f + threat * 0.3f;
    float alpha = d->watcher_alpha * (dt > 0.0f ? dt * 60.0f : 1.0f);
    if (alpha > 1.0f) alpha = 1.0f;
    d->tension += alpha * (target - d->tension);
    /* plateau drift: the menace-gauge medium band */
    d->tension += 0.01f * (d->plateau - d->tension);
    if (d->tension < 0.0f) d->tension = 0.0f;
    if (d->tension > 1.0f) d->tension = 1.0f;
    if (dt > 0.0f)
    {
        int i;
        d->since_event += dt;
        for (i = 0; i < d->n_events; ++i) d->events[i].since_fire += dt;
    }
}

/* ---------- pacing ---------- */

/* Seconds until the next event under the pacing formula. */
MU_INLINE float mu_director_time_to_event(const mu_director* d)
{
    float t = (d->a * d->k + d->b) / (d->tension + d->c);
    if (t < d->min_rest) t = d->min_rest;
    return t;
}

/* Should an event fire now? Advances the internal clock. */
MU_INLINE int mu_director_should_fire(mu_director* d, float dt)
{
    float t;
    (void)dt;
    if (d->since_event < d->min_rest) return 0;
    t = mu_director_time_to_event(d);
    return (d->since_event >= t) ? 1 : 0;
}

/* Pick a ready event index (weighted, kind-biased by tension). -1 = none. */
MU_INLINE int mu_director_pick(mu_director* d)
{
    float total = 0.0f, r, acc = 0.0f;
    int i, ready = 0;
    for (i = 0; i < d->n_events; ++i)
    {
        if (d->events[i].since_fire < d->events[i].cooldown) continue;
        ++ready;
    }
    if (ready == 0) return -1;
    for (i = 0; i < d->n_events; ++i)
    {
        float w;
        if (d->events[i].since_fire < d->events[i].cooldown) continue;
        w = d->events[i].weight;
        if (d->tension >= d->max_stress && d->events[i].kind == MU_DIRECTOR_RELIEF)
            w *= 3.0f;                     /* peak -> vent valve            */
        if (d->tension < 0.35f && d->events[i].kind == MU_DIRECTOR_THREAT)
            w *= 1.6f;                     /* valley -> progress pressure   */
        total += w;
    }
    if (total <= 0.0f) return -1;
    r = mu_director_randf(&d->rng_state) * total;
    for (i = 0; i < d->n_events; ++i)
    {
        float w;
        if (d->events[i].since_fire < d->events[i].cooldown) continue;
        w = d->events[i].weight;
        if (d->tension >= d->max_stress && d->events[i].kind == MU_DIRECTOR_RELIEF)
            w *= 3.0f;
        if (d->tension < 0.35f && d->events[i].kind == MU_DIRECTOR_THREAT)
            w *= 1.6f;
        acc += w;
        if (r <= acc) return i;
    }
    return -1;
}

/* After firing event `idx`: reset clocks, apply its tension delta. */
MU_INLINE void mu_director_notify_fired(mu_director* d, int idx)
{
    if (idx < 0 || idx >= d->n_events) return;
    d->events[idx].since_fire = 0.0f;
    d->since_event = 0.0f;
    d->tension += d->events[idx].tension_delta;
    if (d->tension < 0.0f) d->tension = 0.0f;
    if (d->tension > 1.0f) d->tension = 1.0f;
}

MU_END_EXTERN_C
