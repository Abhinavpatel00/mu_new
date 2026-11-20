#pragma once

/*
===============================================================================

mu_utility.h — utility AI scoring (response curves + consideration aggregation).

The scoring layer under behavior trees / HTN / GOAP: given N considerations,
each mapped through a response curve to [0,1], produce one desirability score.

- Response curves (Dave Mark's utility-theory shapes): linear, power, logistic,
  sine, step, inverse. Each maps a clamped input to [0,1].
- Aggregation: multiplicative (default — "insurmountable odds" semantics),
  min, or mean.
- Compensation: 1-(1-s)^k, the low-score rebalancer for product aggregation
  when the number of considerations makes scores collapse toward zero.
- Low-pass: score inertia (commitment) so an action already begun is sticky.

No allocation, no globals, float math only, bit-deterministic.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

#ifndef MU_UTILITY_PI
#define MU_UTILITY_PI 3.14159265358979323846f
#endif

typedef enum mu_curve_kind
{
    MU_CURVE_LINEAR = 0,   /* y = x                                     */
    MU_CURVE_POWER,        /* y = x^p0            (p0 > 0; 1 = linear)  */
    MU_CURVE_LOGISTIC,     /* y = sigmoid(x)      (p0 = midpoint, p1 = steepness) */
    MU_CURVE_SINE,         /* y = sin(x * pi/2)   (concave rise)        */
    MU_CURVE_STEP,         /* y = x >= p0                               */
    MU_CURVE_INVERSE       /* y = 1 - x                                 */
} mu_curve_kind;

typedef struct mu_curve
{
    mu_curve_kind kind;
    float p0;
    float p1;
} mu_curve;

MU_INLINE mu_curve mu_curve_make(mu_curve_kind kind, float p0, float p1)
{
    mu_curve c;
    c.kind = kind;
    c.p0 = p0;
    c.p1 = p1;
    return c;
}

/* Evaluate one curve. Input is clamped to [0,1]; output is in [0,1]. */
MU_INLINE float mu_curve_eval(const mu_curve* c, float x)
{
    float y;
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    switch (c->kind)
    {
    case MU_CURVE_POWER:
        y = (c->p0 == 1.0f) ? x : powf(x, c->p0);
        break;
    case MU_CURVE_LOGISTIC:
    {
        /* Normalized so f(0)=0, f(1)=1 regardless of midpoint/steepness. */
        float k = (c->p1 == 0.0f) ? 1.0f : c->p1;
        float m = c->p0;
        float s0 = 1.0f / (1.0f + expf(-k * (0.0f - m)));
        float s1 = 1.0f / (1.0f + expf(-k * (1.0f - m)));
        float sx = 1.0f / (1.0f + expf(-k * (x - m)));
        float r = s1 - s0;
        y = (r > 1e-6f) ? (sx - s0) / r : x;
        break;
    }
    case MU_CURVE_SINE:
        y = sinf(x * (MU_UTILITY_PI * 0.5f));
        break;
    case MU_CURVE_STEP:
        y = (x >= c->p0) ? 1.0f : 0.0f;
        break;
    case MU_CURVE_INVERSE:
        y = 1.0f - x;
        break;
    case MU_CURVE_LINEAR:
    default:
        y = x;
        break;
    }
    /* clamp output against float drift */
    if (y < 0.0f) y = 0.0f;
    if (y > 1.0f) y = 1.0f;
    return y;
}

/* ---------- aggregation ---------- */

MU_INLINE float mu_utility_aggregate_mul(const float* vals, int n)
{
    float p = 1.0f;
    int i;
    for (i = 0; i < n; ++i) p *= vals[i];
    return p;
}

MU_INLINE float mu_utility_aggregate_min(const float* vals, int n)
{
    float m = 1.0f;
    int i;
    for (i = 0; i < n; ++i) if (vals[i] < m) m = vals[i];
    return m;
}

MU_INLINE float mu_utility_aggregate_mean(const float* vals, int n)
{
    float s = 0.0f;
    int i;
    if (n <= 0) return 0.0f;
    for (i = 0; i < n; ++i) s += vals[i];
    return s / (float)n;
}

/* Compensation factor for product aggregation: k > 1 lifts low scores back up.
 * k = 1 is identity; typical choice is around the consideration count. */
MU_INLINE float mu_utility_compensate(float score, float k)
{
    if (score <= 0.0f) return 0.0f;
    if (score >= 1.0f) return 1.0f;
    if (k == 1.0f) return score;
    return 1.0f - powf(1.0f - score, k);
}

/* Score inertia: alpha in (0,1]; smaller = stickier to prev. */
MU_INLINE float mu_utility_lowpass(float prev, float cur, float alpha)
{
    return prev + alpha * (cur - prev);
}

/* One call: evaluate n (curve, input) pairs and aggregate.
 * `compensation` <= 1.0f disables compensation. */
MU_INLINE float mu_utility_score(const mu_curve* curves,
                                 const float* inputs, int n,
                                 float compensation)
{
    float vals[64];
    float agg;
    int i;
    if (n <= 0) return 0.0f;
    if (n > 64) n = 64;
    for (i = 0; i < n; ++i) vals[i] = mu_curve_eval(&curves[i], inputs[i]);
    agg = mu_utility_aggregate_mul(vals, n);
    if (compensation > 1.0f) agg = mu_utility_compensate(agg, compensation);
    return agg;
}

MU_END_EXTERN_C
