#pragma once

/*
===============================================================================

mu_playermodel.h — behavioral feature statistics and drift detection.

The player-modeling pipeline (Yannakakis/Spronck/Loiacono/André, Dagstuhl
2013; Yannakakis & Togelius 2025 Ch. 10-12), minus the ML: observe gameplay
features per tick, keep running statistics, and notice when the player
*changes*.

- Per-feature Welford mean/variance (numerically stable) + EMA for recency.
- z-score: how unusual is the current behavior vs. this player's baseline.
- CUSUM-lite drift detection: cumulative sum of deviations past a margin,
  with a threshold — trips on sustained mean shifts, ignores noise.
- 8 feature slots (aggression, stealth, greed, risk, pace, ...), caller-named
  semantics.

Consumers: mu_director (DDA), mu_bandit (per-regime tuning). No allocation.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

#define MU_PM_FEATURES 8

typedef struct mu_playermodel
{
    struct
    {
        uint64_t n;       /* observation count                       */
        double   mean;    /* Welford running mean                    */
        double   m2;      /* Welford running sum of squares          */
        float    ema;     /* exponential moving average              */
        float    ema_alpha;
        /* CUSUM drift */
        float    cusum_pos;
        float    cusum_neg;
        float    baseline;/* frozen mean at reset_drift              */
    } f[MU_PM_FEATURES];
} mu_playermodel;

MU_INLINE void mu_playermodel_init(mu_playermodel* pm)
{
    int i;
    for (i = 0; i < MU_PM_FEATURES; ++i)
    {
        pm->f[i].n = 0;
        pm->f[i].mean = 0.0;
        pm->f[i].m2 = 0.0;
        pm->f[i].ema = 0.0f;
        pm->f[i].ema_alpha = 0.05f;
        pm->f[i].cusum_pos = 0.0f;
        pm->f[i].cusum_neg = 0.0f;
        pm->f[i].baseline = 0.0f;
    }
}

MU_INLINE void mu_playermodel_observe(mu_playermodel* pm, int feature, float x)
{
    double delta;
    if (feature < 0 || feature >= MU_PM_FEATURES) return;
    pm->f[feature].n += 1;
    delta = (double)x - pm->f[feature].mean;
    pm->f[feature].mean += delta / (double)pm->f[feature].n;
    pm->f[feature].m2 += delta * ((double)x - pm->f[feature].mean);
    pm->f[feature].ema += pm->f[feature].ema_alpha * (x - pm->f[feature].ema);

    /* CUSUM against frozen baseline */
    {
        float dev = x - pm->f[feature].baseline;
        pm->f[feature].cusum_pos += dev;
        pm->f[feature].cusum_neg -= dev;
        if (pm->f[feature].cusum_pos < 0.0f) pm->f[feature].cusum_pos = 0.0f;
        if (pm->f[feature].cusum_neg < 0.0f) pm->f[feature].cusum_neg = 0.0f;
    }
}

MU_INLINE float mu_playermodel_mean(const mu_playermodel* pm, int feature)
{
    if (feature < 0 || feature >= MU_PM_FEATURES) return 0.0f;
    return (float)pm->f[feature].mean;
}

MU_INLINE float mu_playermodel_variance(const mu_playermodel* pm, int feature)
{
    if (feature < 0 || feature >= MU_PM_FEATURES) return 0.0f;
    if (pm->f[feature].n < 2) return 0.0f;
    return (float)(pm->f[feature].m2 / (double)(pm->f[feature].n - 1));
}

MU_INLINE float mu_playermodel_std(const mu_playermodel* pm, int feature)
{
    return sqrtf(mu_playermodel_variance(pm, feature));
}

/* z-score of a fresh sample vs the current model. */
MU_INLINE float mu_playermodel_z(const mu_playermodel* pm, int feature, float x)
{
    float sd = mu_playermodel_std(pm, feature);
    if (sd < 1e-6f) return 0.0f;
    return (x - mu_playermodel_mean(pm, feature)) / sd;
}

/* Freeze the baseline (e.g. after onboarding). */
MU_INLINE void mu_playermodel_reset_drift(mu_playermodel* pm, int feature)
{
    if (feature < 0 || feature >= MU_PM_FEATURES) return;
    pm->f[feature].baseline = (float)pm->f[feature].mean;
    pm->f[feature].cusum_pos = 0.0f;
    pm->f[feature].cusum_neg = 0.0f;
}

/* Drift check: +1 sustained up, -1 sustained down, 0 none. */
MU_INLINE int mu_playermodel_drift(const mu_playermodel* pm, int feature,
                                   float threshold)
{
    if (feature < 0 || feature >= MU_PM_FEATURES) return 0;
    if (pm->f[feature].cusum_pos > threshold) return 1;
    if (pm->f[feature].cusum_neg > threshold) return -1;
    return 0;
}

MU_END_EXTERN_C
