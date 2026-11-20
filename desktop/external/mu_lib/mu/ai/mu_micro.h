#pragma once

/*
===============================================================================

mu_micro.h — RTS reactive combat: the micro-management canon.

From the micro-evolution research (CIG 2014 ECSLBot; co-evolving micro
arXiv:1803.10314) and the SMAC benchmark (arXiv:1902.04043): focus fire beats
everything, naive focus fire overkills, kiting is three parameters, and
damaged units should leave the front line — but not oscillate.

- Future-damage ledger: committed-but-not-yet-applied damage per enemy slot;
  effective HP accounts for in-flight shots (overkill prevention).
- Target selection: kill-fastest = threat / time-to-kill, with damage-type
  matchup multipliers and an overkill guard.
- Kiting state machine: Hold -> Back (retreat D_kb) -> Pause (wait s_t) -> ...
  exactly the three documented knobs (D_k, s_t, D_kb) so a GA or mu_bandit
  can tune them.
- Flee hysteresis: flee below hp_flee, return only above hp_flee + hysteresis.

2D floats, no allocation, no globals. Movement deltas are outputs; you wire
them to mu_steering.

===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

#define MU_MICRO_MAX_UNITS 32

/* ---------- damage ledger ---------- */

typedef struct mu_damage_ledger
{
    float committed[MU_MICRO_MAX_UNITS]; /* indexed by enemy slot */
} mu_damage_ledger;

MU_INLINE void mu_ledger_clear(mu_damage_ledger* l)
{
    int i;
    for (i = 0; i < MU_MICRO_MAX_UNITS; ++i) l->committed[i] = 0.0f;
}

MU_INLINE void mu_ledger_commit(mu_damage_ledger* l, int enemy, float dmg)
{
    if (enemy < 0 || enemy >= MU_MICRO_MAX_UNITS) return;
    l->committed[enemy] += dmg;
}

MU_INLINE void mu_ledger_release(mu_damage_ledger* l, int enemy, float dmg)
{
    if (enemy < 0 || enemy >= MU_MICRO_MAX_UNITS) return;
    l->committed[enemy] -= dmg;
    if (l->committed[enemy] < 0.0f) l->committed[enemy] = 0.0f;
}

MU_INLINE float mu_ledger_effective_hp(const mu_damage_ledger* l,
                                       int enemy, float hp)
{
    float e = hp;
    if (enemy < 0 || enemy >= MU_MICRO_MAX_UNITS) return hp;
    e -= l->committed[enemy];
    return (e < 0.0f) ? 0.0f : e;
}

/* ---------- target selection ---------- */

/* Kill-fastest heuristic (documented Bayesian-RTS line): prefer enemies that
 * die fastest under our fire, weighted by the threat they put out.
 * Returns enemy slot or -1. Arrays are length n. */
MU_INLINE int mu_micro_select_target(const float* enemy_hp,
                                     const float* enemy_dps,
                                     const float* matchup,  /* our dmg mult */
                                     const float* dist,
                                     int n, const mu_damage_ledger* ledger)
{
    int i, best = -1;
    float best_score = -1.0f;
    for (i = 0; i < n && i < MU_MICRO_MAX_UNITS; ++i)
    {
        float eff_hp, ttk, score;
        if (enemy_hp[i] <= 0.0f) continue;
        eff_hp = mu_ledger_effective_hp(ledger, i, enemy_hp[i]);
        if (eff_hp <= 0.0f) continue;              /* already doomed — skip */
        ttk = eff_hp / (matchup[i] > 0.01f ? matchup[i] : 0.01f);
        score = enemy_dps[i] / (ttk + 0.05f) - 0.01f * dist[i];
        if (score > best_score) { best_score = score; best = i; }
    }
    return best;
}

/* ---------- kiting (the three-parameter state machine) ---------- */

typedef enum mu_kite_state
{
    MU_KITE_HOLD = 0,   /* outside D_k: close in   */
    MU_KITE_BACK,       /* inside D_k: retreat D_kb */
    MU_KITE_PAUSE       /* after firing: wait s_t    */
} mu_kite_state;

typedef struct mu_kite
{
    mu_kite_state state;
    float D_k;     /* begin kiting when target closer than this */
    float s_t;     /* wait after firing before moving           */
    float D_kb;    /* retreat distance before firing again      */
    float moved;   /* distance retreated so far in BACK         */
    float pause_t; /* time spent in PAUSE                       */
} mu_kite;

MU_INLINE void mu_kite_init(mu_kite* k, float D_k, float s_t, float D_kb)
{
    k->state = MU_KITE_HOLD;
    k->D_k = D_k;
    k->s_t = s_t;
    k->D_kb = D_kb;
    k->moved = 0.0f;
    k->pause_t = 0.0f;
}

/* Tick the machine.
 *   dist        — distance to target
 *   can_fire    — weapon ready this tick
 *   dt          — seconds
 *   out_move    — signed meters to move ALONG the away-axis this tick
 *                 (positive = retreat); caller applies via mu_steering.
 * Returns 1 if the caller should fire this tick. */
MU_INLINE int mu_kite_tick(mu_kite* k, float dist, int can_fire,
                           float dt, float* out_move)
{
    *out_move = 0.0f;
    switch (k->state)
    {
    case MU_KITE_HOLD:
        if (dist < k->D_k)
        {
            k->state = MU_KITE_BACK;
            k->moved = 0.0f;
        }
        break;

    case MU_KITE_BACK:
        if (k->moved < k->D_kb)
        {
            float step = k->D_kb - k->moved;
            *out_move = step;           /* retreat the remaining distance */
            k->moved += step;
        }
        else if (can_fire)
        {
            k->state = MU_KITE_PAUSE;
            k->pause_t = 0.0f;
            return 1;                   /* fire! */
        }
        break;

    case MU_KITE_PAUSE:
        k->pause_t += dt;
        if (k->pause_t >= k->s_t)
        {
            k->state = MU_KITE_HOLD;
            k->moved = 0.0f;
        }
        break;
    }
    return 0;
}

/* ---------- flee hysteresis ---------- */

typedef struct mu_flee
{
    int   fleeing;
    float threshold;    /* flee when hp_frac < threshold            */
    float hysteresis;   /* return only when hp_frac > threshold + h */
} mu_flee;

MU_INLINE void mu_flee_init(mu_flee* f, float threshold, float hysteresis)
{
    f->fleeing = 0;
    f->threshold = threshold;
    f->hysteresis = hysteresis;
}

/* Returns 1 while the unit should run. */
MU_INLINE int mu_flee_tick(mu_flee* f, float hp_frac)
{
    if (!f->fleeing && hp_frac < f->threshold)
        f->fleeing = 1;
    else if (f->fleeing && hp_frac > f->threshold + f->hysteresis)
        f->fleeing = 0;
    return f->fleeing;
}

MU_END_EXTERN_C
