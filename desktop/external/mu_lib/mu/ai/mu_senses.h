#pragma once

/*
===============================================================================

mu_senses.h — perception and the graded awareness ladder.

The Thief / Dark Engine / Alien: Isolation contract, distilled: perception is
a multi-signal computation (light, distance, motion, cone, sound), and the
result is not a boolean but a *ladder* that rises fast and decays slow.

- mu_sense_vision: cone x range falloff x light x motion -> strength [0,1].
- mu_sense_hearing: loudness over distance with occlusion.
- Awareness ladder: Idle -> Suspicious -> Searching -> Alert, with exponential
  decay back down and a remembered last-known position (the Investigator
  pattern: search where the prey *was*, not where it is).
- Legibility hook: `why` records which senses contributed (bitmask) so a UI
  can show the player a light gem. If the player can't read detection, the
  system is unplayable — that is the whole lesson.

No cheating: everything is computed from caller-supplied signals.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

#ifndef MU_SENSE_PI
#define MU_SENSE_PI 3.14159265358979323846f
#endif

/* ---------- senses ---------- */

/* small local clamp so this header stands alone */
MU_INLINE float mu_clampf_local(float x, float lo, float hi)
{
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* Vision strength of `target` from `origin` looking along `forward`.
 * fov_half_rad: half-angle of the cone. range: hard cutoff.
 * light01: 0 = pitch dark (nearly invisible), 1 = fully lit.
 * motion01: 0 = still, 1 = sprinting. min_light_floor: how much vision
 * survives darkness (guards with torches: 0.2; blind dark: 0). */
MU_INLINE float mu_sense_vision(const float origin[2], const float forward[2],
                                float fov_half_rad, float range,
                                const float target[2],
                                float light01, float motion01,
                                float min_light_floor)
{
    float dx = target[0] - origin[0];
    float dy = target[1] - origin[1];
    float dist2 = dx * dx + dy * dy;
    float dist, fx, fy, flen, cosang, coslim, cone, falloff, vis;
    if (dist2 > range * range) return 0.0f;
    dist = sqrtf(dist2);
    if (dist < 1e-6f) return 1.0f;
    flen = sqrtf(forward[0] * forward[0] + forward[1] * forward[1]);
    if (flen < 1e-6f) return 0.0f;
    fx = forward[0] / flen;
    fy = forward[1] / flen;
    cosang = (dx * fx + dy * fy) / dist;
    coslim = cosf(fov_half_rad);
    if (cosang < coslim) return 0.0f;                    /* outside the cone */
    /* smooth cone factor across the outer 25% of the cone */
    {
        float band = coslim + (1.0f - coslim) * 0.25f;
        cone = (cosang >= band) ? 1.0f : (cosang - coslim) / (band - coslim);
    }
    falloff = 1.0f - dist / range;
    if (falloff < 0.0f) falloff = 0.0f;
    vis = min_light_floor + (1.0f - min_light_floor) * light01;
    /* motion amplifies visibility up to 2x, clamped at 1 */
    vis *= 1.0f + mu_clampf_local(motion01, 0.0f, 1.0f);
    if (vis > 1.0f) vis = 1.0f;
    return cone * falloff * vis;
}

/* Hearing strength: linear falloff over `range`, occlusion in [0,1] where
 * 1 = fully blocked (multiplies by (1-occlusion)). */
MU_INLINE float mu_sense_hearing(float loudness, float dist,
                                 float occlusion01, float range)
{
    float s;
    if (range <= 0.0f) return 0.0f;
    if (dist >= range) return 0.0f;
    s = loudness * (1.0f - dist / range);
    s *= 1.0f - mu_clampf_local(occlusion01, 0.0f, 1.0f);
    return mu_clampf_local(s, 0.0f, 1.0f);
}

/* ---------- awareness ladder ---------- */

typedef enum mu_aware_level
{
    MU_AWARE_IDLE = 0,
    MU_AWARE_SUSPICIOUS,
    MU_AWARE_SEARCHING,
    MU_AWARE_ALERT
} mu_aware_level;

#define MU_SENSE_CAUSE_VISION  0x1u
#define MU_SENSE_CAUSE_HEARING 0x2u
#define MU_SENSE_CAUSE_CORPSE  0x4u
#define MU_SENSE_CAUSE_OTHER   0x8u

typedef struct mu_awareness
{
    mu_aware_level level;
    float strength;          /* [0,1] stimulus accumulator               */
    float decay_rate;        /* per-second exponential decay             */
    float rise;              /* how fast a stimulus pushes strength up   */
    float last_known[2];
    int   has_last_known;
    uint32_t why;            /* cause bitmask of the latest stimulus     */
} mu_awareness;

MU_INLINE void mu_awareness_init(mu_awareness* a)
{
    a->level = MU_AWARE_IDLE;
    a->strength = 0.0f;
    a->decay_rate = 0.25f;
    a->rise = 1.0f;
    a->last_known[0] = 0.0f;
    a->last_known[1] = 0.0f;
    a->has_last_known = 0;
    a->why = 0u;
}

/* Feed a stimulus (strength in [0,1]); updates ladder + last-known position.
 * Returns the new level. */
MU_INLINE mu_aware_level mu_awareness_stimulate(mu_awareness* a,
                                                float strength,
                                                uint32_t cause,
                                                const float pos[2])
{
    if (strength <= 0.0f) return a->level;
    a->strength += a->rise * strength;
    if (a->strength > 1.0f) a->strength = 1.0f;
    a->why = cause;
    if (pos)
    {
        a->last_known[0] = pos[0];
        a->last_known[1] = pos[1];
        a->has_last_known = 1;
    }
    /* thresholds: suspicious at 0.25, searching at 0.55, alert at 0.85 */
    if (a->strength >= 0.85f)      a->level = MU_AWARE_ALERT;
    else if (a->strength >= 0.55f) a->level = MU_AWARE_SEARCHING;
    else if (a->strength >= 0.25f) a->level = MU_AWARE_SUSPICIOUS;
    return a->level;
}

/* Frame step: exponential decay; level falls as strength crosses thresholds.
 * `hysteresis` (e.g. 0.05) makes the level stick a little before dropping. */
MU_INLINE void mu_awareness_update(mu_awareness* a, float dt)
{
    if (dt <= 0.0f) return;
    a->strength *= expf(-a->decay_rate * dt);
    if (a->strength < 1e-4f)
    {
        a->strength = 0.0f;
        a->level = MU_AWARE_IDLE;
        a->why = 0u;
        return;
    }
    switch (a->level)
    {
    case MU_AWARE_ALERT:
        if (a->strength < 0.80f) a->level = MU_AWARE_SEARCHING;
        break;
    case MU_AWARE_SEARCHING:
        if (a->strength < 0.50f) a->level = MU_AWARE_SUSPICIOUS;
        break;
    case MU_AWARE_SUSPICIOUS:
        if (a->strength < 0.20f) a->level = MU_AWARE_IDLE;
        break;
    default:
        break;
    }
}

/* Voluntary forget (e.g. search concluded). */
MU_INLINE void mu_awareness_forget(mu_awareness* a)
{
    a->strength = 0.0f;
    a->level = MU_AWARE_IDLE;
    a->has_last_known = 0;
    a->why = 0u;
}

MU_END_EXTERN_C
