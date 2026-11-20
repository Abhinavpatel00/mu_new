#pragma once

/*
===============================================================================

mu_steering.h — movement AI: steering behaviors, boids, formation slots.

Reynolds' steering forces (GDC 1999) in the production shape: one agent struct
(packed float[2] positions — identical layout to mu_vec2), force accumulators
clamped by max_accel, and an integrator. Local avoidance through separation;
formations through slot offsets in the leader frame (Squad API, minimal).

- seek / flee / arrive (slowing radius) / pursue / evade (predictive)
- wander (jittered circle; explicit rng state, xorshift32)
- separation / cohesion / alignment (boids trio, neighbor callback-free:
  you pass the neighbor array — pair it with mu_spatial_hash for queries)
- path follow (nearest-segment projection + arrival at end)
- formation slot (offset in leader frame; offset [x,y] with y>0 = goal-side,
  the companion "soccer" position)
- integrate: accel clamp -> velocity clamp -> position, heading follows vel

2D, floats, no allocation, no globals. Deterministic given a rng state.

===============================================================================
*/

#include "mu_common.h"

#include <math.h>

MU_BEGIN_EXTERN_C

typedef struct mu_steering_agent
{
    float pos[2];
    float vel[2];
    float max_speed;   /* units/s  */
    float max_accel;   /* units/s^2 */
} mu_steering_agent;

MU_INLINE uint32_t mu_steering_rand32(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x9E3779B9u;
    return *s;
}

MU_INLINE float mu_steering_randf(uint32_t* s)
{
    return (float)(mu_steering_rand32(s) >> 8) * (1.0f / 16777216.0f);
}

MU_INLINE void mu_steering_force_set(float out[2], float x, float y)
{
    out[0] = x;
    out[1] = y;
}

/* ---------- steering behaviors ---------- */

MU_INLINE void mu_steering_seek(const mu_steering_agent* a,
                                const float target[2], float out[2])
{
    float d[2] = { target[0] - a->pos[0], target[1] - a->pos[1] };
    float len2 = d[0] * d[0] + d[1] * d[1];
    if (len2 > 1e-12f)
    {
        float inv = a->max_speed / sqrtf(len2);
        out[0] = d[0] * inv - a->vel[0];
        out[1] = d[1] * inv - a->vel[1];
    }
    else
    {
        out[0] = -a->vel[0];
        out[1] = -a->vel[1];
    }
}

MU_INLINE void mu_steering_flee(const mu_steering_agent* a,
                                const float threat[2], float out[2])
{
    float d[2] = { a->pos[0] - threat[0], a->pos[1] - threat[1] };
    float len2 = d[0] * d[0] + d[1] * d[1];
    if (len2 > 1e-12f)
    {
        float inv = a->max_speed / sqrtf(len2);
        out[0] = d[0] * inv - a->vel[0];
        out[1] = d[1] * inv - a->vel[1];
    }
    else
    {
        out[0] = a->max_speed - a->vel[0];
        out[1] = 0.0f;
    }
}

/* Arrive: full speed outside slow_radius, linear slowdown inside, stop at 0.2. */
MU_INLINE void mu_steering_arrive(const mu_steering_agent* a,
                                  const float target[2], float slow_radius,
                                  float out[2])
{
    float d[2] = { target[0] - a->pos[0], target[1] - a->pos[1] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1]);
    float speed;
    if (dist < 0.2f)
    {
        out[0] = -a->vel[0];
        out[1] = -a->vel[1];
        return;
    }
    speed = (slow_radius > 0.0f && dist < slow_radius)
          ? a->max_speed * (dist / slow_radius)
          : a->max_speed;
    {
        float inv = speed / dist;
        out[0] = d[0] * inv - a->vel[0];
        out[1] = d[1] * inv - a->vel[1];
    }
}

MU_INLINE void mu_steering_pursue(const mu_steering_agent* a,
                                  const float target_pos[2],
                                  const float target_vel[2],
                                  float out[2])
{
    float d[2] = { target_pos[0] - a->pos[0], target_pos[1] - a->pos[1] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1]);
    float speed = sqrtf(a->vel[0] * a->vel[0] + a->vel[1] * a->vel[1]);
    float t = (speed > 1e-3f) ? dist / speed : 0.0f;
    float lead[2] = { target_pos[0] + target_vel[0] * t,
                      target_pos[1] + target_vel[1] * t };
    mu_steering_seek(a, lead, out);
}

MU_INLINE void mu_steering_evade(const mu_steering_agent* a,
                                 const float threat_pos[2],
                                 const float threat_vel[2],
                                 float out[2])
{
    float d[2] = { threat_pos[0] - a->pos[0], threat_pos[1] - a->pos[1] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1]);
    float speed = sqrtf(a->vel[0] * a->vel[0] + a->vel[1] * a->vel[1]);
    float t = (speed > 1e-3f) ? dist / speed : 0.0f;
    float lead[2] = { threat_pos[0] + threat_vel[0] * t,
                      threat_pos[1] + threat_vel[1] * t };
    mu_steering_flee(a, lead, out);
}

/* Wander: steer toward a point jittered on a circle ahead of the agent. */
MU_INLINE void mu_steering_wander(const mu_steering_agent* a, uint32_t* rng,
                                  float circle_radius, float jitter, float out[2])
{
    float speed = sqrtf(a->vel[0] * a->vel[0] + a->vel[1] * a->vel[1]);
    float fwd[2];
    float ang = (mu_steering_randf(rng) * 2.0f - 1.0f) * jitter;
    float ca = cosf(ang), sa = sinf(ang);
    float center[2];
    if (speed > 1e-4f)
    {
        fwd[0] = a->vel[0] / speed;
        fwd[1] = a->vel[1] / speed;
    }
    else
    {
        fwd[0] = 1.0f;
        fwd[1] = 0.0f;
    }
    center[0] = a->pos[0] + fwd[0] * circle_radius * 2.0f;
    center[1] = a->pos[1] + fwd[1] * circle_radius * 2.0f;
    {
        float px = fwd[0] * ca - fwd[1] * sa;
        float py = fwd[0] * sa + fwd[1] * ca;
        float target[2] = { center[0] + px * circle_radius,
                            center[1] + py * circle_radius };
        mu_steering_seek(a, target, out);
    }
}

/* ---------- boids ---------- */
/* neighbors: array of positions [x0,y0,x1,y1,...]; neighbor_vel optional. */

MU_INLINE void mu_steering_separation(const mu_steering_agent* a,
                                      const float* neighbors, int n,
                                      float radius, float out[2])
{
    float fx = 0.0f, fy = 0.0f;
    int i, count = 0;
    float r2 = radius * radius;
    for (i = 0; i < n; ++i)
    {
        float dx = a->pos[0] - neighbors[i * 2 + 0];
        float dy = a->pos[1] - neighbors[i * 2 + 1];
        float d2 = dx * dx + dy * dy;
        if (d2 > 1e-12f && d2 < r2)
        {
            float d = sqrtf(d2);
            float w = 1.0f - d / radius;
            fx += (dx / d) * w;
            fy += (dy / d) * w;
            ++count;
        }
    }
    if (count > 0)
    {
        out[0] = fx * a->max_speed - a->vel[0];
        out[1] = fy * a->max_speed - a->vel[1];
    }
    else
    {
        out[0] = 0.0f;
        out[1] = 0.0f;
    }
}

MU_INLINE void mu_steering_cohesion(const mu_steering_agent* a,
                                    const float* neighbors, int n,
                                    float out[2])
{
    float cx = 0.0f, cy = 0.0f;
    int i, count = 0;
    for (i = 0; i < n; ++i)
    {
        cx += neighbors[i * 2 + 0];
        cy += neighbors[i * 2 + 1];
        ++count;
    }
    if (count > 0)
    {
        float center[2] = { cx / (float)count, cy / (float)count };
        mu_steering_seek(a, center, out);
    }
    else
    {
        out[0] = 0.0f;
        out[1] = 0.0f;
    }
}

MU_INLINE void mu_steering_alignment(const mu_steering_agent* a,
                                     const float* neighbor_vel, int n,
                                     float out[2])
{
    float vx = 0.0f, vy = 0.0f;
    int i, count = 0;
    for (i = 0; i < n; ++i)
    {
        vx += neighbor_vel[i * 2 + 0];
        vy += neighbor_vel[i * 2 + 1];
        ++count;
    }
    if (count > 0)
    {
        float len2 = vx * vx + vy * vy;
        if (len2 > 1e-12f)
        {
            float inv = a->max_speed / sqrtf(len2);
            out[0] = vx * inv - a->vel[0];
            out[1] = vy * inv - a->vel[1];
        }
        else { out[0] = 0.0f; out[1] = 0.0f; }
    }
    else { out[0] = 0.0f; out[1] = 0.0f; }
}

/* ---------- path & formations ---------- */

/* Follow polyline with a look-ahead target; arrive at the end.
 * The seek target is the nearest-point projection advanced `lookahead` along
 * the path direction (seeking the projection itself would stall on it).
 * `out_end` (nullable) receives 1 when the agent is within arrival of the end. */
MU_INLINE void mu_steering_path_follow(const mu_steering_agent* a,
                                       const float* pts, int n,
                                       float path_radius, float out[2],
                                       int* out_end)
{
    float best_d2 = 1e30f;
    float best_p[2] = { 0.0f, 0.0f };
    float best_dir[2] = { 1.0f, 0.0f };
    float lookahead = path_radius * 2.0f;
    int i;
    if (out_end) *out_end = 0;
    if (n <= 0) { out[0] = 0.0f; out[1] = 0.0f; return; }
    if (n == 1)
    {
        mu_steering_arrive(a, pts, path_radius, out);
        if (out_end) *out_end = 1;
        return;
    }
    for (i = 0; i + 1 < n; ++i)
    {
        float ax = pts[i * 2 + 0], ay = pts[i * 2 + 1];
        float bx = pts[(i + 1) * 2 + 0], by = pts[(i + 1) * 2 + 1];
        float dx = bx - ax, dy = by - ay;
        float len2 = dx * dx + dy * dy;
        float t = 0.0f, px, py, ddx, ddy, d2;
        if (len2 > 1e-12f)
        {
            t = ((a->pos[0] - ax) * dx + (a->pos[1] - ay) * dy) / len2;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
        }
        px = ax + dx * t;
        py = ay + dy * t;
        ddx = px - a->pos[0];
        ddy = py - a->pos[1];
        d2 = ddx * ddx + ddy * ddy;
        if (d2 < best_d2)
        {
            float len = sqrtf(len2);
            best_d2 = d2;
            best_p[0] = px;
            best_p[1] = py;
            if (len > 1e-6f) { best_dir[0] = dx / len; best_dir[1] = dy / len; }
            else { best_dir[0] = 1.0f; best_dir[1] = 0.0f; }
        }
    }
    {
        float end[2] = { pts[(n - 1) * 2 + 0], pts[(n - 1) * 2 + 1] };
        float edx = end[0] - a->pos[0], edy = end[1] - a->pos[1];
        if (edx * edx + edy * edy < path_radius * path_radius)
        {
            mu_steering_arrive(a, end, path_radius, out);
            if (out_end) *out_end = 1;
        }
        else
        {
            float target[2] = { best_p[0] + best_dir[0] * lookahead,
                                best_p[1] + best_dir[1] * lookahead };
            /* do not overshoot past the end */
            {
                float tx = end[0] - best_p[0], ty = end[1] - best_p[1];
                float t2 = tx * tx + ty * ty;
                if (t2 < lookahead * lookahead)
                {
                    target[0] = end[0];
                    target[1] = end[1];
                }
            }
            mu_steering_seek(a, target, out);
        }
    }
}

/* Formation slot in the leader's frame: target = leader + rot(heading) * off.
 * Common slots: offset {0, +d} = goal-side ahead, {0, -d} = trailing,
 * {±d, 0} = flanks. */
MU_INLINE void mu_steering_formation_slot(const mu_steering_agent* a,
                                          const float leader_pos[2],
                                          const float leader_heading[2],
                                          const float offset[2],
                                          float out[2])
{
    float hx = leader_heading[0], hy = leader_heading[1];
    float len2 = hx * hx + hy * hy;
    float target[2];
    if (len2 < 1e-12f) { hx = 1.0f; hy = 0.0f; }
    else { float inv = 1.0f / sqrtf(len2); hx *= inv; hy *= inv; }
    /* heading = +y axis of the frame; right = perpendicular */
    target[0] = leader_pos[0] + (-hy) * offset[0] + hx * offset[1];
    target[1] = leader_pos[1] + ( hx) * offset[0] + hy * offset[1];
    mu_steering_arrive(a, target, 1.0f, out);
}

/* ---------- integration ---------- */

MU_INLINE void mu_steering_integrate(mu_steering_agent* a,
                                     const float force[2], float dt)
{
    float fx = force[0], fy = force[1];
    float flen = sqrtf(fx * fx + fy * fy);
    if (flen > a->max_accel && flen > 1e-12f)
    {
        float s = a->max_accel / flen;
        fx *= s;
        fy *= s;
    }
    a->vel[0] += fx * dt;
    a->vel[1] += fy * dt;
    {
        float vlen2 = a->vel[0] * a->vel[0] + a->vel[1] * a->vel[1];
        float vmax = a->max_speed;
        if (vlen2 > vmax * vmax && vlen2 > 1e-12f)
        {
            float s = vmax / sqrtf(vlen2);
            a->vel[0] *= s;
            a->vel[1] *= s;
        }
    }
    a->pos[0] += a->vel[0] * dt;
    a->pos[1] += a->vel[1] * dt;
}

MU_END_EXTERN_C
