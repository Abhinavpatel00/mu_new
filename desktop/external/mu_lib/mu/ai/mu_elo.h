#pragma once

/*
===============================================================================

mu_elo.h — skill rating (Elo + win probability, TrueSkill-lite team sums).

Rate the bots against each other and against baselines in the roguelike gate:
who is actually better is data, not opinion.

- Classic Elo update with configurable K and expected-score logistic (base 10).
- K-schedule: high K for young ratings, decaying after `decay_games`.
- Team play: sum of member ratings vs sum of opponent ratings (the common
  approximation; true TrueSkill factor graphs are out of scope here).
- Win probability from rating difference — the number you actually report.

No allocation, no globals.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

/* Expected score of A against B, in [0,1]. */
MU_INLINE float mu_elo_expected(float ra, float rb)
{
    return 1.0f / (1.0f + powf(10.0f, (rb - ra) / 400.0f));
}

/* Team variant: sum of ratings. */
MU_INLINE float mu_elo_team_expected(const float* ra, int na,
                                     const float* rb, int nb)
{
    float sa = 0.0f, sb = 0.0f;
    int i;
    for (i = 0; i < na; ++i) sa += ra[i];
    for (i = 0; i < nb; ++i) sb += rb[i];
    return mu_elo_expected(sa, sb);
}

/* K-factor schedule: k0 until decay_games, then halves every decay_games. */
MU_INLINE float mu_elo_k_schedule(float k0, uint32_t games, uint32_t decay_games)
{
    float k = k0;
    uint32_t g = games;
    if (decay_games == 0) return k;
    while (g >= decay_games)
    {
        k *= 0.5f;
        g -= decay_games;
        if (k < 1.0f) { k = 1.0f; break; }
    }
    return k;
}

/* One rated game. score_a: 1 win, 0.5 draw, 0 loss for A. */
MU_INLINE void mu_elo_update(float* ra, float* rb, float score_a, float k)
{
    float ea = mu_elo_expected(*ra, *rb);
    *ra += k * (score_a - ea);
    *rb += k * ((1.0f - score_a) - (1.0f - ea));
}

/* Win probability of A over B (expected score under the logistic model). */
MU_INLINE float mu_elo_win_probability(float ra, float rb)
{
    return mu_elo_expected(ra, rb);
}

MU_END_EXTERN_C
