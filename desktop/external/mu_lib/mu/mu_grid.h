#pragma once

/*
===============================================================================

mu_grid.h — chunked 2D tile grid for engine/game code.

Design (DOD):
  - A tile is a u32 (tileset / atlas index). Empty = 0.
  - The world is split into fixed chunks (default 16x16 tiles). Chunks are
    allocated lazily and stored contiguously — one flat tile[] array per chunk,
    packed in a slab so iteration over loaded chunks is cache-friendly.
  - Chunk lookup is an open-addressing map from packed (cx, cy) -> chunk slot.
  - No per-tile allocation, no pointer chasing inside a chunk.

Canonical surface: init / init_static / destroy / clear, count/capacity, the two
hot ops (get/set), a span-based iteration form, plus autotile bitmask helpers.

===============================================================================
*/

#include "mu_common.h"
#include "mu_span.h"

#ifndef MU_GRID_CHUNK_BITS
#  define MU_GRID_CHUNK_BITS 4u   /* 16 x 16 tiles per chunk */
#endif
#define MU_GRID_CHUNK_SIZE (1u << MU_GRID_CHUNK_BITS)      /* 16 */
#define MU_GRID_CHUNK_TILES (MU_GRID_CHUNK_SIZE * MU_GRID_CHUNK_SIZE) /* 256 */
#define MU_GRID_CHUNK_MASK (MU_GRID_CHUNK_SIZE - 1u)

MU_BEGIN_EXTERN_C

typedef struct mu_grid
{
    uint32_t tile_w;   /* pixel width  of one tile */
    uint32_t tile_h;   /* pixel height of one tile */

    /* ---- chunk slot pool (dense, cache-friendly iteration) ---- */
    int32_t*  chunk_cx;   /* chunk x coordinate of slot i, or -1 if empty */
    int32_t*  chunk_cy;   /* chunk y coordinate of slot i */
    uint32_t* chunk_tiles;/* [slot_count * CHUNK_TILES] tile ids */
    uint32_t  chunk_count;
    uint32_t  chunk_cap;

    /* ---- sparse map: packed (cx,cy) -> slot index (or -1) ---- */
    uint64_t* map_keys;   /* packed chunk coordinate */
    int32_t*  map_vals;   /* slot index, -1 = empty */
    uint32_t  map_cap;    /* power of two */
    uint32_t  map_count;
} mu_grid;

/* Forwarded: defined below after mu_grid_init. */
MU_INLINE void mu_grid_destroy(mu_grid* g);

/* Packed chunk coordinate key for the open-addressing map. */
MU_INLINE uint64_t mu_grid_pack(int32_t cx, int32_t cy)
{
    return ((uint64_t)(uint32_t)cx << 32) | (uint64_t)(uint32_t)(cy);
}

/* ---------- lifecycle ---------- */

/* Allocate a grid with room for `max_chunks` live chunks. */
MU_INLINE bool mu_grid_init(mu_grid* g, uint32_t tile_w, uint32_t tile_h,
                            uint32_t max_chunks)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(tile_w > 0u && tile_h > 0u);
    MU_ASSERT(max_chunks > 0u);

    g->tile_w = tile_w;
    g->tile_h = tile_h;
    g->chunk_count = 0;
    g->chunk_cap = max_chunks;
    g->map_count = 0;

    /* Map capacity: next power of two >= max_chunks * 2 (keep load factor < 0.5). */
    uint32_t mc = max_chunks * 2u;
    uint32_t cap = 8u;
    while(cap < mc)
        cap <<= 1;
    g->map_cap = cap;

    g->chunk_cx = (int32_t*)MU_MALLOC(sizeof(int32_t) * max_chunks);
    g->chunk_cy = (int32_t*)MU_MALLOC(sizeof(int32_t) * max_chunks);
    g->chunk_tiles = (uint32_t*)MU_MALLOC(sizeof(uint32_t) * max_chunks *
                                          MU_GRID_CHUNK_TILES);
    g->map_keys = (uint64_t*)MU_MALLOC(sizeof(uint64_t) * cap);
    g->map_vals = (int32_t*)MU_MALLOC(sizeof(int32_t) * cap);

    bool ok = g->chunk_cx && g->chunk_cy && g->chunk_tiles &&
              g->map_keys && g->map_vals;
    if(ok)
    {
        MU_MEMSET(g->map_vals, 0xFF, sizeof(int32_t) * cap); /* -1 = empty */
        MU_MEMSET(g->chunk_tiles, 0,
                  sizeof(uint32_t) * max_chunks * MU_GRID_CHUNK_TILES);
    }
    else
    {
        mu_grid_destroy(g);
    }
    return ok;
}

/* Static variant: caller supplies all storage, no malloc at all. */
MU_INLINE bool mu_grid_init_static(mu_grid* g, uint32_t tile_w, uint32_t tile_h,
                                   uint32_t max_chunks,
                                   int32_t* chunk_cx, int32_t* chunk_cy,
                                   uint32_t* chunk_tiles,
                                   uint64_t* map_keys, int32_t* map_vals,
                                   uint32_t map_cap)
{
    MU_ASSERT(g != NULL);
    MU_ASSERT(tile_w > 0u && tile_h > 0u);
    MU_ASSERT(max_chunks > 0u && map_cap > 0u);

    g->tile_w = tile_w;
    g->tile_h = tile_h;
    g->chunk_count = 0;
    g->chunk_cap = max_chunks;
    g->map_count = 0;
    g->map_cap = map_cap;

    g->chunk_cx = chunk_cx;
    g->chunk_cy = chunk_cy;
    g->chunk_tiles = chunk_tiles;
    g->map_keys = map_keys;
    g->map_vals = map_vals;
    MU_MEMSET(g->map_vals, 0xFF, sizeof(int32_t) * map_cap);
    MU_MEMSET(g->chunk_tiles, 0,
              sizeof(uint32_t) * max_chunks * MU_GRID_CHUNK_TILES);
    return true;
}

MU_INLINE void mu_grid_destroy(mu_grid* g)
{
    if(g == NULL)
        return;
    MU_FREE(g->chunk_cx);
    MU_FREE(g->chunk_cy);
    MU_FREE(g->chunk_tiles);
    MU_FREE(g->map_keys);
    MU_FREE(g->map_vals);
    g->chunk_cx = NULL;
    g->chunk_cy = NULL;
    g->chunk_tiles = NULL;
    g->map_keys = NULL;
    g->map_vals = NULL;
    g->chunk_count = 0;
    g->chunk_cap = 0;
    g->map_cap = 0;
    g->map_count = 0;
}

MU_INLINE void mu_grid_clear(mu_grid* g)
{
    MU_ASSERT(g != NULL);
    g->chunk_count = 0;
    g->map_count = 0;
    MU_MEMSET(g->map_vals, 0xFF, sizeof(int32_t) * g->map_cap);
    MU_MEMSET(g->chunk_tiles, 0,
              sizeof(uint32_t) * g->chunk_cap * MU_GRID_CHUNK_TILES);
}

/* ---------- queries ---------- */

MU_INLINE uint32_t mu_grid_tile_width(const mu_grid* g)  { return g->tile_w; }
MU_INLINE uint32_t mu_grid_tile_height(const mu_grid* g) { return g->tile_h; }
MU_INLINE uint32_t mu_grid_chunk_count(const mu_grid* g) { return g->chunk_count; }

/* World tile coordinate -> chunk coordinate + tile-in-chunk. */
MU_INLINE void mu_grid_to_chunk(int32_t tx, int32_t ty, int32_t* cx, int32_t* cy,
                                int32_t* lx, int32_t* ly)
{
    /* Arithmetic shift floors toward negative infinity for negatives. */
    *cx = tx >> MU_GRID_CHUNK_BITS;
    *cy = ty >> MU_GRID_CHUNK_BITS;
    *lx = tx & (int32_t)MU_GRID_CHUNK_MASK;
    *ly = ty & (int32_t)MU_GRID_CHUNK_MASK;
}

/* Find the slot for a chunk, inserting an empty one if missing. Returns slot
 * index, or -1 if the grid is full. `must_exist` when false creates the chunk
 * (zero-filled) if absent; when true returns -1 for an absent chunk. */
/* Const lookup: returns slot index or -1 if the chunk is absent. */
MU_INLINE int32_t mu_grid_find_slot(const mu_grid* g, int32_t cx, int32_t cy)
{
    uint64_t key = mu_grid_pack(cx, cy);
    uint32_t cap = g->map_cap;
    uint32_t mask = cap - 1u;
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32);
    for(uint32_t probe = 0; probe < cap; ++probe)
    {
        uint32_t idx = (h + probe) & mask;
        if(g->map_vals[idx] < 0)
            return -1;
        if(g->map_keys[idx] == key)
            return g->map_vals[idx];
    }
    return -1;
}

/* Mutating locate: finds an existing slot or creates an empty chunk. Returns
 * slot index, or -1 if the grid is full and the chunk must be allocated. */
MU_INLINE int32_t mu_grid_locate_slot(mu_grid* g, int32_t cx, int32_t cy,
                                      bool must_exist)
{
    if(!must_exist)
    {
        int32_t existing = mu_grid_find_slot(g, cx, cy);
        if(existing >= 0)
            return existing;
    }
    else
    {
        return mu_grid_find_slot(g, cx, cy);
    }

    /* Must create. */
    if(g->chunk_count >= g->chunk_cap)
        return -1; /* full */
    uint64_t key = mu_grid_pack(cx, cy);
    uint32_t cap = g->map_cap;
    uint32_t mask = cap - 1u;
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32);
    for(uint32_t probe = 0;; ++probe)
    {
        uint32_t idx = (h + probe) & mask;
        if(g->map_vals[idx] < 0)
        {
            uint32_t slot = g->chunk_count++;
            g->map_keys[idx] = key;
            g->map_vals[idx] = (int32_t)slot;
            g->chunk_cx[slot] = cx;
            g->chunk_cy[slot] = cy;
            MU_MEMSET(g->chunk_tiles + slot * MU_GRID_CHUNK_TILES, 0,
                      sizeof(uint32_t) * MU_GRID_CHUNK_TILES);
            g->map_count++;
            return (int32_t)slot;
        }
        if(g->map_keys[idx] == key)
            return g->map_vals[idx];
    }
}

/* Read a tile id at world tile coordinate (tx, ty). Returns 0 if the chunk is
 * unloaded (empty). */
MU_INLINE uint32_t mu_grid_get(const mu_grid* g, int32_t tx, int32_t ty)
{
    int32_t cx, cy, lx, ly;
    mu_grid_to_chunk(tx, ty, &cx, &cy, &lx, &ly);
    int32_t slot = mu_grid_find_slot(g, cx, cy);
    if(slot < 0)
        return 0u;
    return g->chunk_tiles[(size_t)slot * MU_GRID_CHUNK_TILES +
                          (size_t)ly * MU_GRID_CHUNK_SIZE + lx];
}

/* Write a tile id. Creates the chunk if needed; returns false if the grid is
 * full and the chunk must be allocated. */
MU_INLINE bool mu_grid_set(mu_grid* g, int32_t tx, int32_t ty, uint32_t tile)
{
    int32_t cx, cy, lx, ly;
    mu_grid_to_chunk(tx, ty, &cx, &cy, &lx, &ly);
    int32_t slot = mu_grid_locate_slot(g, cx, cy, false);
    if(slot < 0)
        return false;
    g->chunk_tiles[(size_t)slot * MU_GRID_CHUNK_TILES +
                   (size_t)ly * MU_GRID_CHUNK_SIZE + lx] = tile;
    return true;
}

/* ---------- iteration (span form) ---------- */

/* Span over all tiles of chunk at slot `slot`. Valid until the grid grows. */
MU_INLINE mu_span_u32 mu_grid_chunk_span(const mu_grid* g, uint32_t slot)
{
    MU_ASSERT(slot < g->chunk_count);
    return mu_span_u32_make(g->chunk_tiles + (size_t)slot * MU_GRID_CHUNK_TILES,
                            MU_GRID_CHUNK_TILES);
}

/* Callback form: invoke f(slot, cx, cy, tile_span) for every loaded chunk.
 * Return false from f to stop early. */
MU_INLINE void mu_grid_foreach_chunk(const mu_grid* g,
                                     bool (*f)(uint32_t slot, int32_t cx,
                                               int32_t cy, mu_span_u32 tiles,
                                               void* ctx),
                                     void* ctx)
{
    for(uint32_t i = 0; i < g->chunk_count; ++i)
    {
        if(!f(i, g->chunk_cx[i], g->chunk_cy[i],
              mu_grid_chunk_span(g, i), ctx))
            break;
    }
}

/* ===========================================================================
 * Autotile bitmask (47-rule style)
 * ===========================================================================
 *
 * Given a center tile and its 8 neighbors, produce a bitmask where bit b is set
 * when neighbor b matches the center tile ("same" bitmask, the most common
 * autotile convention for ground/road tilesets):
 *
 *     1 2 4
 *     8 X 16
 *     32 64 128
 *
 * mu_grid_autotile_match_mask returns this mask given a get-tile callback.
 * mu_grid_autotile_47 returns the 47-tile tileset index for a match mask
 * (requires a tileset laid out in the standard 47-rule order).
 * =========================================================================== */

MU_INLINE uint32_t mu_grid_autotile_match_mask(uint32_t (*get)(void* ctx,
                                                               int32_t tx,
                                                               int32_t ty),
                                               void* ctx, int32_t cx,
                                               int32_t cy)
{
    uint32_t center = get(ctx, cx, cy);
    uint32_t mask = 0u;
    static const int32_t dx[8] = {0, 1, 0, -1, -1, 1, 1, -1};
    static const int32_t dy[8] = {-1, 0, 1, 0, -1, -1, 1, 1};
    static const uint32_t bit[8] = {1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u};
    for(int i = 0; i < 8; ++i)
    {
        if(get(ctx, cx + dx[i], cy + dy[i]) == center)
            mask |= bit[i];
    }
    return mask;
}

/* Map a match-mask (bits for N/E/S/W + diagonals) to a 47-rule tile index.
 * Returns 0..46, or 47 (=> use center tile as-is) when the mask has no standard
 * entry. This covers single tiles, corners, edges, and crosses of the classic
 * 47-rule autotiling set. */
MU_INLINE uint32_t mu_grid_autotile_47(uint32_t mask)
{
    /* bit order: 1=N,2=E,4=S,8=W,16=NW,32=NE,64=SE,128=SW */
    uint32_t n = MU_HAS_FLAG(mask, 1u);
    uint32_t e = MU_HAS_FLAG(mask, 2u);
    uint32_t s = MU_HAS_FLAG(mask, 4u);
    uint32_t w = MU_HAS_FLAG(mask, 8u);
    uint32_t nw = MU_HAS_FLAG(mask, 16u);
    uint32_t ne = MU_HAS_FLAG(mask, 32u);
    uint32_t se = MU_HAS_FLAG(mask, 64u);
    uint32_t sw = MU_HAS_FLAG(mask, 128u);

    /* Handle by cardinal count + corners. */
    uint32_t card = n + e + s + w;

    if(card == 0u)
        return 0u; /* isolated */

    /* Single edge. */
    if(card == 1u)
    {
        if(n) return 1u;
        if(e) return 2u;
        if(s) return 3u;
        return 4u; /* w */
    }

    /* Two adjacent corners (corner piece). */
    if(card == 2u)
    {
        if(n && e) return nw ? 5u : 6u;
        if(e && s) return ne ? 7u : 8u;
        if(s && w) return se ? 9u : 10u;
        if(w && n) return sw ? 11u : 12u;
        /* Opposite edges -> split/straight variants, simplified to straight. */
        if(n && s) return 13u;
        if(e && w) return 14u;
    }

    /* Three cardinals (T-junction). */
    if(card == 3u)
    {
        if(!n) return 15u;  /* missing N */
        if(!e) return 16u;  /* missing E */
        if(!s) return 17u;  /* missing S */
        return 18u;         /* missing W */
    }

    /* Four cardinals (cross / full). Corners decide inner vs outer corner. */
    if(card == 4u)
    {
        if(nw && ne && se && sw) return 19u; /* all corners -> full block */
        /* Otherwise a T with one corner filled; map by missing corner. */
        if(!nw) return 20u;
        if(!ne) return 21u;
        if(!se) return 22u;
        return 23u; /* !sw */
    }

    /* card >= 5 impossible (4 cardinals max). Return full. */
    return 19u;
}

MU_END_EXTERN_C
