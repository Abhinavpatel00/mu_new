#pragma once

/*
===============================================================================

mu_geom.h — geometry primitives and queries for 2D/3D engine code.

Types are raw float arrays (same layout as mu_math.h: mu_vec3 = float[3]),
so values copy straight into vertex/uniform buffers.

Queries are branch-light and NaN-safe where it matters:

  * mu_ray_aabb — slab test. Handles zero / axis-parallel directions (a ray
    parallel to a slab never enters it) without producing NaN, and never
    divides by zero.
  * mu_swept_aabb — conservative advancement for a moving AABB vs a static
    AABB; reports the time of impact (toi in [0,1]) and the normal.
  * mu_ray_sphere / mu_ray_capsule — classic analytic tests.

No allocation. All functions MU_INLINE.

===============================================================================
*/

#include <float.h>
#include <math.h>

#include "mu_common.h"

MU_BEGIN_EXTERN_C

/* ===========================================================================
 * Types
 * =========================================================================== */

typedef struct mu_aabb
{
    float min[3];
    float max[3];
} mu_aabb;

typedef struct mu_ray
{
    float origin[3];
    float dir[3];   /* unit length preferred; zero handled gracefully */
} mu_ray;

typedef struct mu_sphere
{
    float center[3];
    float radius;
} mu_sphere;

typedef struct mu_capsule
{
    float p0[3];
    float p1[3];
    float radius;
} mu_capsule;

typedef struct mu_hit
{
    bool  hit;
    float t;        /* distance along ray / time of impact in [0,1] */
    float normal[3];
    float point[3];
} mu_hit;

/* ===========================================================================
 * AABB
 * =========================================================================== */

MU_INLINE void mu_aabb_empty(mu_aabb* b)
{
    b->min[0] = b->min[1] = b->min[2] =  FLT_MAX;
    b->max[0] = b->max[1] = b->max[2] = -FLT_MAX;
}

MU_INLINE void mu_aabb_from_points(mu_aabb* b, const float* pts, uint32_t count)
{
    mu_aabb_empty(b);
    for(uint32_t i = 0; i < count; ++i)
    {
        const float* p = pts + (size_t)i * 3u;
        for(int c = 0; c < 3; ++c)
        {
            if(p[c] < b->min[c]) b->min[c] = p[c];
            if(p[c] > b->max[c]) b->max[c] = p[c];
        }
    }
}

MU_INLINE void mu_aabb_expand(mu_aabb* b, const float* p)
{
    for(int c = 0; c < 3; ++c)
    {
        if(p[c] < b->min[c]) b->min[c] = p[c];
        if(p[c] > b->max[c]) b->max[c] = p[c];
    }
}

MU_INLINE bool mu_aabb_contains_point(const mu_aabb* b, const float* p)
{
    return p[0] >= b->min[0] && p[0] <= b->max[0] &&
           p[1] >= b->min[1] && p[1] <= b->max[1] &&
           p[2] >= b->min[2] && p[2] <= b->max[2];
}

MU_INLINE bool mu_aabb_intersects_aabb(const mu_aabb* a, const mu_aabb* b)
{
    return a->min[0] <= b->max[0] && a->max[0] >= b->min[0] &&
           a->min[1] <= b->max[1] && a->max[1] >= b->min[1] &&
           a->min[2] <= b->max[2] && a->max[2] >= b->min[2];
}

/* Closest point on an AABB to a point (clamps). Writes to out[3]. */
MU_INLINE void mu_aabb_closest_point(const mu_aabb* b, const float* p,
                                     float out[3])
{
    for(int c = 0; c < 3; ++c)
        out[c] = MU_CLAMP(p[c], b->min[c], b->max[c]);
}

/* ===========================================================================
 * Ray — AABB (slab test, NaN-safe for zero / axis-parallel directions)
 * =========================================================================== */

/* Intersect ray with AABB. Returns hit with t and normal of the slab entered.
 * A zero direction component means the ray is parallel to that slab: it hits
 * only if the origin lies inside the slab on that axis, and contributes no
 * t constraint (no division by zero, no NaN). */
MU_INLINE mu_hit mu_ray_aabb(const mu_ray* r, const mu_aabb* b)
{
    mu_hit h;
    h.hit = false;
    h.t = 0.0f;
    h.normal[0] = h.normal[1] = h.normal[2] = 0.0f;
    h.point[0] = h.point[1] = h.point[2] = 0.0f;

    float t_min = -FLT_MAX;
    float t_max =  FLT_MAX;
    int   axis  = -1;
    int   sign  = 0; /* +1 = entering from min side, -1 = from max side */

    for(int c = 0; c < 3; ++c)
    {
        float d = r->dir[c];
        if(d == 0.0f)
        {
            /* Parallel to this slab: must already be inside it. */
            if(r->origin[c] < b->min[c] || r->origin[c] > b->max[c])
                return h; /* no hit */
            continue;
        }
        float inv_d = 1.0f / d;
        float t0 = (b->min[c] - r->origin[c]) * inv_d;
        float t1 = (b->max[c] - r->origin[c]) * inv_d;
        if(t0 > t1)
        {
            float tmp = t0; t0 = t1; t1 = tmp;
        }
        if(t0 > t_min)
        {
            t_min = t0;
            axis = c;
            sign = (d > 0.0f) ? 1 : -1;
        }
        if(t1 < t_max)
            t_max = t1;
        if(t_min > t_max)
            return h; /* missed */
    }

    if(axis < 0)
    {
        /* Ray has zero direction (a point) — hits only if origin inside. */
        if(mu_aabb_contains_point(b, r->origin))
        {
            h.hit = true;
            h.t = 0.0f;
            MU_MEMCPY(h.point, r->origin, sizeof(h.point));
        }
        return h;
    }

    if(t_max < 0.0f)
        return h; /* hit is behind the ray */

    float t = (t_min < 0.0f) ? t_max : t_min;
    if(t < 0.0f)
        return h;

    h.hit = true;
    h.t = t;
    h.point[0] = r->origin[0] + r->dir[0] * t;
    h.point[1] = r->origin[1] + r->dir[1] * t;
    h.point[2] = r->origin[2] + r->dir[2] * t;
    h.normal[0] = h.normal[1] = h.normal[2] = 0.0f;
    h.normal[axis] = (float)sign;
    return h;
}

/* ===========================================================================
 * Ray — Sphere
 * =========================================================================== */

MU_INLINE mu_hit mu_ray_sphere(const mu_ray* r, const mu_sphere* s)
{
    mu_hit h;
    h.hit = false;
    h.t = 0.0f;
    h.normal[0] = h.normal[1] = h.normal[2] = 0.0f;
    h.point[0] = h.point[1] = h.point[2] = 0.0f;

    float oc[3] = { r->origin[0] - s->center[0],
                    r->origin[1] - s->center[1],
                    r->origin[2] - s->center[2] };
    float b = oc[0] * r->dir[0] + oc[1] * r->dir[1] + oc[2] * r->dir[2];
    float c = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] -
              s->radius * s->radius;
    if(c > 0.0f && b > 0.0f)
        return h; /* outside and pointing away */

    float disc = b * b - c;
    if(disc < 0.0f)
        return h;
    float sd = sqrtf(disc);

    float t = -b - sd;
    if(t < 0.0f)
        t = -b + sd;
    if(t < 0.0f)
        return h;

    h.hit = true;
    h.t = t;
    h.point[0] = r->origin[0] + r->dir[0] * t;
    h.point[1] = r->origin[1] + r->dir[1] * t;
    h.point[2] = r->origin[2] + r->dir[2] * t;
    h.normal[0] = (h.point[0] - s->center[0]) / s->radius;
    h.normal[1] = (h.point[1] - s->center[1]) / s->radius;
    h.normal[2] = (h.point[2] - s->center[2]) / s->radius;
    return h;
}

/* ===========================================================================
 * Swept AABB vs static AABB (conservative advancement)
 * =========================================================================== */

/* Move AABB `a` by `velocity` (per-second) for one second of simulated time
 * against static AABB `b`. On hit: *h receives the time of impact toi in
 * [0,1], the normal of the face struck, and the impact point. Returns true if
 * they collide during the interval. Conservative advancement: subdivides the
 * step so no tunneling through thin geometry. */
MU_INLINE bool mu_swept_aabb(const mu_aabb* a, const float velocity[3],
                             const mu_aabb* b, mu_hit* h)
{
    mu_hit res;
    res.hit = false;
    res.t = 0.0f;
    res.normal[0] = res.normal[1] = res.normal[2] = 0.0f;
    res.point[0] = res.point[1] = res.point[2] = 0.0f;

    float t_enter = 0.0f;
    float t_exit = 1.0f;
    int   axis = -1;
    int   sign = 0;

    for(int c = 0; c < 3; ++c)
    {
        if(velocity[c] > 0.0f)
        {
            float t_enter_c = (b->min[c] - a->max[c]) / velocity[c];
            float t_exit_c  = (b->max[c] - a->min[c]) / velocity[c];
            if(t_enter_c > t_enter)
            {
                t_enter = t_enter_c;
                axis = c;
                sign = 1;
            }
            if(t_exit_c < t_exit)
                t_exit = t_exit_c;
        }
        else if(velocity[c] < 0.0f)
        {
            float t_enter_c = (b->max[c] - a->min[c]) / velocity[c];
            float t_exit_c  = (b->min[c] - a->max[c]) / velocity[c];
            if(t_enter_c > t_enter)
            {
                t_enter = t_enter_c;
                axis = c;
                sign = -1;
            }
            if(t_exit_c < t_exit)
                t_exit = t_exit_c;
        }
        else
        {
            /* Stationary on this axis: must already overlap. */
            if(a->max[c] < b->min[c] || a->min[c] > b->max[c])
            {
                MU_MEMCPY(h, &res, sizeof(res));
                return false;
            }
        }
    }

    if(t_enter > t_exit || t_enter < 0.0f)
    {
        MU_MEMCPY(h, &res, sizeof(res));
        return false;
    }

    /* Position at impact (min corner of a at time of impact). */
    float pos[3] = { a->min[0] + velocity[0] * t_enter,
                     a->min[1] + velocity[1] * t_enter,
                     a->min[2] + velocity[2] * t_enter };

    res.hit = true;
    res.t = t_enter;
    res.normal[0] = res.normal[1] = res.normal[2] = 0.0f;
    if(axis >= 0)
        res.normal[axis] = (float)sign;
    for(int c = 0; c < 3; ++c)
        res.point[c] = pos[c];
    if(axis >= 0)
        res.point[axis] = sign > 0 ? b->min[axis] : b->max[axis];

    MU_MEMCPY(h, &res, sizeof(res));
    return true;
}

/* ===========================================================================
 * Closest point: segment vs segment (used for capsule-vs-capsule)
 * =========================================================================== */

/* Closest points on segment (p0,p1) and segment (q0,q1). Writes
 * cp0/cp1 and returns the squared distance between them. */
MU_INLINE float mu_closest_point_segment_segment(const float p0[3],
        const float p1[3], const float q0[3], const float q1[3],
        float cp0[3], float cp1[3])
{
    float d1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
    float d2[3] = { q1[0]-q0[0], q1[1]-q0[1], q1[2]-q0[2] };
    float r[3]  = { p0[0]-q0[0], p0[1]-q0[1], p0[2]-q0[2] };

    float a = d1[0]*d1[0] + d1[1]*d1[1] + d1[2]*d1[2]; /* |d1|^2 */
    float e = d2[0]*d2[0] + d2[1]*d2[1] + d2[2]*d2[2]; /* |d2|^2 */
    float f = d2[0]*r[0] + d2[1]*r[1] + d2[2]*r[2];     /* d2·r */

    float s = 0.0f, t = 0.0f;
    if(a <= 1e-12f && e <= 1e-12f)
    {
        /* Both segments degenerate to points. */
        MU_MEMCPY(cp0, p0, sizeof(float)*3);
        MU_MEMCPY(cp1, q0, sizeof(float)*3);
        float diff[3] = { p0[0]-q0[0], p0[1]-q0[1], p0[2]-q0[2] };
        return diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2];
    }
    if(a <= 1e-12f)
    {
        s = 0.0f;
        t = MU_CLAMP(f / e, 0.0f, 1.0f);
    }
    else
    {
        float c = d1[0]*r[0] + d1[1]*r[1] + d1[2]*r[2]; /* d1·r */
        if(e <= 1e-12f)
        {
            t = 0.0f;
            s = MU_CLAMP(-c / a, 0.0f, 1.0f);
        }
        else
        {
            float b = d1[0]*d2[0] + d1[1]*d2[1] + d1[2]*d2[2]; /* d1·d2 */
            float denom = a*e - b*b;
            if(denom != 0.0f)
                s = MU_CLAMP((b*f - c*e) / denom, 0.0f, 1.0f);
            else
                s = 0.0f;
            t = (b*s + f) / e;
            if(t < 0.0f)
            {
                t = 0.0f;
                s = MU_CLAMP(-c / a, 0.0f, 1.0f);
            }
            else if(t > 1.0f)
            {
                t = 1.0f;
                s = MU_CLAMP((b - c) / a, 0.0f, 1.0f);
            }
        }
    }

    cp0[0] = p0[0] + d1[0] * s;
    cp0[1] = p0[1] + d1[1] * s;
    cp0[2] = p0[2] + d1[2] * s;
    cp1[0] = q0[0] + d2[0] * t;
    cp1[1] = q0[1] + d2[1] * t;
    cp1[2] = q0[2] + d2[2] * t;

    float diff[3] = { cp0[0]-cp1[0], cp0[1]-cp1[1], cp0[2]-cp1[2] };
    return diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2];
}

/* ===========================================================================
 * Ray — Capsule
 * =========================================================================== */

/* Intersect ray with capsule (segment p0->p1, radius). Returns the nearest
 * hit (smallest t >= 0). Method: solve against the infinite cylinder around
 * the capsule segment, keep solutions whose closest point on the segment lies
 * in [0,1], then also test the two endpoint spheres; take the smallest t. */
MU_INLINE mu_hit mu_ray_capsule(const mu_ray* r, const mu_capsule* c)
{
    mu_hit h;
    h.hit = false;
    h.t = FLT_MAX;
    h.normal[0] = h.normal[1] = h.normal[2] = 0.0f;
    h.point[0] = h.point[1] = h.point[2] = 0.0f;

    float seg[3] = { c->p1[0]-c->p0[0], c->p1[1]-c->p0[1], c->p1[2]-c->p0[2] };
    float seg_len2 = seg[0]*seg[0] + seg[1]*seg[1] + seg[2]*seg[2];

    /* Degenerate capsule = sphere. */
    if(seg_len2 <= 1e-12f)
    {
        mu_sphere s;
        MU_MEMCPY(s.center, c->p0, sizeof(s.center));
        s.radius = c->radius;
        return mu_ray_sphere(r, &s);
    }

    float seg_len = sqrtf(seg_len2);
    float seg_inv_len = 1.0f / seg_len;
    float seg_dir[3] = { seg[0]*seg_inv_len, seg[1]*seg_inv_len, seg[2]*seg_inv_len };

    /* Vector from p0 to ray origin. */
    float w[3] = { r->origin[0]-c->p0[0], r->origin[1]-c->p0[1],
                   r->origin[2]-c->p0[2] };

    /* Project ray origin and direction onto the segment axis. */
    float w_parallel = w[0]*seg_dir[0] + w[1]*seg_dir[1] + w[2]*seg_dir[2];
    float d_parallel = r->dir[0]*seg_dir[0] + r->dir[1]*seg_dir[1] + r->dir[2]*seg_dir[2];

    /* Perpendicular components. */
    float w_perp[3] = { w[0] - w_parallel*seg_dir[0],
                        w[1] - w_parallel*seg_dir[1],
                        w[2] - w_parallel*seg_dir[2] };
    float d_perp[3] = { r->dir[0] - d_parallel*seg_dir[0],
                        r->dir[1] - d_parallel*seg_dir[1],
                        r->dir[2] - d_parallel*seg_dir[2] };

    /* Solve |w_perp + s * d_perp|^2 = radius^2  (cylinder hit) */
    float a = d_perp[0]*d_perp[0] + d_perp[1]*d_perp[1] + d_perp[2]*d_perp[2];
    float bb = 2.0f * (w_perp[0]*d_perp[0] + w_perp[1]*d_perp[1] + w_perp[2]*d_perp[2]);
    float cc = w_perp[0]*w_perp[0] + w_perp[1]*w_perp[1] + w_perp[2]*w_perp[2]
             - c->radius * c->radius;

    bool have_hit = false;
    float best_t = FLT_MAX;
    float best_point[3] = {0,0,0};
    float best_normal[3] = {0,0,0};

    /* Helper to consider a candidate t. */
    #define MU_GEOM_CONSIDER(t)                                                   \
        do {                                                                      \
            if((t) >= 0.0f && (t) < best_t)                                       \
            {                                                                     \
                float pt[3] = { r->origin[0] + r->dir[0]*(t),                     \
                                r->origin[1] + r->dir[1]*(t),                     \
                                r->origin[2] + r->dir[2]*(t) };                   \
                /* Closest point on segment to pt. */                             \
                float tp = (pt[0]-c->p0[0])*seg_dir[0] +                           \
                           (pt[1]-c->p0[1])*seg_dir[1] +                           \
                           (pt[2]-c->p0[2])*seg_dir[2];                           \
                if(tp < 0.0f) tp = 0.0f;                                          \
                if(tp > seg_len) tp = seg_len;                                    \
                float cp[3] = { c->p0[0] + seg_dir[0]*tp,                         \
                                c->p0[1] + seg_dir[1]*tp,                         \
                                c->p0[2] + seg_dir[2]*tp };                       \
                float diff[3] = { pt[0]-cp[0], pt[1]-cp[1], pt[2]-cp[2] };        \
                float dist2 = diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2];\
                if(dist2 <= c->radius*c->radius + 1e-6f)                          \
                {                                                                 \
                    best_t = (t);                                                 \
                    MU_MEMCPY(best_point, pt, sizeof(pt));                        \
                    float len = sqrtf(dist2);                                     \
                    if(len > 1e-6f)                                               \
                    {                                                             \
                        best_normal[0] = diff[0]/len;                             \
                        best_normal[1] = diff[1]/len;                             \
                        best_normal[2] = diff[2]/len;                             \
                    }                                                             \
                    else                                                          \
                    {                                                             \
                        best_normal[0] = 0.0f; best_normal[1] = 1.0f;             \
                        best_normal[2] = 0.0f;                                    \
                    }                                                             \
                    have_hit = true;                                              \
                }                                                                 \
            }                                                                     \
        } while(0)

    if(a > 1e-12f)
    {
        float disc = bb*bb - 4.0f*a*cc;
        if(disc >= 0.0f)
        {
            float sd = sqrtf(disc);
            float t1 = (-bb - sd) / (2.0f*a);
            float t2 = (-bb + sd) / (2.0f*a);
            MU_GEOM_CONSIDER(t1);
            MU_GEOM_CONSIDER(t2);
        }
    }
    else
    {
        /* Ray parallel to cylinder axis: hit iff |w_perp| <= radius,
         * any t works; the segment endpoints clamp the valid range. */
        if(cc <= 0.0f)
        {
            /* The whole ray is within the cylinder radius radially;
             * valid t range is where the axial position is in [0, seg_len]. */
            float t_a = -w_parallel / d_parallel;
            float t_b = (seg_len - w_parallel) / d_parallel;
            float t_lo = MU_MIN(t_a, t_b);
            float t_hi = MU_MAX(t_a, t_b);
            float t_start = MU_MAX(t_lo, 0.0f);
            if(t_start <= t_hi)
                MU_GEOM_CONSIDER(t_start);
        }
    }

    /* Endpoint spheres (covers the rounded caps the cylinder test misses). */
    mu_sphere s0, s1;
    MU_MEMCPY(s0.center, c->p0, sizeof(s0.center)); s0.radius = c->radius;
    MU_MEMCPY(s1.center, c->p1, sizeof(s1.center)); s1.radius = c->radius;
    mu_hit h0 = mu_ray_sphere(r, &s0);
    mu_hit h1 = mu_ray_sphere(r, &s1);
    if(h0.hit && h0.t < best_t) { best_t = h0.t; MU_MEMCPY(best_point, h0.point, sizeof(best_point)); MU_MEMCPY(best_normal, h0.normal, sizeof(best_normal)); have_hit = true; }
    if(h1.hit && h1.t < best_t) { best_t = h1.t; MU_MEMCPY(best_point, h1.point, sizeof(best_point)); MU_MEMCPY(best_normal, h1.normal, sizeof(best_normal)); have_hit = true; }

    #undef MU_GEOM_CONSIDER

    if(!have_hit)
        return h; /* hit == false */

    h.hit = true;
    h.t = best_t;
    MU_MEMCPY(h.point, best_point, sizeof(h.point));
    MU_MEMCPY(h.normal, best_normal, sizeof(h.normal));
    return h;
}

MU_END_EXTERN_C
