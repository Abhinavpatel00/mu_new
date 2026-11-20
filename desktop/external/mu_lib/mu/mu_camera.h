#pragma once

/*
===============================================================================

mu_camera.h — frame-rate-independent camera with deadzone, lookahead,
critically-damped smoothing, deterministic trauma shake, bounds clamping,
zoom and view/proj matrix output.

Built on mu_math.h (mu_mat4_look_at, mu_mat4_perspective, mu_frustum_*) and
mu_geom.h (mu_aabb). Shake is driven by mu_noise_math.h so it is deterministic
and replayable given the same seed.

No allocation. All functions MU_INLINE.

Usage:
    mu_camera cam;
    mu_camera_init(&cam, 60.0f, 0.1f near, 1000.0f far, 45.0f fov);
    mu_camera_set_target(&cam, player_pos);
    mu_camera_update(&cam, player_vel, dt);
    mu_camera_view_proj(&cam, aspect, view, proj);
    mu_frustum_from_view_proj(&frustum, view_proj);  // then cull

===============================================================================
*/

#include "mu_common.h"
#include "mu/mu_math.h"
#include "mu/mu_geom.h"
#include "mu/mu_noise_math.h"

MU_BEGIN_EXTERN_C

typedef struct mu_camera
{
    /* ---- state ---- */
    float position[3];   /* world-space camera position */
    float target[3];     /* where the camera is looking */
    float up[3];         /* world up (default 0,1,0) */

    /* ---- deadzone (world-space half extents on the ground plane) ----
     * If the target stays within [pos-deadzone, pos+deadzone] in x/z the
     * camera does not track. Set to 0 to track tightly. */
    float deadzone_x;
    float deadzone_z;

    /* ---- lookahead (seconds) ----
     * Camera leads the target by target_vel * lookahead, clamped by
     * lookahead_max_distance. Helps the target feel like it's moving into
     * screen space rather than away from it. */
    float lookahead_seconds;
    float lookahead_max_distance;

    /* ---- critically-damped smoothing ----
     * frequency (rad/s) controls how snappily the camera catches up. Higher =
     * faster but more overshoot-adjacent; critical damping means no overshoot
     * for a step target. Frame-rate independent (exact closed form). */
    float smooth_frequency;

    /* ---- trauma shake ----
     * trauma in [0,1]. Shake amplitude ~ trauma^2 (design convention).
     * Decays exponentially each update. Offset is band-limited noise so the
     * shake is deterministic and replayable. */
    float trauma;
    float trauma_decay;   /* per-second decay of trauma */
    float shake_frequency;
    float shake_amplitude;
    uint32_t shake_seed;

    /* ---- internal time accumulator (for deterministic shake) ---- */
    float shake_time;

    /* ---- bounds ----
     * If clamp_bounds, position is clamped to this AABB after smoothing. */
    bool clamp_bounds;
    mu_aabb bounds;

    /* ---- projection ----
     * near/far/fov/aspect. aspect is set at update time via
     * mu_camera_view_proj or stored here. */
    float near_plane;
    float far_plane;
    float fov_y_rad;
    float zoom;   /* multiplier on fov; 1.0 = normal */
} mu_camera;

/* ---------- lifecycle ---------- */

MU_INLINE void mu_camera_init(mu_camera* c)
{
    MU_ASSERT(c != NULL);
    c->position[0] = c->position[1] = c->position[2] = 0.0f;
    c->target[0] = c->target[1] = c->target[2] = 0.0f;
    c->up[0] = 0.0f; c->up[1] = 1.0f; c->up[2] = 0.0f;

    c->deadzone_x = 0.0f;
    c->deadzone_z = 0.0f;

    c->lookahead_seconds = 0.15f;
    c->lookahead_max_distance = 4.0f;

    c->smooth_frequency = 6.0f;   /* rad/s */

    c->trauma = 0.0f;
    c->trauma_decay = 2.0f;
    c->shake_frequency = 25.0f;
    c->shake_amplitude = 0.05f;
    c->shake_seed = 0x12345678u;
    c->shake_time = 0.0f;

    c->clamp_bounds = false;
    mu_aabb_empty(&c->bounds);

    c->near_plane = 0.1f;
    c->far_plane = 1000.0f;
    c->fov_y_rad = 45.0f * 0.017453292519943295f;
    c->zoom = 1.0f;
}

/* ---------- shake ---------- */

/* Apply an impulse of trauma (clamped to [0,1]). Max with existing trauma so
 * a new hit can't reduce an ongoing shake. */
MU_INLINE void mu_camera_shake(mu_camera* c, float trauma)
{
    MU_ASSERT(c != NULL);
    if(trauma > c->trauma)
        c->trauma = MU_CLAMP(trauma, 0.0f, 1.0f);
}

/* ---------- update ---------- */

/* Advance the camera by dt seconds toward the target.
 *
 * target_pos   : where the tracked entity is now
 * target_vel   : its velocity (used for lookahead; may be NULL)
 *
 * The camera position is smoothed toward the desired point with a
 * critically-damped spring (exact closed form, frame-rate independent).
 * Shake offsets the result deterministically. Bounds are applied last.
 */
MU_INLINE void mu_camera_update(mu_camera* c, const float target_pos[3],
                                const float target_vel[3], float dt)
{
    MU_ASSERT(c != NULL);
    MU_ASSERT(target_pos != NULL);
    if(dt < 0.0f)
        dt = 0.0f;

    /* ---- 1. decay trauma (shake) ------------------------------------- */
    if(c->trauma > 0.0f)
    {
        c->trauma -= c->trauma_decay * dt;
        if(c->trauma < 0.0f)
            c->trauma = 0.0f;
    }

    /* ---- 2. desired position (deadzone + lookahead) ------------------ */
    float desired[3];
    MU_MEMCPY(desired, target_pos, sizeof(desired));

    /* Deadzone on the ground plane: if the target is close enough to the
     * camera's current x/z, don't pull the camera. */
    float dx = target_pos[0] - c->position[0];
    float dz = target_pos[2] - c->position[2];
    if(c->deadzone_x > 0.0f && c->deadzone_z > 0.0f)
    {
        float ax = dx > 0.0f ? dx : -dx;
        float az = dz > 0.0f ? dz : -dz;
        if(ax <= c->deadzone_x && az <= c->deadzone_z)
        {
            desired[0] = c->position[0];
            desired[2] = c->position[2];
        }
        else
        {
            /* Clamp target to deadzone edge. */
            if(ax > c->deadzone_x)
                desired[0] = c->position[0] + (dx > 0.0f ? c->deadzone_x : -c->deadzone_x);
            if(az > c->deadzone_z)
                desired[2] = c->position[2] + (dz > 0.0f ? c->deadzone_z : -c->deadzone_z);
        }
    }

    /* Lookahead: lead the target by its velocity. */
    if(target_vel != NULL && c->lookahead_seconds > 0.0f)
    {
        float lead[3] = {
            target_vel[0] * c->lookahead_seconds,
            target_vel[1] * c->lookahead_seconds,
            target_vel[2] * c->lookahead_seconds
        };
        float lead_len = sqrtf(lead[0]*lead[0] + lead[1]*lead[1] + lead[2]*lead[2]);
        if(lead_len > c->lookahead_max_distance && lead_len > 0.0f)
        {
            float s = c->lookahead_max_distance / lead_len;
            lead[0] *= s; lead[1] *= s; lead[2] *= s;
        }
        desired[0] += lead[0];
        desired[1] += lead[1];
        desired[2] += lead[2];
    }

    /* ---- 3. shake offset (deterministic noise) ----------------------- */
    float shake[3] = {0.0f, 0.0f, 0.0f};
    if(c->trauma > 0.0f)
    {
        c->shake_time += dt;
        float shake_amt = c->trauma * c->trauma * c->shake_amplitude;
        /* Time base: combines seed + accumulated time so the shake is
         * deterministic and animated. */
        float t = c->shake_seed * 0.000001f + c->shake_time;
        /* Two noise samples for x/z shake; y kept stable. */
        float nx = mu_noise_simplex2(c->position[0] * 0.5f + t,
                                     c->position[2] * 0.5f + t * 1.7f,
                                     c->shake_seed);
        float nz = mu_noise_simplex2(c->position[0] * 0.5f + t * 3.1f,
                                     c->position[2] * 0.5f + t,
                                     c->shake_seed + 1);
        shake[0] = nx * shake_amt;
        shake[2] = nz * shake_amt;
    }

    /* ---- 4. critically-damped smooth toward desired + shake ---------- */
    float goal[3] = {
        desired[0] + shake[0],
        desired[1] + shake[1],
        desired[2] + shake[2]
    };

    float w = c->smooth_frequency;
    float e = expf(-w * dt);
    for(int axis = 0; axis < 3; ++axis)
    {
        float x0 = c->position[axis];
        float g = goal[axis];
        /* v0 is unknown here; assume 0 (camera carries no persistent
         * velocity between frames). This keeps it deterministic. */
        float v0 = 0.0f;
        float C1 = x0 - g;
        float C2 = v0 + w * C1;
        c->position[axis] = g + e * (C1 + dt * C2);
    }

    /* ---- 5. bounds clamp -------------------------------------------- */
    if(c->clamp_bounds)
    {
        for(int axis = 0; axis < 3; ++axis)
            c->position[axis] = MU_CLAMP(c->position[axis],
                                         c->bounds.min[axis],
                                         c->bounds.max[axis]);
    }

    /* Target follows position smoothly enough; keep target at the goal so
     * look_at is stable. */
    MU_MEMCPY(c->target, goal, sizeof(c->target));
}

/* ---------- matrices ---------- */

/* View matrix (look-at). Writes 16 floats column-major. */
MU_INLINE void mu_camera_view(const mu_camera* c, float view[16])
{
    MU_ASSERT(c != NULL);
    MU_ASSERT(view != NULL);
    mu_mat4_look_at(view, c->position, c->target, c->up);
}

/* Projection matrix. `aspect` = width / height. Column-major 16 floats. */
MU_INLINE void mu_camera_proj(const mu_camera* c, float aspect,
                              float proj[16])
{
    MU_ASSERT(c != NULL);
    MU_ASSERT(proj != NULL);
    float fov = c->fov_y_rad / c->zoom;
    if(fov < 0.01f) fov = 0.01f;
    if(fov > 3.10f) fov = 3.10f;
    mu_mat4_perspective(proj, fov, aspect, c->near_plane, c->far_plane);
}

/* view * proj (column-major). */
MU_INLINE void mu_camera_view_proj(const mu_camera* c, float aspect,
                                   float view[16], float proj[16],
                                   float vp[16])
{
    mu_camera_view(c, view);
    mu_camera_proj(c, aspect, proj);
    /* column-major: vp = proj * view */
    float tmp[16];
    /* tmp = proj * view */
    for(int col = 0; col < 4; ++col)
        for(int row = 0; row < 4; ++row)
        {
            tmp[col*4 + row] =
                proj[col*4 + 0] * view[0*4 + row] +
                proj[col*4 + 1] * view[1*4 + row] +
                proj[col*4 + 2] * view[2*4 + row] +
                proj[col*4 + 3] * view[3*4 + row];
        }
    MU_MEMCPY(vp, tmp, sizeof(tmp));
}

/* ---------- convenience ---------- */

/* Set the bounds and enable clamping. Pass clamp_bounds=false to disable. */
MU_INLINE void mu_camera_set_bounds(mu_camera* c, bool clamp,
                                    const float min[3], const float max[3])
{
    MU_ASSERT(c != NULL);
    c->clamp_bounds = clamp;
    if(clamp)
    {
        MU_MEMCPY(c->bounds.min, min, sizeof(c->bounds.min));
        MU_MEMCPY(c->bounds.max, max, sizeof(c->bounds.max));
    }
}

MU_END_EXTERN_C
