#pragma once

/*
===============================================================================

mu_mood.h — EMA-lite appraisal: events -> emotions -> mood.

The emotional layer under utility scores and directors (OCC 1988; Marsella &
Gratch's EMA, simplified hard). Six occurrence/emotion slots, a PAD-lite mood
that low-passes them, and decay so nothing sticks forever.

- Appraisal inputs: goal relevance, desirability (-1..1), likelihood (0..1),
  agency_self (0..1). EMA's rule set, trimmed to the cases that ship:
    joy       <- desirability of a *concerned* event
    distress  <- undesirability
    hope      <- desirable + uncertain
    fear      <- undesirable + uncertain
    satisfaction <- desirable + confirmed   (likelihood had been high)
    disappointment <- desirable + denied    (likelihood had been low)
- Mood: (valence, arousal, dominance) low-pass of the emotion vector.
- Consumers: mu_utility (bias considerations by mood), mu_director (pacing).

No allocation, no globals, decay-by-dt (frame-rate independent).

===============================================================================
*/

#include "mu_common.h"

MU_BEGIN_EXTERN_C

typedef enum mu_emotion_slot
{
    MU_EMOTION_JOY = 0,
    MU_EMOTION_DISTRESS,
    MU_EMOTION_HOPE,
    MU_EMOTION_FEAR,
    MU_EMOTION_SATISFACTION,
    MU_EMOTION_DISAPPOINTMENT,
    MU_EMOTION_COUNT
} mu_emotion_slot;

typedef struct mu_mood
{
    float emotion[MU_EMOTION_COUNT];  /* intensity in [0,1]                */
    float valence;                    /* mood: unpleasant .. pleasant      */
    float arousal;                    /* mood: calm .. activated           */
    float dominance;                  /* mood: submissive .. in-control    */
    float emotion_decay;              /* per-second decay rate toward 0    */
    float mood_alpha;                 /* low-pass for mood, e.g. 0.05      */
} mu_mood;

MU_INLINE void mu_mood_init(mu_mood* m)
{
    int i;
    for (i = 0; i < MU_EMOTION_COUNT; ++i) m->emotion[i] = 0.0f;
    m->valence = 0.0f;
    m->arousal = 0.0f;
    m->dominance = 0.0f;
    m->emotion_decay = 0.25f;
    m->mood_alpha = 0.05f;
}

MU_INLINE float mu_clampf(float x, float lo, float hi)
{
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* One appraisal step (EMA-lite). All inputs scaled roughly to [-1,1] or [0,1].
 * `intensity` scales the whole update (event magnitude). */
MU_INLINE void mu_mood_appraise(mu_mood* m,
                                float goal_relevance,   /* [0,1]  */
                                float desirability,     /* [-1,1] */
                                float likelihood,       /* [0,1]  */
                                float agency_self,      /* [0,1]  */
                                float intensity)        /* [0,1]  */
{
    float rel = mu_clampf(goal_relevance, 0.0f, 1.0f) * mu_clampf(intensity, 0.0f, 1.0f);
    float des = mu_clampf(desirability, -1.0f, 1.0f);
    float lik = mu_clampf(likelihood, 0.0f, 1.0f);
    if (rel <= 0.0f) return;

    /* occurrence emotions */
    if (des > 0.0f) m->emotion[MU_EMOTION_JOY] += rel * des;
    else            m->emotion[MU_EMOTION_DISTRESS] += rel * (-des);

    /* prospective emotions (desirable + uncertain) */
    if (des > 0.0f && lik < 1.0f) m->emotion[MU_EMOTION_HOPE] += rel * des * (1.0f - lik) * 2.0f;
    if (des < 0.0f && lik < 1.0f) m->emotion[MU_EMOTION_FEAR] += rel * (-des) * (1.0f - lik) * 2.0f;

    /* confirm/deny: satisfaction when a hoped-for event lands,
     * disappointment when it fails (only if it was desirable) */
    if (des > 0.0f && lik > 0.95f) m->emotion[MU_EMOTION_SATISFACTION] += rel * des;
    if (des > 0.0f && lik < 0.05f) m->emotion[MU_EMOTION_DISAPPOINTMENT] += rel * des;

    /* self-agency raises dominance */
    m->dominance = mu_clampf(m->dominance + 0.1f * rel * (mu_clampf(agency_self, 0.0f, 1.0f) * 2.0f - 1.0f),
                             -1.0f, 1.0f);

    /* intensity is bounded: every slot clamps at 1 */
    {
        int i;
        for (i = 0; i < MU_EMOTION_COUNT; ++i)
            m->emotion[i] = mu_clampf(m->emotion[i], 0.0f, 1.0f);
    }
}

/* Frame step: decay emotions, low-pass mood from the emotion vector. */
MU_INLINE void mu_mood_update(mu_mood* m, float dt)
{
    int i;
    float decay, v, a;

    if (dt <= 0.0f) return;
    decay = m->emotion_decay * dt;
    if (decay > 1.0f) decay = 1.0f;
    for (i = 0; i < MU_EMOTION_COUNT; ++i)
    {
        m->emotion[i] -= m->emotion[i] * decay;
        if (m->emotion[i] < 0.0f) m->emotion[i] = 0.0f;
    }

    /* emotion -> PAD mapping (documented simplification):
     * valence = joy+satisfaction - distress-disappointment
     * arousal = fear+distress+hope - (baseline)
     */
    v = (m->emotion[MU_EMOTION_JOY] + m->emotion[MU_EMOTION_SATISFACTION])
      - (m->emotion[MU_EMOTION_DISTRESS] + m->emotion[MU_EMOTION_DISAPPOINTMENT]);
    a = m->emotion[MU_EMOTION_FEAR] + m->emotion[MU_EMOTION_DISTRESS]
      + 0.5f * m->emotion[MU_EMOTION_HOPE];

    v = mu_clampf(v, -1.0f, 1.0f);
    a = mu_clampf(a, 0.0f, 1.0f);

    m->valence += m->mood_alpha * (v - m->valence);
    m->arousal += m->mood_alpha * (a - m->arousal);
}

/* Mood bias helper for mu_utility: scale a consideration input by mood. */
MU_INLINE float mu_mood_bias(float x, float valence, float strength)
{
    float b = x * (1.0f + strength * valence);
    return mu_clampf(b, 0.0f, 1.0f);
}

MU_END_EXTERN_C
