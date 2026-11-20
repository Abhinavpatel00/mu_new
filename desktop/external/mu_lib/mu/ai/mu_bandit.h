#pragma once

/*
===============================================================================

mu_bandit.h — online parameter tuning (multi-armed bandits).

The rule from the RTS micro-evolution research (ECSLBot, CIG 2014): tune the
knobs, don't hand-set them. Where mu_genetic tunes offline, bandits tune
online, in production, against the live reward stream.

- epsilon-greedy: explore with fixed probability.
- UCB1: optimism under uncertainty; sqrt(2 ln n / n_i) bonus.
- Thompson sampling: Beta posterior, sample-and-max (Box-Muller ->
  Marsaglia-Tsang Gamma -> Beta ratio).
- discounted UCB: non-stationary variant; counts decay so old evidence fades.

Rewards are in [0,1]. All randomness goes through the caller's rng state
(xorshift32, advances in place) — no globals, bit-deterministic given a seed.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

#define MU_BANDIT_MAX_ARMS 32

typedef enum mu_bandit_method
{
    MU_BANDIT_EPS_GREEDY = 0,
    MU_BANDIT_UCB1,
    MU_BANDIT_THOMPSON,
    MU_BANDIT_DISCOUNTED_UCB
} mu_bandit_method;

typedef struct mu_bandit
{
    mu_bandit_method method;
    int      n_arms;
    float    epsilon;      /* EPS_GREEDY                                   */
    float    discount;     /* DISCOUNTED_UCB: per-pull decay, e.g. 0.999   */
    uint32_t pulls[MU_BANDIT_MAX_ARMS];
    float    reward_sum[MU_BANDIT_MAX_ARMS];
    float    alpha[MU_BANDIT_MAX_ARMS];   /* Beta posterior, Thompson      */
    float    beta_[MU_BANDIT_MAX_ARMS];
    uint32_t total_pulls;
} mu_bandit;

/* ---------- caller-owned rng (xorshift32) ---------- */

MU_INLINE uint32_t mu_bandit_rand32(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x9E3779B9u;
    return *s;
}

MU_INLINE float mu_bandit_randf(uint32_t* s)
{
    return (float)(mu_bandit_rand32(s) >> 8) * (1.0f / 16777216.0f);
}

/* Box-Muller normal from two uniforms; deterministic, no cache. */
MU_INLINE float mu_bandit_normal(uint32_t* s)
{
    float u1 = mu_bandit_randf(s);
    float u2 = mu_bandit_randf(s);
    if (u1 < 1e-12f) u1 = 1e-12f;
    return sqrtf(-2.0f * logf(u1)) * cosf(6.28318530717958647692f * u2);
}

/* Marsaglia-Tsang Gamma(shape, 1). shape > 0. */
MU_INLINE float mu_bandit_gamma(float shape, uint32_t* s)
{
    float d, c, x, v, u;
    if (shape < 1.0f)
    {
        /* boost: Gamma(a) = Gamma(a+1) * U^(1/a) */
        float u = mu_bandit_randf(s);
        if (u < 1e-12f) u = 1e-12f;
        return mu_bandit_gamma(shape + 1.0f, s) * powf(u, 1.0f / shape);
    }
    d = shape - (1.0f / 3.0f);
    c = 1.0f / sqrtf(9.0f * d);
    for (;;)
    {
        x = mu_bandit_normal(s);
        v = 1.0f + c * x;
        if (v <= 0.0f) continue;
        v = v * v * v;
        u = mu_bandit_randf(s);
        if (u < 1.0f - 0.0331f * (x * x) * (x * x)) return d * v;
        if (logf(u) < 0.5f * x * x + d * (1.0f - v + logf(v))) return d * v;
    }
}

/* Beta(alpha, beta) sample: X/(X+Y), X~Gamma(alpha), Y~Gamma(beta). */
MU_INLINE float mu_bandit_sample_beta(float alpha, float beta_, uint32_t* s)
{
    float x, y;
    if (alpha <= 0.0f) alpha = 1e-3f;
    if (beta_ <= 0.0f) beta_ = 1e-3f;
    x = mu_bandit_gamma(alpha, s);
    y = mu_bandit_gamma(beta_, s);
    if (x + y <= 0.0f) return 0.5f;
    return x / (x + y);
}

/* ---------- lifecycle ---------- */

MU_INLINE void mu_bandit_init(mu_bandit* b, mu_bandit_method method,
                              int n_arms)
{
    int i;
    b->method = method;
    b->n_arms = (n_arms > MU_BANDIT_MAX_ARMS) ? MU_BANDIT_MAX_ARMS : n_arms;
    b->epsilon = 0.1f;
    b->discount = 0.999f;
    b->total_pulls = 0;
    for (i = 0; i < b->n_arms; ++i)
    {
        b->pulls[i] = 0;
        b->reward_sum[i] = 0.0f;
        b->alpha[i] = 1.0f;
        b->beta_[i] = 1.0f;
    }
}

MU_INLINE void mu_bandit_update(mu_bandit* b, int arm, float reward)
{
    if (arm < 0 || arm >= b->n_arms) return;
    if (reward < 0.0f) reward = 0.0f;
    if (reward > 1.0f) reward = 1.0f;
    if (b->method == MU_BANDIT_DISCOUNTED_UCB)
    {
        int i;
        float d = b->discount;
        for (i = 0; i < b->n_arms; ++i)
        {
            b->pulls[i] = (uint32_t)((float)b->pulls[i] * d);
            b->reward_sum[i] *= d;
        }
    }
    b->pulls[arm] += 1;
    b->reward_sum[arm] += reward;
    b->alpha[arm] += reward;
    b->beta_[arm] += 1.0f - reward;
    b->total_pulls += 1;
}

/* Choose an arm. `rng` is caller-owned xorshift32 state (advances in place). */
MU_INLINE int mu_bandit_choose(mu_bandit* b, uint32_t* rng)
{
    int i, best;
    float best_score;

    if (b->n_arms <= 0) return -1;
    if (b->n_arms == 1) return 0;

    switch (b->method)
    {
    case MU_BANDIT_EPS_GREEDY:
        if (mu_bandit_randf(rng) < b->epsilon)
            return (int)(mu_bandit_rand32(rng) % (uint32_t)b->n_arms);
        best = 0;
        best_score = -1.0f;
        for (i = 0; i < b->n_arms; ++i)
        {
            float mean = b->pulls[i] ? b->reward_sum[i] / (float)b->pulls[i] : 1e9f;
            if (mean > best_score) { best_score = mean; best = i; }
        }
        return best;

    case MU_BANDIT_THOMPSON:
        best = 0;
        best_score = -1.0f;
        for (i = 0; i < b->n_arms; ++i)
        {
            float s = mu_bandit_sample_beta(b->alpha[i], b->beta_[i], rng);
            if (s > best_score) { best_score = s; best = i; }
        }
        return best;

    case MU_BANDIT_UCB1:
    case MU_BANDIT_DISCOUNTED_UCB:
    default:
        for (i = 0; i < b->n_arms; ++i)   /* pull every arm once first */
            if (b->pulls[i] == 0) return i;
        best = 0;
        best_score = -1.0f;
        for (i = 0; i < b->n_arms; ++i)
        {
            float mean = b->reward_sum[i] / (float)b->pulls[i];
            float bonus = sqrtf(2.0f * logf((float)b->total_pulls + 1.0f) /
                                (float)b->pulls[i]);
            float score = mean + bonus;
            if (score > best_score) { best_score = score; best = i; }
        }
        return best;
    }
}

MU_END_EXTERN_C
