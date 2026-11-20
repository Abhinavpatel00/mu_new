#pragma once

/*
===============================================================================

mu_reaction_diffusion.h — Gray-Scott reaction-diffusion (Turing patterns).

Texturing & Modeling: A Procedural Approach — reaction-diffusion chapter.

Two chemicals U and V diffuse across a 2D grid and react:

    du/dt = Du * lap(U) - U * V^2 + F * (1 - U)
    dv/dt = Dv * lap(V) + U * V^2 - (F + k) * V

- 5-point stencil Laplacian with wrap-around (toroidal) boundaries.
- Explicit Euler integration; `dt` is the caller's time step.
- `seed()` fills U with ~1 and V with ~0, then injects a V-rich square spot
  at the center so patterns emerge from a small perturbation.
- `step()` advances one `dt`. `run()` advances `n` steps.
- get_u/get_v read a cell.

All flat arrays; no per-cell allocation after init.

===============================================================================
*/

#include "mu_common.h"

#include <stdlib.h>
#include <string.h>

MU_BEGIN_EXTERN_C

typedef struct mu_rd_grid
{
    float* u;                 /* [width * height] concentration of U    */
    float* v;                 /* [width * height] concentration of V    */
    uint32_t width;
    uint32_t height;

    float Du;                 /* diffusion rate of U                    */
    float Dv;                 /* diffusion rate of V                    */
    float F;                  /* feed rate                              */
    float k;                  /* kill rate                              */
    float dt;                 /* time step                              */
} mu_rd_grid;

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_rd_init(mu_rd_grid* g, uint32_t width, uint32_t height)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(width > 0u && height > 0u);

    size_t ncells = (size_t)width * (size_t)height;
    g->u = (float*)MU_CALLOC(ncells, sizeof(float));
    g->v = (float*)MU_CALLOC(ncells, sizeof(float));
    if(!g->u || !g->v)
    {
        MU_FREE(g->u);
        MU_FREE(g->v);
        g->u = NULL;
        g->v = NULL;
        return false;
    }

    g->width = width;
    g->height = height;
    g->Du = 0.16f;
    g->Dv = 0.08f;
    g->F = 0.060f;
    g->k = 0.062f;
    g->dt = 1.0f;
    return true;
}

MU_INLINE void mu_rd_destroy(mu_rd_grid* g)
{
    if(g == NULL)
        return;
    MU_FREE(g->u);
    MU_FREE(g->v);
    g->u = NULL;
    g->v = NULL;
    g->width = g->height = 0u;
}

MU_INLINE uint32_t mu_rd_width(const mu_rd_grid* g)  { return g->width; }
MU_INLINE uint32_t mu_rd_height(const mu_rd_grid* g) { return g->height; }

MU_INLINE void mu_rd_set_params(mu_rd_grid* g, float Du, float Dv,
                                float F, float k, float dt)
{
    MU_ASSERT(g != NULL);
    g->Du = Du;
    g->Dv = Dv;
    g->F = F;
    g->k = k;
    g->dt = dt;
}

MU_INLINE float mu_rd_get_u(const mu_rd_grid* g, uint32_t x, uint32_t y)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(x < g->width && y < g->height);
    return g->u[(size_t)y * (size_t)g->width + (size_t)x];
}

MU_INLINE float mu_rd_get_v(const mu_rd_grid* g, uint32_t x, uint32_t y)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(x < g->width && y < g->height);
    return g->v[(size_t)y * (size_t)g->width + (size_t)x];
}

MU_INLINE void mu_rd_set_cell(mu_rd_grid* g, uint32_t x, uint32_t y,
                              float u, float v)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(x < g->width && y < g->height);
    size_t i = (size_t)y * (size_t)g->width + (size_t)x;
    g->u[i] = u;
    g->v[i] = v;
}

/* ---------- seeding ---------- */

/* Seed: U = 1, V = 0 everywhere, then a V-rich square spot at the center.
 * This is the classic Gray-Scott start that grows spots/stripes/mazes
 * depending on F and k. */
MU_INLINE void mu_rd_seed(mu_rd_grid* g)
{
    MU_ASSERT(g != NULL);
    size_t ncells = (size_t)g->width * (size_t)g->height;
    for(size_t i = 0; i < ncells; ++i)
    {
        g->u[i] = 1.0f;
        g->v[i] = 0.0f;
    }

    uint32_t cx = g->width / 2u;
    uint32_t cy = g->height / 2u;
    uint32_t r = g->width / 10u;
    if(r < 1u)
        r = 1u;
    uint32_t x0 = cx > r ? cx - r : 0u;
    uint32_t y0 = cy > r ? cy - r : 0u;
    uint32_t x1 = cx + r < g->width ? cx + r : g->width - 1u;
    uint32_t y1 = cy + r < g->height ? cy + r : g->height - 1u;

    for(uint32_t y = y0; y <= y1; ++y)
    {
        size_t row = (size_t)y * (size_t)g->width;
        for(uint32_t x = x0; x <= x1; ++x)
        {
            size_t i = row + (size_t)x;
            g->u[i] = 0.5f;
            g->v[i] = 0.25f;
        }
    }
}

/* ---------- stepping ---------- */

/* Toroidal index helpers. */
MU_INLINE int32_t mu_rd_wrap_i(int32_t v, int32_t n)
{
    v %= (int32_t)n;
    return v < 0 ? v + (int32_t)n : v;
}

/* Advance one explicit-Euler step of size g->dt. */
MU_INLINE void mu_rd_step(mu_rd_grid* g)
{
    MU_ASSERT(g != NULL);
    uint32_t w = g->width, h = g->height;
    size_t ncells = (size_t)w * (size_t)h;

    float* un = (float*)MU_MALLOC(sizeof(float) * ncells);
    float* vn = (float*)MU_MALLOC(sizeof(float) * ncells);
    if(!un || !vn)
    {
        MU_FREE(un);
        MU_FREE(vn);
        return;
    }

    float Du = g->Du * g->dt;
    float Dv = g->Dv * g->dt;
    float F = g->F * g->dt;
    float k = g->k * g->dt;

    for(uint32_t y = 0; y < h; ++y)
    {
        uint32_t ym = y == 0u ? h - 1u : y - 1u;
        uint32_t yp = (y + 1u) % h;
        size_t rowm = (size_t)ym * (size_t)w;
        size_t row = (size_t)y * (size_t)w;
        size_t rowp = (size_t)yp * (size_t)w;

        for(uint32_t x = 0; x < w; ++x)
        {
            uint32_t xm = x == 0u ? w - 1u : x - 1u;
            uint32_t xp = (x + 1u) % w;

            size_t i = row + (size_t)x;
            float u = g->u[i];
            float v = g->v[i];

            float ul = g->u[row + (size_t)xm];
            float ur = g->u[row + (size_t)xp];
            float ut = g->u[rowm + (size_t)x];
            float ub = g->u[rowp + (size_t)x];
            float lap_u = ul + ur + ut + ub - 4.0f * u;

            float vl = g->v[row + (size_t)xm];
            float vr = g->v[row + (size_t)xp];
            float vt = g->v[rowm + (size_t)x];
            float vb = g->v[rowp + (size_t)x];
            float lap_v = vl + vr + vt + vb - 4.0f * v;

            float uv2 = u * v * v;

            un[i] = u + Du * lap_u - uv2 + F * (1.0f - u);
            vn[i] = v + Dv * lap_v + uv2 - k * v;

            /* guard against tiny negative drift */
            if(un[i] < 0.0f) un[i] = 0.0f;
            if(vn[i] < 0.0f) vn[i] = 0.0f;
            if(un[i] > 1.0f) un[i] = 1.0f;
            if(vn[i] > 1.0f) vn[i] = 1.0f;
        }
    }

    MU_MEMCPY(g->u, un, sizeof(float) * ncells);
    MU_MEMCPY(g->v, vn, sizeof(float) * ncells);
    MU_FREE(un);
    MU_FREE(vn);
}

/* Advance `n` explicit-Euler steps. */
MU_INLINE void mu_rd_run(mu_rd_grid* g, uint32_t n)
{
    MU_ASSERT(g != NULL);
    for(uint32_t i = 0; i < n; ++i)
        mu_rd_step(g);
}

MU_END_EXTERN_C
