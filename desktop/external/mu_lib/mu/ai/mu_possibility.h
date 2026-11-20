#pragma once

/*
===============================================================================

mu_possibility.h — 2D possibility map (bitmask grid).

Game AI Pro: "Possibility Maps for Tactical AI", "Spatial Reasoning with
Possibility Maps", "Cover and Flanking Analysis via Possibility Maps".

- A width x height grid where each cell holds a uint32_t bitmask of
  "possibilities" (e.g. bit0 = can stand, bit1 = can crouch, bit2 = cover
  from left, bit3 = flank route, ...).
- Per-cell get/set/or/and of bitmasks.
- Rectangular region AND/OR sweeps.
- Count / find-first cells matching a mask (ANY or ALL bits).
- 4-connected flood fill: spread a possibility mask from seed cells through
  connected cells, optionally blocked by cells carrying a "blocker" bit.

All flat arrays; no per-cell allocation after init.

===============================================================================
*/

#include "mu_common.h"

#include <stdlib.h>
#include <string.h>

MU_BEGIN_EXTERN_C

/* Match mode for count / find-first queries. */
typedef enum mu_poss_match
{
    MU_POSS_MATCH_ANY = 0, /* (cell & mask) != 0        */
    MU_POSS_MATCH_ALL = 1, /* (cell & mask) == mask     */
} mu_poss_match;

typedef struct mu_possibility_map
{
    uint32_t* cells;       /* [width * height] bitmask per cell   */
    uint8_t*  visited;     /* [width * height] stamp array (flood) */
    uint32_t  width;
    uint32_t  height;
    uint32_t  visit_stamp;
} mu_possibility_map;

/* ---------- lifecycle ---------- */

MU_INLINE bool mu_possibility_map_init(mu_possibility_map* m, uint32_t width,
                                       uint32_t height)
{
    MU_ASSERT(m != NULL);
    MU_ASSERT(width > 0u && height > 0u);

    size_t ncells = (size_t)width * (size_t)height;
    m->cells = (uint32_t*)MU_CALLOC(ncells, sizeof(uint32_t));
    m->visited = (uint8_t*)MU_CALLOC(ncells, sizeof(uint8_t));
    if(!m->cells || !m->visited)
    {
        MU_FREE(m->cells);
        MU_FREE(m->visited);
        m->cells = NULL;
        m->visited = NULL;
        return false;
    }

    m->width = width;
    m->height = height;
    m->visit_stamp = 1u;
    return true;
}

MU_INLINE void mu_possibility_map_destroy(mu_possibility_map* m)
{
    if(m == NULL)
        return;
    MU_FREE(m->cells);
    MU_FREE(m->visited);
    m->cells = NULL;
    m->visited = NULL;
    m->width = m->height = 0u;
    m->visit_stamp = 0u;
}

MU_INLINE void mu_possibility_map_clear(mu_possibility_map* m)
{
    MU_ASSERT(m != NULL);
    size_t ncells = (size_t)m->width * (size_t)m->height;
    MU_MEMSET(m->cells, 0, ncells * sizeof(uint32_t));
    MU_MEMSET(m->visited, 0, ncells * sizeof(uint8_t));
    m->visit_stamp = 1u;
}

MU_INLINE uint32_t mu_possibility_map_width(const mu_possibility_map* m)
{
    return m->width;
}
MU_INLINE uint32_t mu_possibility_map_height(const mu_possibility_map* m)
{
    return m->height;
}

/* ---------- cell access ---------- */

MU_INLINE uint32_t mu_possibility_map_cell(const mu_possibility_map* m,
                                           uint32_t x, uint32_t y)
{
    MU_ASSERT(m != NULL);
    MU_ASSERT(x < m->width && y < m->height);
    return m->cells[(size_t)y * (size_t)m->width + (size_t)x];
}

MU_INLINE void mu_possibility_map_set_cell(mu_possibility_map* m, uint32_t x,
                                           uint32_t y, uint32_t mask)
{
    MU_ASSERT(m != NULL);
    MU_ASSERT(x < m->width && y < m->height);
    m->cells[(size_t)y * (size_t)m->width + (size_t)x] = mask;
}

MU_INLINE bool mu_possibility_map_has(const mu_possibility_map* m, uint32_t x,
                                      uint32_t y, uint32_t bit,
                                      mu_poss_match mode)
{
    MU_ASSERT(m != NULL);
    if(x >= m->width || y >= m->height)
        return false;
    uint32_t cell = m->cells[(size_t)y * (size_t)m->width + (size_t)x];
    if(mode == MU_POSS_MATCH_ALL)
        return (cell & bit) == bit;
    return (cell & bit) != 0u;
}

MU_INLINE void mu_possibility_map_or(mu_possibility_map* m, uint32_t x,
                                     uint32_t y, uint32_t bits)
{
    MU_ASSERT(m != NULL);
    MU_ASSERT(x < m->width && y < m->height);
    m->cells[(size_t)y * (size_t)m->width + (size_t)x] |= bits;
}

MU_INLINE void mu_possibility_map_and(mu_possibility_map* m, uint32_t x,
                                      uint32_t y, uint32_t bits)
{
    MU_ASSERT(m != NULL);
    MU_ASSERT(x < m->width && y < m->height);
    m->cells[(size_t)y * (size_t)m->width + (size_t)x] &= bits;
}

/* ---------- region sweeps ---------- */

/* OR `bits` into every cell of the inclusive rectangle [x0,x1] x [y0,y1].
 * Out-of-range coordinates are clamped. */
MU_INLINE void mu_possibility_map_region_or(mu_possibility_map* m,
                                            uint32_t x0, uint32_t y0,
                                            uint32_t x1, uint32_t y1,
                                            uint32_t bits)
{
    MU_ASSERT(m != NULL);
    if(x0 > x1 || y0 > y1)
        return;
    uint32_t wx = m->width, hy = m->height;
    uint32_t sx = x0 < wx ? x0 : wx - 1u;
    uint32_t ex = x1 < wx ? x1 : wx - 1u;
    uint32_t sy = y0 < hy ? y0 : hy - 1u;
    uint32_t ey = y1 < hy ? y1 : hy - 1u;
    for(uint32_t y = sy; y <= ey; ++y)
    {
        size_t row = (size_t)y * (size_t)wx;
        for(uint32_t x = sx; x <= ex; ++x)
            m->cells[row + (size_t)x] |= bits;
    }
}

/* AND `bits` into every cell of the inclusive rectangle. Clamped. */
MU_INLINE void mu_possibility_map_region_and(mu_possibility_map* m,
                                             uint32_t x0, uint32_t y0,
                                             uint32_t x1, uint32_t y1,
                                             uint32_t bits)
{
    MU_ASSERT(m != NULL);
    if(x0 > x1 || y0 > y1)
        return;
    uint32_t wx = m->width, hy = m->height;
    uint32_t sx = x0 < wx ? x0 : wx - 1u;
    uint32_t ex = x1 < wx ? x1 : wx - 1u;
    uint32_t sy = y0 < hy ? y0 : hy - 1u;
    uint32_t ey = y1 < hy ? y1 : hy - 1u;
    for(uint32_t y = sy; y <= ey; ++y)
    {
        size_t row = (size_t)y * (size_t)wx;
        for(uint32_t x = sx; x <= ex; ++x)
            m->cells[row + (size_t)x] &= bits;
    }
}

/* ---------- count / find ---------- */

/* Count cells whose mask matches `mask` under `mode`. */
MU_INLINE uint32_t mu_possibility_map_count(const mu_possibility_map* m,
                                            uint32_t mask, mu_poss_match mode)
{
    MU_ASSERT(m != NULL);
    size_t ncells = (size_t)m->width * (size_t)m->height;
    uint32_t n = 0u;
    for(size_t i = 0; i < ncells; ++i)
    {
        uint32_t cell = m->cells[i];
        bool hit = (mode == MU_POSS_MATCH_ALL) ? ((cell & mask) == mask)
                                               : ((cell & mask) != 0u);
        if(hit)
            ++n;
    }
    return n;
}

/* Find the first cell (row-major) matching `mask` under `mode`.
 * Returns true and fills out_x/out_y, or false if none. */
MU_INLINE bool mu_possibility_map_find_first(const mu_possibility_map* m,
                                             uint32_t mask, mu_poss_match mode,
                                             uint32_t* out_x, uint32_t* out_y)
{
    MU_ASSERT(m != NULL);
    MU_ASSERT(out_x != NULL && out_y != NULL);
    uint32_t w = m->width, h = m->height;
    for(uint32_t y = 0; y < h; ++y)
    {
        size_t row = (size_t)y * (size_t)w;
        for(uint32_t x = 0; x < w; ++x)
        {
            uint32_t cell = m->cells[row + (size_t)x];
            bool hit = (mode == MU_POSS_MATCH_ALL) ? ((cell & mask) == mask)
                                                   : ((cell & mask) != 0u);
            if(hit)
            {
                *out_x = x;
                *out_y = y;
                return true;
            }
        }
    }
    return false;
}

/* ---------- flood fill ---------- */

/* 4-connected flood fill.
 *
 * Seed cells are those matching `seed_mask` under `seed_mode`. From each seed,
 * BFS spreads to 4-neighbors, OR-ing `spread_mask` into every visited cell.
 * Propagation does not cross a cell whose mask has `blocker` bit set, nor a
 * cell already visited this call. Returns the number of cells visited
 * (seeds included). */
MU_INLINE uint32_t mu_possibility_map_flood(mu_possibility_map* m,
                                            uint32_t seed_mask,
                                            mu_poss_match seed_mode,
                                            uint32_t spread_mask,
                                            uint32_t blocker)
{
    MU_ASSERT(m != NULL);
    uint32_t w = m->width, h = m->height;
    size_t ncells = (size_t)w * (size_t)h;

    ++m->visit_stamp;
    if(m->visit_stamp == 0u)
    {
        /* stamp overflowed; reset visited array */
        MU_MEMSET(m->visited, 0, ncells * sizeof(uint8_t));
        m->visit_stamp = 1u;
    }

    static const int32_t dx[4] = {1, -1, 0, 0};
    static const int32_t dy[4] = {0, 0, 1, -1};

    /* Simple array queue of cell indices. */
    uint32_t* queue = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * ncells);
    if(!queue)
        return 0u;

    uint32_t head = 0, tail = 0, filled = 0u;

    /* Enqueue all seed cells. */
    for(uint32_t y = 0; y < h; ++y)
    {
        size_t row = (size_t)y * (size_t)w;
        for(uint32_t x = 0; x < w; ++x)
        {
            size_t idx = row + (size_t)x;
            uint32_t cell = m->cells[idx];
            bool is_seed = (seed_mode == MU_POSS_MATCH_ALL)
                               ? ((cell & seed_mask) == seed_mask)
                               : ((cell & seed_mask) != 0u);
            if(!is_seed)
                continue;
            if(m->visited[idx] == m->visit_stamp)
                continue;
            m->visited[idx] = m->visit_stamp;
            queue[tail++] = idx;
        }
    }

    while(head < tail)
    {
        size_t idx = queue[head++];
        uint32_t x = (uint32_t)(idx % (size_t)w);
        uint32_t y = (uint32_t)(idx / (size_t)w);

        /* Apply the spread mask to this cell. */
        m->cells[idx] |= spread_mask;
        ++filled;

        for(int d = 0; d < 4; ++d)
        {
            int32_t nx = (int32_t)x + dx[d];
            int32_t ny = (int32_t)y + dy[d];
            if(nx < 0 || (uint32_t)nx >= w || ny < 0 || (uint32_t)ny >= h)
                continue;
            size_t nidx = (size_t)ny * (size_t)w + (size_t)nx;
            if(m->visited[nidx] == m->visit_stamp)
                continue;
            /* Blocker cells stop propagation (they are not visited). */
            if((m->cells[nidx] & blocker) != 0u)
                continue;
            m->visited[nidx] = m->visit_stamp;
            queue[tail++] = nidx;
        }
    }

    MU_FREE(queue);
    return filled;
}

MU_END_EXTERN_C
