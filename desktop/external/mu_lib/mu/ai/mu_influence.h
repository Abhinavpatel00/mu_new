#pragma once

/*
===============================================================================

mu_influence.h — 2D influence map with flood-fill decay.

Tactical AI toolkit (Game AI Pro: "Modular Tactical Influence Maps",
"Infinite-Resolution Influence Mapping", "Flooding the Influence Map for
Chase in Dishonored 2").

- A grid of cells, each holding a float influence value.
- Sources inject influence at a cell; influence decays with distance.
- Re-flood each frame via breadth-first wave fronts from every source;
  cell value = max over all sources.
- Gradient query returns the direction of increasing influence.

All flat arrays; no per-cell allocation.

===============================================================================
*/

#include "mu_common.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

MU_BEGIN_EXTERN_C

typedef struct mu_influence
{
    float*  cells;      /* [width * height] */
    int32_t width;
    int32_t height;

    int32_t*  src_x;
    int32_t*  src_y;
    float*  src_strength;
    uint32_t  src_count;
    uint32_t  src_cap;

    int32_t*  queue;    /* BFS frontier */
    uint8_t*  visited;  /* [width*height] stamp array */
    uint32_t  visit_stamp;

    float decay_per_cell;   /* 0..1 multiplier per step outward */
    float min_influence;    /* flood stops below this */
} mu_influence;

MU_INLINE int32_t mu_influence_xy_to_cell(const mu_influence* inf, int32_t x,
                                          int32_t y)
{
    return y * inf->width + x;
}

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_influence_init(mu_influence* inf, int32_t width,
                                 int32_t height)
{
    MU_ASSERT(inf != NULL);
    MU_ASSERT(width > 0 && height > 0);

    size_t ncells = (size_t)width * (size_t)height;
    inf->cells = (float*)MU_CALLOC(ncells, sizeof(float));
    inf->visited = (uint8_t*)MU_CALLOC(ncells, sizeof(uint8_t));
    inf->src_x = (int32_t*)MU_MALLOC(sizeof(int32_t) * 64u);
    inf->src_y = (int32_t*)MU_MALLOC(sizeof(int32_t) * 64u);
    inf->src_strength = (float*)MU_MALLOC(sizeof(float) * 64u);
    inf->queue = (int32_t*)MU_MALLOC(sizeof(int32_t) * ncells);

    bool ok = inf->cells && inf->visited && inf->src_x && inf->src_y &&
              inf->src_strength && inf->queue;
    if(!ok)
    {
        MU_FREE(inf->cells);
        MU_FREE(inf->visited);
        MU_FREE(inf->src_x);
        MU_FREE(inf->src_y);
        MU_FREE(inf->src_strength);
        MU_FREE(inf->queue);
        inf->cells = NULL;
        inf->visited = NULL;
        inf->src_x = NULL;
        inf->src_y = NULL;
        inf->src_strength = NULL;
        inf->queue = NULL;
        return false;
    }

    inf->width = width;
    inf->height = height;
    inf->src_count = 0;
    inf->src_cap = 64u;
    inf->visit_stamp = 1u;
    inf->decay_per_cell = 0.9f;
    inf->min_influence = 0.01f;
    return true;
}

MU_INLINE void mu_influence_destroy(mu_influence* inf)
{
    if(inf == NULL)
        return;
    MU_FREE(inf->cells);
    MU_FREE(inf->visited);
    MU_FREE(inf->src_x);
    MU_FREE(inf->src_y);
    MU_FREE(inf->src_strength);
    MU_FREE(inf->queue);
    inf->cells = NULL;
    inf->visited = NULL;
    inf->src_x = NULL;
    inf->src_y = NULL;
    inf->src_strength = NULL;
    inf->queue = NULL;
    inf->width = inf->height = 0;
    inf->src_count = inf->src_cap = 0;
    inf->visit_stamp = 0;
}

MU_INLINE void mu_influence_clear(mu_influence* inf)
{
    MU_ASSERT(inf != NULL);
    size_t ncells = (size_t)inf->width * (size_t)inf->height;
    MU_MEMSET(inf->cells, 0, ncells * sizeof(float));
    MU_MEMSET(inf->visited, 0, ncells * sizeof(uint8_t));
    inf->src_count = 0;
    inf->visit_stamp = 1u;
}

MU_INLINE void mu_influence_set_decay(mu_influence* inf, float decay_per_cell,
                                      float min_influence)
{
    MU_ASSERT(inf != NULL);
    inf->decay_per_cell = MU_CLAMP(decay_per_cell, 0.0f, 1.0f);
    inf->min_influence = min_influence;
}

MU_INLINE int32_t mu_influence_width(const mu_influence* inf)  { return inf->width; }
MU_INLINE int32_t mu_influence_height(const mu_influence* inf) { return inf->height; }

/* ---------- sources ---------- */

MU_INLINE bool mu_influence_add_source(mu_influence* inf, int32_t x, int32_t y,
                                       float strength)
{
    MU_ASSERT(inf != NULL);
    if(x < 0 || x >= inf->width || y < 0 || y >= inf->height)
        return false;
    if(inf->src_count == inf->src_cap)
    {
        uint32_t nc = inf->src_cap * 2u;
        int32_t* sx = (int32_t*)MU_REALLOC(inf->src_x, sizeof(int32_t) * nc);
        int32_t* sy = (int32_t*)MU_REALLOC(inf->src_y, sizeof(int32_t) * nc);
        float* ss = (float*)MU_REALLOC(inf->src_strength, sizeof(float) * nc);
        if(!sx || !sy || !ss)
            return false;
        inf->src_x = sx;
        inf->src_y = sy;
        inf->src_strength = ss;
        inf->src_cap = nc;
    }
    inf->src_x[inf->src_count] = x;
    inf->src_y[inf->src_count] = y;
    inf->src_strength[inf->src_count] = strength;
    ++inf->src_count;
    return true;
}

/* ---------- flood fill ---------- */

/* Recompute influence grid from all sources. Multi-source BFS: each cell gets
 * max(decayed strength from any source that reaches it). */
MU_INLINE void mu_influence_flood(mu_influence* inf)
{
    MU_ASSERT(inf != NULL);

    size_t ncells = (size_t)inf->width * (size_t)inf->height;
    MU_MEMSET(inf->cells, 0, ncells * sizeof(float));
    MU_MEMSET(inf->visited, 0, ncells * sizeof(uint8_t));
    ++inf->visit_stamp;
    if(inf->visit_stamp == 0u)
    {
        /* stamp overflowed; reset */
        MU_MEMSET(inf->visited, 0, ncells * sizeof(uint8_t));
        inf->visit_stamp = 1u;
    }

    static const int32_t dx[4] = {1, -1, 0, 0};
    static const int32_t dy[4] = {0, 0, 1, -1};

    /* Precompute decay^dist table up to max possible distance. */
    uint32_t max_dist = (uint32_t)inf->width + (uint32_t)inf->height;
    float* decay = (float*)MU_MALLOC(sizeof(float) * (size_t)(max_dist + 1u));
    if(!decay)
        return;
    decay[0] = 1.0f;
    for(uint32_t d = 1; d <= max_dist; ++d)
        decay[d] = decay[d - 1] * inf->decay_per_cell;

    for(uint32_t s = 0; s < inf->src_count; ++s)
    {
        int32_t sx = inf->src_x[s];
        int32_t sy = inf->src_y[s];
        float strength = inf->src_strength[s];
        if(strength <= inf->min_influence)
            continue;

        int32_t start = sy * inf->width + sx;
        uint32_t head = 0, tail = 0;
        inf->queue[tail++] = start;
        inf->visited[start] = inf->visit_stamp;

        /* Parallel distance array is avoided by level-by-level BFS. */
        uint32_t level_end = tail;
        uint32_t dist = 0;

        while(head < tail)
        {
            int32_t cell = inf->queue[head++];
            float decayed = strength * decay[dist];
            if(decayed < inf->min_influence)
            {
                /* This level and beyond are too weak; stop expanding this
                 * source but drain nothing more. */
                break;
            }
            if(decayed > inf->cells[cell])
                inf->cells[cell] = decayed;

            int32_t cx = cell % inf->width;
            int32_t cy = cell / inf->width;

            /* Only expand while we are processing the current level and the
             * next level would still be above min influence. */
            float next_decay = strength * decay[dist + 1u];
            bool expand = (next_decay >= inf->min_influence);

            if(expand)
            {
                for(int d = 0; d < 4; ++d)
                {
                    int32_t nx = cx + dx[d];
                    int32_t ny = cy + dy[d];
                    if(nx < 0 || nx >= inf->width || ny < 0 || ny >= inf->height)
                        continue;
                    int32_t ncell = ny * inf->width + nx;
                    if(inf->visited[ncell] == inf->visit_stamp)
                        continue;
                    inf->visited[ncell] = inf->visit_stamp;
                    inf->queue[tail++] = ncell;
                }
            }

            if(head == level_end)
            {
                level_end = tail;
                ++dist;
                if(dist > max_dist)
                    break;
            }
        }
    }

    MU_FREE(decay);
}

/* ---------- queries ---------- */

MU_INLINE float mu_influence_get(const mu_influence* inf, int32_t x, int32_t y)
{
    MU_ASSERT(inf != NULL);
    if(x < 0 || x >= inf->width || y < 0 || y >= inf->height)
        return 0.0f;
    return inf->cells[(size_t)y * (size_t)inf->width + (size_t)x];
}

/* Gradient at (x, y): direction of steepest increase. Returns false if flat. */
MU_INLINE bool mu_influence_gradient(const mu_influence* inf, int32_t x,
                                     int32_t y, float* gx, float* gy)
{
    MU_ASSERT(inf != NULL);
    *gx = 0.0f;
    *gy = 0.0f;
    if(x < 0 || x >= inf->width || y < 0 || y >= inf->height)
        return false;

    size_t here = (size_t)y * (size_t)inf->width + (size_t)x;
    float c = inf->cells[here];

    float best_dx = 0.0f, best_dy = 0.0f;
    float best_mag = 0.0f;

    /* sample the four neighbors; pick the max increase direction */
    struct { int32_t dx, dy; } nb[4] = {{1,0},{-1,0},{0,1},{0,-1}};
    for(int i = 0; i < 4; ++i)
    {
        int32_t nx = x + nb[i].dx;
        int32_t ny = y + nb[i].dy;
        if(nx < 0 || nx >= inf->width || ny < 0 || ny >= inf->height)
            continue;
        float v = inf->cells[(size_t)ny * (size_t)inf->width + (size_t)nx];
        float diff = v - c;
        float mag = diff > 0.0f ? diff : -diff;
        if(mag > best_mag)
        {
            best_mag = mag;
            best_dx = (float)nb[i].dx;
            best_dy = (float)nb[i].dy;
        }
    }

    if(best_mag < 1e-6f)
        return false;
    float len = sqrtf(best_dx * best_dx + best_dy * best_dy);
    *gx = best_dx / len;
    *gy = best_dy / len;
    return true;
}

MU_END_EXTERN_C
