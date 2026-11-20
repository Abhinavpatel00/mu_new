#include "mu.h"
#include "mu/mu_spsc_queue.h"

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

typedef struct TestTimerRecord
{
    uint32_t count;
    uint64_t sum_user;
} TestTimerRecord;

static void test_timer_fire_cb(uint32_t timer_id, uint64_t user_data, void* ctx)
{
    (void)timer_id;
    TestTimerRecord* rec = (TestTimerRecord*)ctx;
    ++rec->count;
    rec->sum_user += user_data;
}

/* ------------------------------------------------------------------ *
 * Macro layer smoke test                                              *
 * ------------------------------------------------------------------ */

MU_INLINE int sum_array(const int* MU_RESTRICT arr, size_t count)
{
    int sum = 0;
    for (size_t i = 0; i < count; ++i)
        sum += arr[i];
    return sum;
}

MU_NOINLINE static int slow_identity(int x) { return x; }

MU_ALIGN(64) static uint8_t aligned_buffer[64];

typedef struct { int id; double value; } Example;
typedef struct { int header; Example example; } Wrapper;

static int failures = 0;

static void deferred_count_free(void* ptr, void* user)
{
    (void)ptr;
    ++*(int*)user;
}

typedef struct TestRbItem
{
    mu_rb_node node;
    int key;
} TestRbItem;

static int test_rb_compare(const mu_rb_node* a, const mu_rb_node* b)
{
    const TestRbItem* ia = MU_CONTAINER_OF((mu_rb_node*)a, TestRbItem, node);
    const TestRbItem* ib = MU_CONTAINER_OF((mu_rb_node*)b, TestRbItem, node);
    return (ia->key > ib->key) - (ia->key < ib->key);
}

static int test_rb_search_compare(const mu_rb_node* node, const void* key)
{
    const TestRbItem* item = MU_CONTAINER_OF((mu_rb_node*)node, TestRbItem, node);
    int value = *(const int*)key;
    return (item->key > value) - (item->key < value);
}

typedef struct TestInterval
{
    mu_interval_node node;
    int id;
} TestInterval;

typedef struct TestSlabEntry
{
    mu_slab_manager_entry entry;
    bool reclaimable;
} TestSlabEntry;

typedef struct TestSlab
{
    mu_slab_manager_slab slab;
    TestSlabEntry entries[4];
} TestSlab;

static mu_slab_manager_slab* test_slab_alloc(void* priv, unsigned heap,
                                             unsigned entry_size,
                                             unsigned group_index)
{
    TestSlab* slab;
    unsigned i;
    MU_UNUSED(priv);
    MU_UNUSED(heap);

    slab = (TestSlab*)MU_CALLOC(1, sizeof(*slab));
    if (!slab)
        return NULL;

    mu_slab_manager_slab_init(&slab->slab, group_index, entry_size, 4);
    for (i = 0; i < 4; ++i)
        mu_slab_manager_slab_add_entry(&slab->slab, &slab->entries[i].entry);
    return &slab->slab;
}

static void test_slab_free(void* priv, mu_slab_manager_slab* slab)
{
    MU_UNUSED(priv);
    MU_FREE(MU_CONTAINER_OF(slab, TestSlab, slab));
}

static bool test_slab_can_reclaim(void* priv, mu_slab_manager_entry* entry)
{
    MU_UNUSED(priv);
    return MU_CONTAINER_OF(entry, TestSlabEntry, entry)->reclaimable;
}

typedef struct TestDagNode
{
    mu_dag_node node;
    int id;
} TestDagNode;

typedef struct TestDagVisit
{
    int ids[8];
    int count;
} TestDagVisit;

static void test_dag_visit(mu_dag_node* node, void* data)
{
    TestDagVisit* visit = (TestDagVisit*)data;
    TestDagNode* item = MU_CONTAINER_OF(node, TestDagNode, node);
    visit->ids[visit->count++] = item->id;
}

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__);              \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

MU_NOINLINE static void worklist_destroy_noinline(mu_worklist* w)
{
    mu_worklist_destroy(w);
}

static void check_spatial_array(void)
{
    mu_spatial_array a;
    int64_t min_x, min_y, min_z, max_x, max_y, max_z;

    mu_spatial_array_init(&a);

    /* first write anchors the origin */
    CHECK(mu_spatial_array_set(&a, 0, 0, 0, (void*)(uintptr_t)1));
    CHECK(mu_spatial_array_get(&a, 0, 0, 0) == (void*)(uintptr_t)1);
    CHECK(mu_spatial_array_contains(&a, 0, 0, 0));

    /* growing towards +inf keeps the origin, doubles the extent */
    CHECK(mu_spatial_array_set(&a, 5, 0, 0, (void*)(uintptr_t)2));
    CHECK(a.origin_x == 0 && a.width == 8);
    CHECK(mu_spatial_array_get(&a, 0, 0, 0) == (void*)(uintptr_t)1);
    CHECK(mu_spatial_array_get(&a, 5, 0, 0) == (void*)(uintptr_t)2);

    /* growing towards -inf keeps the far edge and pushes the origin down */
    CHECK(mu_spatial_array_set(&a, -5, 0, 0, (void*)(uintptr_t)3));
    CHECK(a.origin_x == -8 && a.width == 16);
    CHECK(mu_spatial_array_get(&a, 0, 0, 0) == (void*)(uintptr_t)1);
    CHECK(mu_spatial_array_get(&a, 5, 0, 0) == (void*)(uintptr_t)2);
    CHECK(mu_spatial_array_get(&a, -5, 0, 0) == (void*)(uintptr_t)3);

    /* out of range reads are rejected, not clamped */
    CHECK(!mu_spatial_array_contains(&a, 100, 0, 0));
    CHECK(mu_spatial_array_get(&a, 100, 0, 0) == NULL);

    /* the other two axes expand independently */
    CHECK(mu_spatial_array_set(&a, 0, 3, -2, (void*)(uintptr_t)4));
    CHECK(a.origin_y == 0 && a.height == 4);
    CHECK(a.origin_z == -3 && a.depth == 4);
    CHECK(mu_spatial_array_get(&a, 0, 3, -2) == (void*)(uintptr_t)4);
    CHECK(mu_spatial_array_get(&a, -5, 0, 0) == (void*)(uintptr_t)3);

    mu_spatial_array_bounds(&a, &min_x, &min_y, &min_z, &max_x, &max_y, &max_z);
    CHECK(min_x == -8 && max_x == 7);
    CHECK(min_y == 0  && max_y == 3);
    CHECK(min_z == -3 && max_z == 0);

    mu_spatial_array_clear(&a);
    CHECK(mu_spatial_array_get(&a, 0, 0, 0) == NULL);
    CHECK(a.width == 16 && a.height == 4 && a.depth == 4);

    mu_spatial_array_destroy(&a);
    CHECK(a.data == NULL && a.width == 0);
}

MU_NOINLINE static void check_worklist_static(void)
{
    mu_worklist w = {0};
    uint32_t entries[4];
    uint64_t present[1];
    uint32_t v = 0;

    CHECK(mu_worklist_init_static(&w, entries, present, 4));
    CHECK(mu_worklist_push_tail(&w, 2));
    CHECK(mu_worklist_pop_head(&w, &v) && v == 2);
    worklist_destroy_noinline(&w);
    CHECK(mu_worklist_capacity(&w) == 0);
}

/* =========================================================================
 * mu_files
 * ========================================================================= */

static void check_mu_files(void)
{
    char out[512];

    /* path join */
    uint32_t n = mu_files_path_join(out, sizeof(out), "dir", "file.txt");
    CHECK(n > 0 && n <= (uint32_t)sizeof(out));
    CHECK(MU_MEMCMP(out, "dir/file.txt\0", 13) == 0 ||
          MU_MEMCMP(out, "dir\\file.txt\0", 13) == 0);

    /* join with trailing sep on dir */
    n = mu_files_path_join(out, sizeof(out), "dir/", "file.txt");
    CHECK(n > 0 && n <= (uint32_t)sizeof(out));

    /* buffer too small -> returns required length */
    uint32_t need = mu_files_path_join(out, 2, "dir", "file.txt");
    CHECK(need > 2u);

    /* normalize: double slashes and ./ */
    n = mu_files_path_normalize(out, sizeof(out), "a//b/./c");
    CHECK(n > 0);
#if defined(_WIN32)
    CHECK(MU_MEMCMP(out, "a\\b\\c\0", 6) == 0);
#else
    CHECK(MU_MEMCMP(out, "a/b/c\0", 6) == 0);
#endif

    /* normalize .. */
    n = mu_files_path_normalize(out, sizeof(out), "a/b/../c");
    CHECK(n > 0);
#if defined(_WIN32)
    CHECK(MU_MEMCMP(out, "a\\c\0", 4) == 0);
#else
    CHECK(MU_MEMCMP(out, "a/c\0", 4) == 0);
#endif

    /* basename */
    n = mu_files_basename(out, sizeof(out), "dir/sub/file.txt");
    CHECK(n > 0);
#if defined(_WIN32)
    CHECK(MU_MEMCMP(out, "file.txt\0", 9) == 0 ||
          MU_MEMCMP(out, "file.txt\0", 9) == 0);
#else
    CHECK(MU_MEMCMP(out, "file.txt\0", 9) == 0);
#endif

    /* dirname */
    n = mu_files_dirname(out, sizeof(out), "dir/sub/file.txt");
    CHECK(n > 0);

    /* extension */
    n = mu_files_extension(out, sizeof(out), "dir/file.txt");
    CHECK(n > 0);
#if defined(_WIN32)
    CHECK(MU_MEMCMP(out, ".txt\0", 5) == 0);
#else
    CHECK(MU_MEMCMP(out, ".txt\0", 5) == 0);
#endif
    n = mu_files_extension(out, sizeof(out), "dir/file");
    CHECK(n == 1u); /* empty extension */

    /* path_exists on a known-nonexistent path */
    CHECK(!mu_files_path_exists("/__mu_nonexistent_xyz__/nope"));

    /* atomic write + read round trip */
    const char* contents = "hello mu files";
    bool ok = mu_files_write_atomic("mu_files_test.tmp", contents, 14);
    CHECK(ok);
    void* data = NULL;
    uint64_t size = 0;
    ok = mu_files_read_all("mu_files_test.tmp", &data, &size);
    CHECK(ok);
    CHECK(size == 14 && data != NULL);
    CHECK(MU_MEMCMP(data, contents, 14) == 0);
    MU_FREE(data);

    /* cleanup */
#if defined(_WIN32)
    DeleteFileA("mu_files_test.tmp");
#else
    unlink("mu_files_test.tmp");
#endif
}

/* =========================================================================
 * mu_grid
 * ========================================================================= */

static uint32_t grid_visit_count;
static bool grid_visit_chunk(uint32_t slot, int32_t cx, int32_t cy,
                             mu_span_u32 tiles, void* ctx)
{
    (void)slot; (void)cx; (void)cy; (void)tiles; (void)ctx;
    ++grid_visit_count;
    return true;
}
static uint32_t grid_all_one_cb(void* ctx, int32_t tx, int32_t ty)
{
    (void)ctx; (void)tx; (void)ty;
    return 1u;
}
static uint32_t grid_center_one_cb(void* ctx, int32_t tx, int32_t ty)
{
    (void)ctx;
    return (tx == 5 && ty == 5) ? 1u : 0u;
}

static void check_mu_grid(void)
{
    mu_grid g;
    /* 8x8 tiles, room for 64 chunks */
    CHECK(mu_grid_init(&g, 8, 8, 64));
    grid_visit_count = 0;

    /* empty grid reads as 0 */
    CHECK(mu_grid_get(&g, 0, 0) == 0u);
    CHECK(mu_grid_get(&g, 100, -100) == 0u);

    /* set + get */
    CHECK(mu_grid_set(&g, 0, 0, 7u));
    CHECK(mu_grid_get(&g, 0, 0) == 7u);

    /* different chunk (16,16 is next chunk with CHUNK_SIZE=16) */
    CHECK(mu_grid_set(&g, 16, 16, 3u));
    CHECK(mu_grid_get(&g, 16, 16) == 3u);
    CHECK(mu_grid_get(&g, 0, 0) == 7u); /* first still there */

    /* negative coordinates */
    CHECK(mu_grid_set(&g, -1, -1, 9u));
    CHECK(mu_grid_get(&g, -1, -1) == 9u);

    /* chunk count */
    CHECK(mu_grid_chunk_count(&g) == 3u);

    mu_grid_foreach_chunk(&g, grid_visit_chunk, NULL);
    CHECK(grid_visit_count == 3u);

    /* autotile match mask: all neighbors same -> 0xFF */
    uint32_t mask = mu_grid_autotile_match_mask(grid_all_one_cb, NULL, 5, 5);
    CHECK(mask == 0xFFu);

    /* center differs from all neighbors -> 0 */
    mask = mu_grid_autotile_match_mask(grid_center_one_cb, NULL, 5, 5);
    CHECK(mask == 0u);

    /* 47-rule: isolated -> 0 */
    CHECK(mu_grid_autotile_47(0u) == 0u);

    mu_grid_destroy(&g);
}

/* =========================================================================
 * mu_timeline
 * ========================================================================= */

static void check_mu_timeline(void)
{
    mu_timeline t;
    CHECK(mu_timeline_init(&t));

    /* add a float track */
    CHECK(mu_timeline_add_track(&t, 1, MU_TRACK_FLOAT, MU_LOOP_NONE,
                                NULL, NULL));
    float v0[4] = { 0.0f, 0, 0, 0 };
    float v1[4] = { 10.0f, 0, 0, 0 };
    CHECK(mu_timeline_add_key(&t, 1, 0.0f, v0, MU_EASE_LINEAR));
    CHECK(mu_timeline_add_key(&t, 1, 1.0f, v1, MU_EASE_LINEAR));

    /* duration */
    CHECK(t.duration == 1.0f);

    /* seek to start */
    mu_timeline_seek(&t, 0.0f);
    mu_track* tr = mu_timeline_find_track(&t, 1);
    CHECK(tr != NULL);
    CHECK(mu_track_eval_float(tr, 0.0f) == 0.0f);
    CHECK(mu_track_eval_float(tr, 1.0f) == 10.0f);
    CHECK(mu_track_eval_float(tr, 0.5f) == 5.0f); /* linear midpoint */

    /* easing: ease_in_quad at 0.5 -> 0.25 */
    CHECK(mu_track_eval_float(tr, 0.5f) == 5.0f);
    /* rebuild with ease */
    mu_timeline_destroy(&t);
    CHECK(mu_timeline_init(&t));
    CHECK(mu_timeline_add_track(&t, 2, MU_TRACK_FLOAT, MU_LOOP_LOOP,
                                NULL, NULL));
    float z[4] = {0,0,0,0}; float h[4] = {100,0,0,0};
    CHECK(mu_timeline_add_key(&t, 2, 0.0f, z, MU_EASE_IN_QUAD));
    CHECK(mu_timeline_add_key(&t, 2, 1.0f, h, MU_EASE_IN_QUAD));
    tr = mu_timeline_find_track(&t, 2);
    /* ease_in_quad(0.5) = 0.25, so value = 25 */
    float mid = mu_track_eval_float(tr, 0.5f);
    CHECK(mid > 24.0f && mid < 26.0f);
    CHECK(t.duration == 1.0f);

    /* markers */
    CHECK(mu_timeline_add_marker(&t, 0.5f, 42u));
    mu_timeline_reset_markers(&t);
    mu_timeline_play(&t);
    mu_timeline_seek(&t, 0.0f);
    bool fired = mu_timeline_update(&t, 0.6f);
    CHECK(fired);
    /* marker fired once */
    CHECK(t.markers[0].fired == true);

    /* loop wrap (track 2 is LOOP_LOOP, duration 1.0) */
    float w = mu_track_wrap(tr, 1.5f);
    CHECK(w > 0.49f && w < 0.51f); /* 1.5 mod 1.0 = 0.5 */
    float w2 = mu_track_wrap(tr, -0.2f);
    CHECK(w2 > 0.79f && w2 < 0.81f); /* wraps around */

    mu_timeline_destroy(&t);
}

/* =========================================================================
 * mu_geom
 * ========================================================================= */

static void check_mu_geom(void)
{
    /* AABB */
    mu_aabb b;
    mu_aabb_empty(&b);
    float pts[6] = { 0,0,0,  2,4,6 };
    mu_aabb_from_points(&b, pts, 2);
    CHECK(b.min[0] == 0.0f && b.min[1] == 0.0f && b.min[2] == 0.0f);
    CHECK(b.max[0] == 2.0f && b.max[1] == 4.0f && b.max[2] == 6.0f);

    float inside[3] = { 1,2,3 };
    float outside[3] = { 5,2,3 };
    CHECK(mu_aabb_contains_point(&b, inside));
    CHECK(!mu_aabb_contains_point(&b, outside));

    mu_aabb b2;
    mu_aabb_empty(&b2);
    float pts2[6] = { 1,1,1,  3,5,7 };
    mu_aabb_from_points(&b2, pts2, 2);
    CHECK(mu_aabb_intersects_aabb(&b, &b2));
    mu_aabb b3;
    mu_aabb_empty(&b3);
    float pts3[6] = { 10,10,10,  12,14,16 };
    mu_aabb_from_points(&b3, pts3, 2);
    CHECK(!mu_aabb_intersects_aabb(&b, &b3));

    /* Ray - AABB hit */
    mu_aabb box;
    mu_aabb_empty(&box);
    float bpts[6] = { -1,-1,-1, 1,1,1 };
    mu_aabb_from_points(&box, bpts, 2);
    mu_ray r;
    r.origin[0] = -5; r.origin[1] = 0; r.origin[2] = 0;
    r.dir[0] = 1; r.dir[1] = 0; r.dir[2] = 0;
    mu_hit h = mu_ray_aabb(&r, &box);
    CHECK(h.hit);
    CHECK(h.t > 3.9f && h.t < 4.1f); /* enters at x=-1, from x=-5 */
    CHECK(h.normal[0] == -1.0f || h.normal[0] == 1.0f);

    /* Ray - AABB miss (parallel, outside slab) */
    mu_ray r2;
    r2.origin[0] = 0; r2.origin[1] = 5; r2.origin[2] = 0;
    r2.dir[0] = 1; r2.dir[1] = 0; r2.dir[2] = 0;
    mu_hit h2 = mu_ray_aabb(&r2, &box);
    CHECK(!h2.hit);

    /* Ray - zero direction (point test) */
    mu_ray r3;
    r3.origin[0] = 0; r3.origin[1] = 0; r3.origin[2] = 0;
    r3.dir[0] = 0; r3.dir[1] = 0; r3.dir[2] = 0;
    mu_hit h3 = mu_ray_aabb(&r3, &box);
    CHECK(h3.hit && h3.t == 0.0f);

    /* Ray - sphere */
    mu_sphere s;
    s.center[0] = 0; s.center[1] = 0; s.center[2] = 0;
    s.radius = 1.0f;
    mu_ray r4;
    r4.origin[0] = -5; r4.origin[1] = 0; r4.origin[2] = 0;
    r4.dir[0] = 1; r4.dir[1] = 0; r4.dir[2] = 0;
    mu_hit h4 = mu_ray_sphere(&r4, &s);
    CHECK(h4.hit);
    CHECK(h4.t > 3.9f && h4.t < 6.1f);

    /* Swept AABB */
    mu_aabb mov;
    mu_aabb_empty(&mov);
    float mpts[6] = { 0,0,0, 1,1,1 };
    mu_aabb_from_points(&mov, mpts, 2);
    mu_aabb wall;
    mu_aabb_empty(&wall);
    float wpts[6] = { 5,0,0, 6,2,2 };
    mu_aabb_from_points(&wall, wpts, 2);
    float vel[3] = { 10, 0, 0 };
    mu_hit h5;
    bool hit = mu_swept_aabb(&mov, vel, &wall, &h5);
    CHECK(hit);
    CHECK(h5.t >= 0.0f && h5.t <= 1.0f);
    CHECK(h5.normal[0] == 1.0f || h5.normal[0] == -1.0f);

    /* Closest point segment-segment */
    float cp0[3], cp1[3];
    float d2 = mu_closest_point_segment_segment(
        (float[3]){0,0,0}, (float[3]){2,0,0},
        (float[3]){1,-1,0}, (float[3]){1,1,0},
        cp0, cp1);
    CHECK(d2 >= 0.0f && d2 < 0.01f); /* cross at (1,0,0) */
}

/* =========================================================================
 * mu_camera
 * ========================================================================= */

static void check_mu_camera(void)
{
    mu_camera c;
    mu_camera_init(&c);
    c.position[0] = 0; c.position[1] = 5; c.position[2] = -10;
    c.target[0] = 0; c.target[1] = 0; c.target[2] = 0;

    /* update toward a target */
    float target[3] = { 10, 0, 0 };
    float vel[3] = { 0, 0, 0 };
    mu_camera_update(&c, target, vel, 0.1f);
    /* should have moved closer to target x=10 */
    CHECK(c.position[0] > 0.0f && c.position[0] < 10.0f);

    /* shake impulse */
    mu_camera_shake(&c, 1.0f);
    float before[3]; MU_MEMCPY(before, c.position, sizeof(before));
    mu_camera_update(&c, target, vel, 0.05f);
    CHECK(c.position[0] != before[0] || c.position[2] != before[2]);

    /* view / proj / view_proj */
    float view[16], proj[16], vp[16];
    mu_camera_view(&c, view);
    mu_camera_proj(&c, 16.0f/9.0f, proj);
    mu_camera_view_proj(&c, 16.0f/9.0f, view, proj, vp);
    /* just check they're finite / non-identity */
    bool view_id = true;
    for(int i = 0; i < 16; ++i) view_id &= (view[i] == 0.0f);
    CHECK(!view_id);

    /* bounds clamp */
    mu_camera_set_bounds(&c, true,
                         (float[3]){-20,-20,-20}, (float[3]){20,20,20});
    float far_target[3] = { 1000, 0, 0 };
    mu_camera_update(&c, far_target, vel, 1.0f);
    CHECK(c.position[0] <= 20.0f);
}

/* =========================================================================
 * mu_particles
 * ========================================================================= */

static void check_mu_particles(void)
{
    mu_particles p;
    CHECK(mu_particles_init(&p, 64));

    CHECK(mu_particles_count(&p) == 0u);
    CHECK(mu_particles_capacity(&p) == 64u);

    /* spawn a few particles */
    uint32_t a = mu_particles_spawn(&p, 0,0,0,  1,0,0,  5.0f, 1.0f, 0xFF0000FF, 1u);
    uint32_t b = mu_particles_spawn(&p, 1,2,3,  0,1,0,  5.0f, 2.0f, 0x00FF00FF, 2u);
    uint32_t c = mu_particles_spawn(&p, 4,5,6,  0,0,1,  5.0f, 3.0f, 0x0000FFFF, 3u);
    CHECK(a != MU_INVALID_INDEX && b != MU_INVALID_INDEX && c != MU_INVALID_INDEX);
    CHECK(mu_particles_count(&p) == 3u);

    /* position access */
    float pos[3];
    mu_particles_position(&p, a, pos);
    CHECK(pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f);
    mu_particles_set_position(&p, a, 7, 8, 9);
    mu_particles_position(&p, a, pos);
    CHECK(pos[0] == 7.0f && pos[1] == 8.0f && pos[2] == 9.0f);

    /* normalized life */
    CHECK(mu_particles_normalized_life(&p, a) == 0.0f); /* just spawned */
    CHECK(mu_particles_curve(&p, a, MU_EASE_LINEAR) == 0.0f);

    /* kill middle particle b; swap-with-last moves c into b's slot */
    mu_particles_kill(&p, b);
    CHECK(mu_particles_count(&p) == 2u);
    /* c's data now at index b */
    mu_particles_position(&p, b, pos);
    CHECK(pos[0] == 4.0f && pos[1] == 5.0f && pos[2] == 6.0f);

    /* update with gravity; life decays */
    float gravity[3] = { 0, -9.81f, 0 };
    uint32_t remaining = mu_particles_update(&p, 1.0f, gravity, 1.0f);
    CHECK(remaining == 2u);
    CHECK(mu_particles_normalized_life(&p, a) > 0.0f);
    CHECK(mu_particles_normalized_life(&p, a) < 1.0f);

    /* age them out: life 5, update by 5 -> both dead */
    mu_particles_update(&p, 4.0f, gravity, 1.0f);
    CHECK(mu_particles_count(&p) == 0u);

    /* deterministic jitter: same seed -> same jitter */
    mu_particles_spawn(&p, 0,0,0, 0,0,0, 1.0f, 1.0f, 0, 42u);
    float j1 = mu_particles_jitter(&p, 0);
    float j2 = mu_particles_jitter(&p, 0);
    CHECK(j1 >= 0.0f && j1 < 1.0f);
    CHECK(j1 != j2); /* stream advances */

    /* fresh pool, same seed -> same first jitter (replayable) */
    mu_particles_destroy(&p);
    CHECK(mu_particles_init(&p, 64));
    mu_particles_spawn(&p, 0,0,0, 0,0,0, 1.0f, 1.0f, 0, 42u);
    CHECK(mu_particles_jitter(&p, 0) == j1);

    /* depth sort (pool cleared so only these three are present) */
    mu_particles_clear(&p);
    uint32_t s0 = mu_particles_spawn(&p, 0,0,0, 0,0,0, 1.0f, 1.0f, 0, 1u);
    uint32_t s1 = mu_particles_spawn(&p, 10,0,0, 0,0,0, 1.0f, 1.0f, 0, 2u);
    uint32_t s2 = mu_particles_spawn(&p, 5,0,0, 0,0,0, 1.0f, 1.0f, 0, 3u);
    CHECK(s0 == 0u && s1 == 1u && s2 == 2u);
    uint32_t order[32];
    mu_particles_sort_by_depth(&p, 0,0,0, order);
    /* furthest first: index 1 (dist 100), index 2 (dist 25), index 0 (dist 0) */
    CHECK(mu_particles_count(&p) == 3u);
    CHECK(order[0] == 1u && order[1] == 2u && order[2] == 0u);

    mu_particles_destroy(&p);

    /* static variant smoke test */
    float px[8], py[8], pz[8], vx[8], vy[8], vz[8], life[8], ttl[8], size[8];
    uint32_t color[8];
    uint64_t rs[8], ri[8];
    mu_particles sp;
    CHECK(mu_particles_init_static(&sp, 8, px,py,pz, vx,vy,vz, life,ttl,size,
                                   color, rs, ri));
    CHECK(mu_particles_spawn(&sp, 0,0,0, 0,0,0, 1.0f, 1.0f, 0, 7u) == 0u);
    CHECK(mu_particles_count(&sp) == 1u);
    mu_particles_destroy(&sp);
}

/* =========================================================================
 * mu_behavior  (FSM + behavior tree + utility scoring)
 * ========================================================================= */

/* ---- FSM test helpers ---- */

typedef struct fsm_ctx
{
    int enter_calls;
    int exit_calls;
    int update_calls;
    uint32_t last_state;
} fsm_ctx;

static void fsm_enter(uint32_t state, void* c)
{
    fsm_ctx* ctx = (fsm_ctx*)c;
    ++ctx->enter_calls;
    ctx->last_state = state;
}

static void fsm_exit(uint32_t state, void* c)
{
    (void)state;
    fsm_ctx* ctx = (fsm_ctx*)c;
    ++ctx->exit_calls;
}

static void fsm_update(uint32_t state, void* c)
{
    fsm_ctx* ctx = (fsm_ctx*)c;
    ++ctx->update_calls;
    ctx->last_state = state;
}

static void check_mu_behavior_fsm(void)
{
    mu_fsm f;
    /* 3 states: idle(0) -> alert(1) -> attack(2); start in idle */
    CHECK(mu_fsm_init(&f, 0, 3, 8));

    /* transitions: idle--see_enemy-->alert ; alert--lose_target-->idle ;
       alert--in_range-->attack ; attack--out_of_range-->alert */
    CHECK(mu_fsm_add_transition(&f, 0, 1u, 1u));  /* see_enemy = 1 */
    CHECK(mu_fsm_add_transition(&f, 1, 2u, 0u));  /* lose_target = 2 */
    CHECK(mu_fsm_add_transition(&f, 1, 3u, 2u));  /* in_range = 3 */
    CHECK(mu_fsm_add_transition(&f, 2, 4u, 1u));  /* out_of_range = 4 */

    fsm_ctx ctx = {0, 0, 0, MU_INVALID_INDEX};
    mu_fsm_state_cb(&f, 0, 10u);
    mu_fsm_state_cb(&f, 1, 11u);
    mu_fsm_state_cb(&f, 2, 12u);

    /* start in idle; update runs idle's update */
    mu_fsm_update(&f, fsm_enter, fsm_exit, fsm_update, &ctx);
    CHECK(f.current == 0u);
    CHECK(ctx.update_calls == 1);
    CHECK(ctx.enter_calls == 0);
    CHECK(ctx.exit_calls == 0);

    /* feed see_enemy -> transition to alert */
    mu_fsm_feed(&f, 1u);
    mu_fsm_update(&f, fsm_enter, fsm_exit, fsm_update, &ctx);
    CHECK(f.current == 1u);
    CHECK(ctx.enter_calls == 1);   /* alert entered */
    CHECK(ctx.exit_calls == 1);    /* idle exited */

    /* feed in_range -> transition to attack */
    mu_fsm_feed(&f, 3u);
    mu_fsm_update(&f, fsm_enter, fsm_exit, fsm_update, &ctx);
    CHECK(f.current == 2u);
    CHECK(ctx.enter_calls == 2);
    CHECK(ctx.exit_calls == 2);

    /* feed out_of_range -> back to alert */
    mu_fsm_feed(&f, 4u);
    mu_fsm_update(&f, fsm_enter, fsm_exit, fsm_update, &ctx);
    CHECK(f.current == 1u);

    /* unknown trigger does nothing */
    mu_fsm_feed(&f, 99u);
    mu_fsm_update(&f, fsm_enter, fsm_exit, fsm_update, &ctx);
    CHECK(f.current == 1u);

    mu_fsm_destroy(&f);
}

/* ---- Behavior tree test helpers ---- */

static uint32_t bt_leaf_call_count = 0;

static mu_bt_status bt_leaf(uint32_t data, void* ctx, float dt,
                            mu_bt_status* out_status)
{
    (void)data; (void)ctx; (void)dt; (void)out_status;
    ++bt_leaf_call_count;
    /* data 0 -> success, 1 -> failure, 2 -> running */
    if(data == 0u) return MU_BT_SUCCESS;
    if(data == 1u) return MU_BT_FAILURE;
    return MU_BT_RUNNING;
}

static void check_mu_behavior_btree(void)
{
    mu_btree t;
    CHECK(mu_btree_init(&t, 16));

    /* root: SELECTOR
       ├─ SEQUENCE (data ignored)
       │  ├─ ACTION success (data 0)
       │  └─ ACTION failure (data 1)
       └─ ACTION success (data 0)
     */
    uint32_t root = mu_btree_add_node(&t, MU_BT_SELECTOR, 0u);
    uint32_t seq  = mu_btree_add_node(&t, MU_BT_SEQUENCE, 0u);
    uint32_t a_ok = mu_btree_add_node(&t, MU_BT_ACTION, 0u);
    uint32_t a_bad = mu_btree_add_node(&t, MU_BT_ACTION, 1u);
    uint32_t b_ok = mu_btree_add_node(&t, MU_BT_ACTION, 0u);
    CHECK(root != MU_INVALID_INDEX && seq != MU_INVALID_INDEX);
    CHECK(a_ok != MU_INVALID_INDEX && a_bad != MU_INVALID_INDEX && b_ok != MU_INVALID_INDEX);

    mu_btree_set_root(&t, root);
    mu_btree_link(&t, root, seq, MU_INVALID_INDEX);
    mu_btree_link(&t, seq, a_ok, MU_INVALID_INDEX);
    mu_btree_link(&t, seq, a_bad, a_ok);
    mu_btree_link(&t, root, b_ok, seq);

    bt_leaf_call_count = 0;
    mu_bt_status st = mu_btree_tick(&t, bt_leaf, NULL, 0.016f);
    /* Sequence: child a_ok succeeds, child a_bad fails => sequence fails.
     * Selector: sequence fails, then b_ok succeeds => selector succeeds. */
    CHECK(st == MU_BT_SUCCESS);
    /* Leaves called: a_ok, a_bad, b_ok = 3 */
    CHECK(bt_leaf_call_count == 3u);

    mu_btree_destroy(&t);

    /* Inverter test: inverter > failure => success */
    CHECK(mu_btree_init(&t, 8));
    uint32_t inv = mu_btree_add_node(&t, MU_BT_INVERTER, 0u);
    uint32_t bad = mu_btree_add_node(&t, MU_BT_ACTION, 1u); /* failure */
    mu_btree_set_root(&t, inv);
    mu_btree_link(&t, inv, bad, MU_INVALID_INDEX);
    st = mu_btree_tick(&t, bt_leaf, NULL, 0.016f);
    CHECK(st == MU_BT_SUCCESS); /* inverted failure -> success */
    mu_btree_destroy(&t);
}

/* ---- Utility scoring test helpers ---- */

static void check_mu_behavior_utility(void)
{
    mu_utility u;
    CHECK(mu_utility_init(&u, 4, 8));

    /* 3 actions: idle(10), patrol(20), attack(30) */
    uint32_t s_idle = mu_utility_add_action(&u, 10u);
    uint32_t s_patrol = mu_utility_add_action(&u, 20u);
    uint32_t s_attack = mu_utility_add_action(&u, 30u);
    CHECK(s_idle != MU_INVALID_INDEX && s_patrol != MU_INVALID_INDEX &&
          s_attack != MU_INVALID_INDEX);

    /* blackboard[0] = enemy_distance (0 = close, 10 = far)
     * blackboard[1] = health (0 = dead, 1 = full) */
    float bb[2] = { 2.0f, 1.0f }; /* close, full health */

    /* attack: high score when close (inverse distance) and healthy */
    /*   consideration: distance -> ease IN_QUAD over [0,10], but we want
     *   closer = higher. Use lo=10 hi=0 inverted? Simpler: use distance
     *   directly with an ease that rewards closeness. We'll just check
     *   relative ordering qualitatively below. */
    CHECK(mu_utility_add_consideration(&u, s_attack, 0, MU_EASE_IN_QUAD, 0.0f, 10.0f, 1.0f));
    /* patrol: score when mid-distance */
    CHECK(mu_utility_add_consideration(&u, s_patrol, 0, MU_EASE_LINEAR, 0.0f, 10.0f, 1.0f));

    uint32_t pick = mu_utility_pick(&u, bb);
    CHECK(pick != MU_INVALID_INDEX);

    /* When very far away (distance=9), patrol (linear high) should beat attack */
    bb[0] = 9.0f;
    pick = mu_utility_pick(&u, bb);
    CHECK(pick == 20u || pick == 10u); /* patrol/idle favored at range */

    /* no actions -> INVALID */
    mu_utility_clear(&u);
    CHECK(mu_utility_pick(&u, bb) == MU_INVALID_INDEX);

    mu_utility_destroy(&u);
}

/* =========================================================================
 * mu_blackboard
 * ========================================================================= */

static void check_mu_blackboard(void)
{
    mu_blackboard b;
    CHECK(mu_blackboard_init(&b, 16));

    CHECK(mu_blackboard_set_float(&b, 1u, 3.5f));
    CHECK(mu_blackboard_set_int(&b, 2u, 42));
    CHECK(mu_blackboard_set_bool(&b, 3u, true));
    CHECK(mu_blackboard_set_handle(&b, 4u, 0xABCDu));

    float f = 0.0f;
    CHECK(mu_blackboard_get_float(&b, 1u, &f));
    CHECK(f == 3.5f);
    int32_t i = 0;
    CHECK(mu_blackboard_get_int(&b, 2u, &i));
    CHECK(i == 42);
    bool bo = false;
    CHECK(mu_blackboard_get_bool(&b, 3u, &bo));
    CHECK(bo == true);
    uint32_t h = 0;
    CHECK(mu_blackboard_get_handle(&b, 4u, &h));
    CHECK(h == 0xABCDu);

    /* type mismatch returns false */
    CHECK(!mu_blackboard_get_int(&b, 1u, &i));
    /* missing key returns false */
    CHECK(!mu_blackboard_get_float(&b, 99u, &f));
    CHECK(mu_blackboard_has(&b, 1u));
    CHECK(!mu_blackboard_has(&b, 99u));

    /* overwrite */
    CHECK(mu_blackboard_set_float(&b, 1u, 7.5f));
    CHECK(mu_blackboard_get_float(&b, 1u, &f));
    CHECK(f == 7.5f);

    /* foreach */
    uint32_t keys[16];
    mu_bb_type types[16];
    uint32_t n = mu_blackboard_foreach(&b, keys, types, 16);
    CHECK(n == 4u);

    mu_blackboard_destroy(&b);
}

/* =========================================================================
 * mu_stats
 * ========================================================================= */

static void check_mu_stats(void)
{
    mu_stats s;
    CHECK(mu_stats_init(&s, 16, 0.1, 100.0));

    double vals[] = { 1.0, 2.0, 3.0, 4.0, 5.0 };
    for(uint32_t i = 0; i < 5u; ++i)
        mu_stats_push(&s, vals[i]);

    CHECK(mu_stats_count(&s) == 5u);
    CHECK(mu_stats_min(&s) == 1.0);
    CHECK(mu_stats_max(&s) == 5.0);
    CHECK(mu_stats_mean(&s) == 3.0);
    CHECK(mu_stats_variance(&s) == 2.5); /* sample variance of 1..5 */
    CHECK(mu_stats_stddev(&s) > 1.5 && mu_stats_stddev(&s) < 1.6);

    /* percentile: p50 of 1..5 -> 3 */
    double p50 = mu_stats_percentile(&s, 50.0);
    CHECK(p50 > 2.0 && p50 < 4.0);

    mu_stats_reset(&s);
    CHECK(mu_stats_count(&s) == 0u);
    CHECK(mu_stats_percentile(&s, 50.0) == 0.0);

    mu_stats_destroy(&s);
}

/* =========================================================================
 * mu_small_vec
 * ========================================================================= */

static void check_mu_small_vec(void)
{
    mu_small_vec v;
    CHECK(mu_small_vec_init(&v, sizeof(int), 4u));

    CHECK(mu_small_vec_empty(&v));
    CHECK(mu_small_vec_count(&v) == 0u);

    int a = 10, b = 20, c = 30, d = 40, e = 50;
    CHECK(mu_small_vec_push(&v, &a));
    CHECK(mu_small_vec_push(&v, &b));
    CHECK(mu_small_vec_push(&v, &c));
    CHECK(mu_small_vec_push(&v, &d));
    CHECK(mu_small_vec_count(&v) == 4u);

    /* push beyond inline cap spills to heap */
    CHECK(mu_small_vec_push(&v, &e));
    CHECK(mu_small_vec_count(&v) == 5u);

    int* p0 = (int*)mu_small_vec_at(&v, 0);
    int* p4 = (int*)mu_small_vec_at(&v, 4);
    CHECK(*p0 == 10 && *p4 == 50);

    int back = 0;
    mu_small_vec_pop(&v, &back);
    CHECK(back == 50);
    CHECK(mu_small_vec_count(&v) == 4u);

    int* front = (int*)mu_small_vec_front(&v);
    int* bak = (int*)mu_small_vec_back(&v);
    CHECK(*front == 10 && *bak == 40);

    mu_small_vec_clear(&v);
    CHECK(mu_small_vec_empty(&v));

    mu_small_vec_destroy(&v);
}

/* =========================================================================
 * mu_sync
 * ========================================================================= */

static void check_mu_sync(void)
{
    mu_spinlock lk;
    mu_spinlock_init(&lk);
    mu_spinlock_lock(&lk);
    mu_spinlock_unlock(&lk);

    mu_rwlock rw;
    mu_rwlock_init(&rw);
    mu_rwlock_rdlock(&rw);
    mu_rwlock_rdunlock(&rw);
    mu_rwlock_wrlock(&rw);
    mu_rwlock_wrunlock(&rw);

    mu_seqlock sq;
    mu_seqlock_init(&sq);
    uint32_t seq = mu_seqlock_write_begin(&sq);
    (void)seq;
    mu_seqlock_write_end(&sq);
    uint32_t r = mu_seqlock_read_begin(&sq);
    CHECK(mu_seqlock_read_retry(&sq, r) == false);
}

/* =========================================================================
 * mu_influence
 * ========================================================================= */

static void check_mu_influence(void)
{
    mu_influence inf;
    MU_MEMSET(&inf, 0, sizeof(inf));
    CHECK(mu_influence_init(&inf, 8, 8));
    mu_influence_set_decay(&inf, 0.5f, 0.01f);

    /* source at center */
    CHECK(mu_influence_add_source(&inf, 4, 4, 1.0f));
    mu_influence_flood(&inf);

    /* center has max influence */
    CHECK(mu_influence_get(&inf, 4, 4) == 1.0f);
    /* neighbors decayed */
    CHECK(mu_influence_get(&inf, 3, 4) > 0.0f);
    CHECK(mu_influence_get(&inf, 3, 4) < 1.0f);
    /* far away is ~0 */
    CHECK(mu_influence_get(&inf, 0, 0) == 0.0f);

    /* gradient at center should point toward higher (center is peak, so
       gradient is ~0 or points inward) — just check it doesn't crash */
    float gx = 0, gy = 0;
    (void)mu_influence_gradient(&inf, 3, 4, &gx, &gy);

    mu_influence_destroy(&inf);
}

/* =========================================================================
 * mu_mcts (tiny race-to-target game)
 * ========================================================================= */

typedef struct mcts_state
{
    uint32_t pos;
    uint32_t target;
} mcts_state;

static void mcts_copy(void* dst, const void* src)
{
    *(mcts_state*)dst = *(const mcts_state*)src;
}

static bool mcts_same(const void* a, const void* b)
{
    return memcmp(a, b, sizeof(mcts_state)) == 0;
}

static uint32_t mcts_legal(const void* state, uint32_t* out, uint32_t cap)
{
    const mcts_state* s = (const mcts_state*)state;
    uint32_t n = 0;
    if(s->pos < s->target && n < cap) out[n++] = 0u; /* +1 */
    if(s->pos + 1u < s->target && n < cap) out[n++] = 1u; /* +2 */
    return n;
}

static bool mcts_apply(void* state, uint32_t action)
{
    mcts_state* s = (mcts_state*)state;
    if(action == 0u)
        s->pos += 1u;
    else if(action == 1u)
        s->pos += 2u;
    if(s->pos > s->target)
        s->pos = s->target;
    return true;
}

static bool mcts_rollout(const void* state, uint32_t to_move,
                         float* out_score, void* rollout_ctx)
{
    (void)to_move; (void)rollout_ctx;
    mcts_state s = *(const mcts_state*)state;
    uint32_t steps = 0;
    while(s.pos < s.target && steps < 64u)
    {
        uint32_t acts[2];
        uint32_t n = mcts_legal(&s, acts, 2);
        if(n == 0u) break;
        uint32_t a = acts[(uint32_t)(12345u * steps + 1u) % n];
        mcts_apply(&s, a);
        ++steps;
    }
    *out_score = (s.pos >= s.target) ? 1.0f : 0.0f;
    return true;
}

static void check_mu_mcts(void)
{
    mu_mcts_game_iface iface;
    iface.copy_state = mcts_copy;
    iface.same_state = mcts_same;
    iface.legal_actions = mcts_legal;
    iface.apply = mcts_apply;
    iface.rollout = mcts_rollout;
    iface.rollout_ctx = NULL;
    iface.state_size = sizeof(mcts_state);

    mu_mcts_config cfg;
    cfg.iterations = 200u;
    cfg.ucb_c = 1.414f;
    cfg.max_depth = 16u;
    cfg.rollout_max_steps = 64u;

    mu_mcts m;
    MU_MEMSET(&m, 0, sizeof(m));
    CHECK(mu_mcts_init(&m, &iface, &cfg, 1024));

    mcts_state root;
    root.pos = 0; root.target = 10u;
    uint32_t best = mu_mcts_search(&m, &root, 0u);
    /* best action should move toward target: 0 (+1) or 1 (+2) */
    CHECK(best != MU_INVALID_INDEX);

    mu_mcts_destroy(&m);
}

/* =========================================================================
 * mu_htn
 * ========================================================================= */

typedef struct htn_world
{
    uint32_t has_key;
    uint32_t door_open;
} htn_world;

static bool htn_condition(const void* world, uint32_t arg)
{
    const htn_world* w = (const htn_world*)world;
    if(arg == 0u) return w->has_key == 1u;   /* has_key */
    if(arg == 1u) return w->door_open == 1u; /* door_open */
    return false;
}

static bool htn_execute(void* world, uint32_t task_id,
                        const uint32_t* args, uint32_t arg_count)
{
    (void)args; (void)arg_count;
    htn_world* w = (htn_world*)world;
    if(task_id == 10u) { w->has_key = 1u; return true; }    /* get_key */
    if(task_id == 20u) { w->door_open = 1u; return true; }  /* open_door */
    if(task_id == 30u) return true;                         /* enter */
    return false;
}

static void check_mu_htn(void)
{
    mu_htn_iface iface;
    iface.condition = htn_condition;
    iface.execute = htn_execute;
    iface.ctx = NULL;

    mu_htn h;
    CHECK(mu_htn_init(&h, &iface, 8, 16));

    /* method: mission(1) <- get_key(10), enter(30); no preconditions */
    mu_htn_method m;
    m.task_id = 1u;
    m.cond_count = 0u;
    m.subtask_count = 2u;
    m.subtask_ids[0] = 10u; m.subtask_arg_counts[0] = 0u;
    m.subtask_ids[1] = 30u; m.subtask_arg_counts[1] = 0u;
    for(uint32_t i = 0; i < 2u; ++i)
        for(uint32_t a = 0; a < MU_HTN_MAX_ARGS; ++a)
            m.subtask_args[i][a] = 0u;
    CHECK(mu_htn_add_method(&h, &m));

    htn_world world = { 0u, 0u };
    mu_htn_task top;
    top.type = MU_HTN_COMPOUND;
    top.id = 1u;
    top.arg_count = 0u;
    CHECK(mu_htn_plan(&h, &top, &world));
    CHECK(h.plan_count == 2u);
    CHECK(h.plan[0].id == 10u);
    CHECK(h.plan[1].id == 30u);

    /* execute: get_key sets has_key, enter succeeds */
    CHECK(mu_htn_execute_plan(&h, &world));
    CHECK(world.has_key == 1u);

    mu_htn_destroy(&h);
}

/* =========================================================================
 * mu_production
 * ========================================================================= */

typedef struct prod_world
{
    uint32_t facts;
} prod_world;

static bool prod_condition(const void* world, uint32_t fact)
{
    (void)world; (void)fact;
    /* fact-set membership is already checked by the library; accept */
    return true;
}

static void prod_action(void* world, uint32_t fact)
{
    prod_world* w = (prod_world*)world;
    w->facts |= fact;
}

static void check_mu_production(void)
{
    prod_world world;
    world.facts = 0u;

    mu_prod_iface iface;
    iface.condition = prod_condition;
    iface.action = prod_action;
    iface.ctx = &world;

    mu_production p;
    CHECK(mu_production_init(&p, &iface, 8, 64));

    /* fact bits: 1 = A, 2 = B, 4 = C */
    /* rule r1: A -> C (retract A, assert C) */
    mu_prod_rule r1;
    r1.id = 1u;
    r1.conditions[0] = 1u; r1.cond_count = 1u;
    r1.actions[0] = 4u; r1.act_count = 1u;
    r1.enabled = 1u;
    CHECK(mu_production_add_rule(&p, &r1));

    mu_production_assert(&p, 1u); /* assert A */
    CHECK(mu_production_contains(&p, 1u));

    uint32_t fired = mu_production_fire_one(&p);
    CHECK(fired != MU_INVALID_INDEX); /* r1 fired */
    CHECK(!mu_production_contains(&p, 1u)); /* A retracted */
    CHECK(mu_production_contains(&p, 4u)); /* C asserted */
    CHECK(world.facts == 4u); /* action asserted C */

    /* no more rules fire (A gone) */
    CHECK(mu_production_fire_one(&p) == MU_INVALID_INDEX);

    mu_production_destroy(&p);
}

/* =========================================================================
 * mu_petri
 * ========================================================================= */

static void check_mu_petri(void)
{
    mu_petri_net net;
    CHECK(mu_petri_init(&net, 4, 4));

    /* places: 0=input, 1=processed, 2=output, 3=buffer */
    mu_petri_set_tokens(&net, 0, 2u);

    /* transition 0: pre[0:1], post[2:1]  (consume input, produce output) */
    uint32_t t0;
    CHECK(mu_petri_add_transition(&net, &t0));
    CHECK(t0 == 0u);
    CHECK(mu_petri_add_pre_arc(&net, 0u, 0u, 1u));
    CHECK(mu_petri_add_post_arc(&net, 0u, 2u, 1u));

    /* transition 1: pre[2:1], post[1:1]  (output -> processed) */
    uint32_t t1;
    CHECK(mu_petri_add_transition(&net, &t1));
    CHECK(t1 == 1u);
    CHECK(mu_petri_add_pre_arc(&net, 1u, 2u, 1u));
    CHECK(mu_petri_add_post_arc(&net, 1u, 1u, 1u));

    /* fire t0: input(2)->1, output(0)->1 */
    CHECK(mu_petri_enabled(&net, 0u));
    CHECK(mu_petri_fire(&net, 0u));
    CHECK(mu_petri_tokens(&net, 0) == 1u);
    CHECK(mu_petri_tokens(&net, 2) == 1u);

    /* fire t1: output(1)->0, processed(0)->1 */
    CHECK(mu_petri_enabled(&net, 1u));
    CHECK(mu_petri_fire(&net, 1u));
    CHECK(mu_petri_tokens(&net, 2) == 0u);
    CHECK(mu_petri_tokens(&net, 1) == 1u);

    /* input still has 1 token -> fire_any fires t0 (a side effect!) */
    CHECK(mu_petri_fire_any(&net) == 0u);
    CHECK(mu_petri_tokens(&net, 0) == 0u);
    CHECK(mu_petri_tokens(&net, 2) == 1u);

    /* now t1 enabled -> fire_any fires t1 */
    CHECK(mu_petri_fire_any(&net) == 1u);
    CHECK(mu_petri_tokens(&net, 2) == 0u);
    CHECK(mu_petri_tokens(&net, 1) == 2u);

    /* nothing enabled now */
    CHECK(mu_petri_fire_any(&net) == MU_INVALID_INDEX);

    mu_petri_destroy(&net);
}

/* =========================================================================
 * mu_ngram
 * ========================================================================= */

static void check_mu_ngram(void)
{
    mu_ngram n;
    CHECK(mu_ngram_init(&n, 3u)); /* trigram */

    /* sequence: 1 2 3 4 5 */
    uint32_t seq[] = { 1u, 2u, 3u, 4u, 5u };
    mu_ngram_train(&n, seq, 5u);

    /* context (1,2) -> token 3 */
    uint64_t ctx = mu_ngram_pack_context((uint32_t[]){1u, 2u}, 2u);
    float p3 = mu_ngram_prob(&n, ctx, 3u, 1.0f);
    CHECK(p3 > 0.0f && p3 <= 1.0f);

    /* distribution */
    float dist[16];
    uint32_t v = mu_ngram_distribution(&n, ctx, dist, 1.0f);
    CHECK(v == n.vocab_size);

    /* sample */
    uint32_t tok = mu_ngram_sample(dist, v, 0.5f);
    CHECK(tok < v);

    /* recent context helper */
    uint32_t recent[] = { 1u, 2u, 3u, 4u, 5u };
    uint64_t rctx = mu_ngram_recent_context(&n, recent, 5u);
    /* last 2 tokens: 4,5 */
    uint64_t expect = mu_ngram_pack_context((uint32_t[]){4u, 5u}, 2u);
    CHECK(rctx == expect);

    mu_ngram_destroy(&n);
}

/* =========================================================================
 * mu_genetic
 * ========================================================================= */

/* target: genes should all equal 7 */
static float genetic_fitness(const uint32_t* genes, uint32_t length,
                             void* ctx)
{
    (void)ctx;
    uint32_t score = 0;
    for(uint32_t i = 0; i < length; ++i)
        if(genes[i] == 7u) ++score;
    return (float)score;
}

static void genetic_init_gene(uint32_t* genes, uint32_t i, void* ctx,
                              uint32_t seed)
{
    (void)ctx; (void)seed;
    genes[i] = (i * 3u + 1u) % 16u;
}

static void check_mu_genetic(void)
{
    mu_genetic g;
    CHECK(mu_genetic_init(&g, genetic_fitness, genetic_init_gene, NULL, NULL,
                          16u, 8u, 16u, 42u));

    mu_genetic_evaluate(&g);
    uint32_t best = mu_genetic_best(&g);
    CHECK(best < 16u);

    /* evolve a few generations */
    for(int gen = 0; gen < 5; ++gen)
        mu_genetic_evolve(&g, 0.3f, 0.7f);

    mu_genetic_evaluate(&g);
    best = mu_genetic_best(&g);
    CHECK(best < 16u);

    mu_genetic_destroy(&g);
}

/* =========================================================================
 * mu_possibility
 * ========================================================================= */

static void check_mu_possibility(void)
{
    mu_possibility_map m;
    CHECK(mu_possibility_map_init(&m, 6u, 6u));

    /* bit0 = STAND, bit1 = CROUCH, bit2 = COVER_LEFT, bit3 = FALLBACK */
    const uint32_t STAND = 1u << 0u;
    const uint32_t CROUCH = 1u << 1u;
    const uint32_t COVER_LEFT = 1u << 2u;
    const uint32_t FALLBACK = 1u << 3u;

    mu_possibility_map_set_cell(&m, 1u, 1u, STAND);
    mu_possibility_map_set_cell(&m, 2u, 2u, STAND | CROUCH);
    mu_possibility_map_set_cell(&m, 3u, 3u, COVER_LEFT);

    CHECK(mu_possibility_map_has(&m, 1u, 1u, STAND, MU_POSS_MATCH_ALL));
    CHECK(mu_possibility_map_has(&m, 1u, 1u, CROUCH, MU_POSS_MATCH_ALL) ==
          false);
    CHECK(mu_possibility_map_has(&m, 2u, 2u, STAND | CROUCH,
                                 MU_POSS_MATCH_ALL));
    CHECK(mu_possibility_map_has(&m, 2u, 2u, STAND, MU_POSS_MATCH_ANY));
    CHECK(mu_possibility_map_has(&m, 0u, 0u, STAND, MU_POSS_MATCH_ANY) ==
          false);

    /* region OR: paint a 2x2 block with FALLBACK */
    mu_possibility_map_region_or(&m, 0u, 0u, 1u, 1u, FALLBACK);
    CHECK(mu_possibility_map_cell(&m, 0u, 0u) & FALLBACK);
    CHECK(mu_possibility_map_cell(&m, 1u, 1u) & FALLBACK);
    CHECK((mu_possibility_map_cell(&m, 2u, 2u) & FALLBACK) == 0u);

    /* count cells matching ANY of {STAND,CROUCH} */
    uint32_t n_any = mu_possibility_map_count(&m, STAND | CROUCH,
                                              MU_POSS_MATCH_ANY);
    /* cells with stand/crouch: (1,1),(2,2) -> 2 */
    CHECK(n_any == 2u);

    /* count cells matching ALL of {STAND,CROUCH}: only (2,2) */
    uint32_t n_all = mu_possibility_map_count(&m, STAND | CROUCH,
                                              MU_POSS_MATCH_ALL);
    CHECK(n_all == 1u);

    /* find first */
    uint32_t fx = 0, fy = 0;
    CHECK(mu_possibility_map_find_first(&m, FALLBACK, MU_POSS_MATCH_ANY,
                                        &fx, &fy));
    CHECK(fx == 0u && fy == 0u);

    /* flood fill: seed cells having STAND, spread FALLBACK, no blocker */
    uint32_t filled =
        mu_possibility_map_flood(&m, STAND, MU_POSS_MATCH_ANY, FALLBACK, 0u);
    CHECK(filled > 0u);
    /* neighbors of seeded stand cells should now have FALLBACK */
    CHECK((mu_possibility_map_cell(&m, 0u, 1u) & FALLBACK) ||
          (mu_possibility_map_cell(&m, 2u, 1u) & FALLBACK) ||
          (mu_possibility_map_cell(&m, 1u, 0u) & FALLBACK) ||
          (mu_possibility_map_cell(&m, 1u, 2u) & FALLBACK));

    /* flood with a blocker: blocker bit is COVER_LEFT; seeds with STAND,
     * but (3,3) has COVER_LEFT so it won't be crossed; still other stand
     * cells fill. */
    mu_possibility_map_clear(&m);
    mu_possibility_map_set_cell(&m, 1u, 1u, STAND);
    mu_possibility_map_set_cell(&m, 3u, 3u, STAND | COVER_LEFT);
    filled = mu_possibility_map_flood(&m, STAND, MU_POSS_MATCH_ANY, FALLBACK,
                                      COVER_LEFT);
    CHECK(filled > 0u);
    /* (3,3) itself is a seed with blocker -> not crossed into, but it IS a
     * seed so it gets FALLBACK applied? No: blocker check applies to
     * neighbors, not the seed. Seeds are visited regardless. */
    CHECK((mu_possibility_map_cell(&m, 3u, 3u) & FALLBACK) != 0u);

    mu_possibility_map_destroy(&m);
}

/* =========================================================================
 * mu_constraint
 * ========================================================================= */

/* x != y */
static bool csp_neq(uint32_t a, uint32_t b, void* ctx)
{
    (void)ctx;
    return a != b;
}

/* x < y */
static bool csp_lt(uint32_t a, uint32_t b, void* ctx)
{
    (void)ctx;
    return a < b;
}

static void check_mu_constraint(void)
{
    mu_csp c;
    CHECK(mu_csp_init(&c, 8u));

    uint32_t x = 0u, y = 0u, z = 0u;
    uint32_t dx[] = {1u, 2u, 3u}; /* x */
    uint32_t dy[] = {1u, 2u};     /* y */
    uint32_t dz[] = {2u, 3u};     /* z */
    CHECK(mu_csp_add_var(&c, dx, 3u, &x));
    CHECK(mu_csp_add_var(&c, dy, 2u, &y));
    CHECK(mu_csp_add_var(&c, dz, 2u, &z));

    /* x != y, x < z, y != z  -> solution x=1,y=2,z=3 */
    CHECK(mu_csp_add_constraint(&c, x, y, csp_neq, NULL));
    CHECK(mu_csp_add_constraint(&c, x, z, csp_lt, NULL));
    CHECK(mu_csp_add_constraint(&c, y, z, csp_neq, NULL));

    uint32_t assign[8];
    for(uint32_t i = 0; i < 8u; ++i)
        assign[i] = 0u;
    CHECK(mu_csp_solve(&c, assign, UINT32_MAX));

    /* verify solution satisfies all constraints */
    CHECK(assign[x] != assign[y]);
    CHECK(assign[x] < assign[z]);
    CHECK(assign[y] != assign[z]);
    CHECK(assign[x] == 1u || assign[x] == 2u || assign[x] == 3u);
    CHECK(assign[y] == 1u || assign[y] == 2u);
    CHECK(assign[z] == 2u || assign[z] == 3u);

    mu_csp_destroy(&c);
}

/* =========================================================================
 * mu_stochastic_grammar
 * ========================================================================= */

static void check_mu_stochastic_grammar(void)
{
    mu_grammar g;
    CHECK(mu_grammar_init(&g));
    mu_grammar_set_start(&g, 1u); /* S: sentence */

    /* Non-terminals: 1=S, 2=NP, 3=VP, 4=Det, 5=N, 6=V, 7=Pro
     * Terminals: 100+=words (never added as symbols -> always terminals) */

    uint32_t rhs_s[] = {2u, 3u};
    CHECK(mu_grammar_add_production(&g, 1u, rhs_s, 2u, 1.0f)); /* S -> NP VP */
    uint32_t rhs_np1[] = {4u, 5u};
    CHECK(mu_grammar_add_production(&g, 2u, rhs_np1, 2u, 0.5f)); /* NP -> Det N */
    uint32_t rhs_np2[] = {7u};
    CHECK(mu_grammar_add_production(&g, 2u, rhs_np2, 1u, 0.5f)); /* NP -> Pro */
    uint32_t rhs_vp1[] = {6u, 2u};
    CHECK(mu_grammar_add_production(&g, 3u, rhs_vp1, 2u, 0.5f)); /* VP -> V NP */
    uint32_t rhs_vp2[] = {6u};
    CHECK(mu_grammar_add_production(&g, 3u, rhs_vp2, 1u, 0.5f)); /* VP -> V */
    uint32_t rhs_det[] = {100u};
    CHECK(mu_grammar_add_production(&g, 4u, rhs_det, 1u, 1.0f)); /* Det -> the */
    uint32_t rhs_n1[] = {101u};
    CHECK(mu_grammar_add_production(&g, 5u, rhs_n1, 1u, 0.5f)); /* N -> dog */
    uint32_t rhs_n2[] = {102u};
    CHECK(mu_grammar_add_production(&g, 5u, rhs_n2, 1u, 0.5f)); /* N -> cat */
    uint32_t rhs_v1[] = {103u};
    CHECK(mu_grammar_add_production(&g, 6u, rhs_v1, 1u, 0.5f)); /* V -> runs */
    uint32_t rhs_v2[] = {104u};
    CHECK(mu_grammar_add_production(&g, 6u, rhs_v2, 1u, 0.5f)); /* V -> sleeps */
    uint32_t rhs_pr1[] = {105u};
    CHECK(mu_grammar_add_production(&g, 7u, rhs_pr1, 1u, 0.5f)); /* Pro -> he */
    uint32_t rhs_pr2[] = {106u};
    CHECK(mu_grammar_add_production(&g, 7u, rhs_pr2, 1u, 0.5f)); /* Pro -> she */

    mu_grammar_rng rng;
    mu_grammar_rng_seed(&rng, 12345u);

    uint32_t out[64];
    uint32_t n = mu_grammar_derive(&g, &rng, 16u, out, 64);
    CHECK(n > 0u && n <= 64u);
    for(uint32_t i = 0; i < n; ++i)
        CHECK(out[i] >= 100u); /* all terminals */

    /* depth bound: max_depth 0 => root's children exceed depth => nothing */
    uint32_t n2 = mu_grammar_derive(&g, &rng, 0u, out, 64);
    CHECK(n2 == 0u);

    mu_grammar_destroy(&g);
}

/* =========================================================================
 * mu_lsystem
 * ========================================================================= */

static void check_mu_lsystem(void)
{
    mu_lsystem l;
    CHECK(mu_lsystem_init(&l, 4096u));

    /* Koch curve: F -> F+F--F+F, angle 60 deg */
    CHECK(mu_lsystem_set_axiom(&l, "F"));
    CHECK(mu_lsystem_add_rule(&l, 'F', "F+F--F+F"));

    uint32_t iters = mu_lsystem_iterate_n(&l, 1u);
    CHECK(iters == 1u);
    /* "F+F--F+F" = 8 symbols */
    CHECK(mu_lsystem_length(&l) == 8u);
    CHECK(mu_lsystem_string(&l)[0] == 'F');

    /* interpret to segments */
    mu_lsystem_segment segs[64];
    uint32_t n = mu_lsystem_interpret(&l, 1.0f, 3.14159265f / 3.0f, segs, 64);
    CHECK(n > 0u && n <= 64u);
    /* every drawn segment should have positive length */
    for(uint32_t i = 0; i < n; ++i)
    {
        float dx = segs[i].x1 - segs[i].x0;
        float dy = segs[i].y1 - segs[i].y0;
        CHECK(dx * dx + dy * dy > 0.0f);
    }

    /* iterate more: length grows (each F -> 5 symbols, others kept) */
    uint32_t len_before = mu_lsystem_length(&l);
    CHECK(mu_lsystem_iterate(&l));
    CHECK(mu_lsystem_length(&l) > len_before);

    /* reset returns to axiom */
    mu_lsystem_reset(&l);
    CHECK(mu_lsystem_length(&l) == 1u);
    CHECK(mu_lsystem_string(&l)[0] == 'F');

    /* override rule */
    CHECK(mu_lsystem_add_rule(&l, 'F', "F"));
    CHECK(mu_lsystem_iterate(&l));
    CHECK(mu_lsystem_length(&l) == 1u);

    /* Dragon curve: FX -> X+YF+, -FX-Y */
    mu_lsystem_destroy(&l);
    CHECK(mu_lsystem_init(&l, 8192u));
    CHECK(mu_lsystem_set_axiom(&l, "FX"));
    CHECK(mu_lsystem_add_rule(&l, 'X', "X+YF+"));
    CHECK(mu_lsystem_add_rule(&l, 'Y', "-FX-Y"));
    CHECK(mu_lsystem_iterate_n(&l, 2u));
    CHECK(mu_lsystem_length(&l) > 2u);
    n = mu_lsystem_interpret(&l, 1.0f, 3.14159265f / 2.0f, segs, 64);
    CHECK(n > 0u && n <= 64u);

    mu_lsystem_destroy(&l);
}

/* =========================================================================
 * mu_reaction_diffusion
 * ========================================================================= */

static void check_mu_reaction_diffusion(void)
{
    mu_rd_grid g;
    CHECK(mu_rd_init(&g, 32u, 32u));
    CHECK(mu_rd_width(&g) == 32u);
    CHECK(mu_rd_height(&g) == 32u);

    mu_rd_seed(&g);

    /* after seeding: center region has V > 0, edges have V == 0 */
    CHECK(mu_rd_get_v(&g, 0u, 0u) == 0.0f);
    CHECK(mu_rd_get_v(&g, 16u, 16u) > 0.0f);
    CHECK(mu_rd_get_u(&g, 16u, 16u) > 0.0f);

    /* all values in [0,1] */
    for(uint32_t y = 0; y < 32u; ++y)
    {
        for(uint32_t x = 0; x < 32u; ++x)
        {
            float u = mu_rd_get_u(&g, x, y);
            float v = mu_rd_get_v(&g, x, y);
            CHECK(u >= 0.0f && u <= 1.0f);
            CHECK(v >= 0.0f && v <= 1.0f);
        }
    }

    /* step a few times; patterns should emerge (values change) */
    float v_center_before = mu_rd_get_v(&g, 16u, 16u);
    mu_rd_run(&g, 10u);
    float v_center_after = mu_rd_get_v(&g, 16u, 16u);
    CHECK(v_center_after >= 0.0f && v_center_after <= 1.0f);
    CHECK(v_center_before != v_center_after);

    /* different parameters -> different behavior; classic spot params */
    mu_rd_set_params(&g, 0.16f, 0.08f, 0.060f, 0.062f, 1.0f);
    mu_rd_seed(&g);
    mu_rd_run(&g, 50u);

    /* some cell in the middle should have moved away from seed values */
    int any_changed = 0;
    for(uint32_t y = 10u; y < 22u && !any_changed; ++y)
    {
        for(uint32_t x = 10u; x < 22u && !any_changed; ++x)
        {
            if(mu_rd_get_u(&g, x, y) != 1.0f || mu_rd_get_v(&g, x, y) != 0.0f)
                any_changed = 1;
        }
    }
    CHECK(any_changed);

    mu_rd_destroy(&g);
}

int main(void)
{
    printf("=== mu macros ===\n");

    MU_STATIC_ASSERT(sizeof(uint64_t) == 8, "u64_must_be_8_bytes");

    int arr[] = {1, 2, 3, 4, 5};
    CHECK(MU_ARRAY_COUNT(arr) == 5);
    CHECK(sum_array(arr, MU_ARRAY_COUNT(arr)) == 15);
    CHECK(slow_identity(42) == 42);

    CHECK(MU_MIN(10, 20) == 10);
    CHECK(MU_MAX(10, 20) == 20);
    CHECK(MU_CLAMP(50, 0, 10) == 10);

    int a = 5, b = 9;
    MU_SWAP(int, a, b);
    CHECK(a == 9 && b == 5);

    CHECK(MU_IS_POW2(64));
    CHECK(!MU_IS_POW2(70));
    CHECK(!MU_IS_POW2(0));

    CHECK(MU_KB(1) == 1024ull);
    CHECK(MU_MB(1) == 1024ull * 1024ull);
    CHECK(MU_GB(1) == 1024ull * 1024ull * 1024ull);

    CHECK(MU_ALIGN_UP(5, 4) == 8);
    CHECK(MU_ALIGN_DOWN(5, 4) == 4);

    CHECK(((uintptr_t)aligned_buffer % 64) == 0);
    CHECK(MU_TOSTRING(Hello) != NULL);
    int MU_CONCAT(test_, 123) = 123;
    CHECK(test_123 == 123);

    Wrapper w;
    w.header        = 11;
    w.example.id    = 99;
    w.example.value = 3.14;
    CHECK(MU_OFFSET_OF(Wrapper, example) == offsetof(Wrapper, example));
    Wrapper* recovered = MU_CONTAINER_OF(&w.example, Wrapper, example);
    CHECK(recovered->header == 11);

    uint64_t mask = 0x000000000000000Bull; /* popcount 3 */
    CHECK(mu_trailing_zeroes_u64(mask) == 0);
    CHECK(mu_leading_zeroes_u64(mask) == 60);
    CHECK(mu_popcount_u64(mask) == 3);
    CHECK(mu_trailing_zeroes_u64(0) == 64);
    CHECK(mu_leading_zeroes_u64(0) == 64);

    MU_ASSERT(sum_array(arr, MU_ARRAY_COUNT(arr)) == 15);

    printf("=== containers ===\n");

    /* array (stretchy buffer) */
    {
        float* v = NULL;
        array_push(v, 1.0f);
        array_push(v, 2.0f);
        CHECK(array_size(v) == 2);
        CHECK(array_back(v) == 2.0f);
        array_free(v);
        CHECK(v == NULL);
    }

    /* bitset */
    {
        mu_bitset* bs = mu_bitset_create_with_capacity(130);
        mu_bitset_set(bs, 1);
        mu_bitset_set(bs, 64);
        mu_bitset_set(bs, 129);
        CHECK(mu_bitset_count(bs) == 3);
        CHECK(mu_bitset_test(bs, 64));

        size_t it = 0, seen = 0;
        while (mu_bitset_next_set_bit(bs, &it)) { ++seen; ++it; }
        CHECK(seen == 3);

        mu_bitset* other = mu_bitset_create_with_capacity(130);
        mu_bitset_set(other, 64);
        CHECK(mu_bitset_intersection_count(bs, other) == 1);
        CHECK(mu_bitset_union_count(bs, other) == 3);

        mu_bitset_free(other);
        mu_bitset_free(bs);
    }

    /* sparse set */
    {
        mu_sparse_set s;
        mu_sparse_set_init(&s, 256);
        mu_sparse_set_insert(&s, 7);
        mu_sparse_set_insert(&s, 200);
        CHECK(mu_sparse_set_contains(&s, 200));
        CHECK(s.count == 2);
        mu_sparse_set_remove(&s, 7);
        CHECK(!mu_sparse_set_contains(&s, 7));
        CHECK(s.count == 1);
        mu_sparse_set_destroy(&s);
    }

    /* hash table (owned) */
    {
        mu_hash32_t h;
        mu_hash32_init(&h, 64);
        mu_hash32_set(&h, 1234u, 42u);
        uint32_t out = 0;
        CHECK(mu_hash32_get(&h, 1234u, &out));
        CHECK(out == 42u);
        CHECK(!mu_hash32_get(&h, 999u, &out));
        mu_hash32_destroy(&h);
    }

    /* perfect hash */
    {
        enum { PHF_N = 256 };
        uint64_t keys[PHF_N];
        uint64_t values[PHF_N];
        mu_phf h;
        uint32_t i;

        for (i = 0; i < PHF_N; ++i)
        {
            keys[i] = (uint64_t)i * UINT64_C(0x9e3779b97f4a7c15) + 17u;
            values[i] = (uint64_t)i + 1000u;
        }

        mu_phf_init(&h);
        CHECK(mu_phf_build(&h, keys, values, PHF_N, 0));
        CHECK(mu_phf_count(&h) == PHF_N);
        CHECK(mu_phf_bucket_count(&h) > 0);
        for (i = 0; i < PHF_N; ++i)
        {
            uint64_t out = 0;
            CHECK(mu_phf_lookup(&h, keys[i], &out));
            CHECK(out == values[i]);
        }
        CHECK(!mu_phf_contains(&h, UINT64_C(0xdeadbeefdeadbeef)));
        CHECK(mu_phf_value_or(&h, keys[7], 0) == values[7]);
        mu_phf_destroy(&h);

        CHECK(mu_phf_build(&h, keys, NULL, PHF_N, 1));
        {
            uint64_t out = 0;
            CHECK(mu_phf_lookup(&h, keys[9], &out));
            CHECK(out == 9u);
        }
        mu_phf_destroy(&h);

        keys[1] = keys[0];
        CHECK(!mu_phf_build(&h, keys, NULL, 2, 0));
        mu_phf_destroy(&h);

        CHECK(mu_phf_build(&h, NULL, NULL, 0, 0));
        CHECK(mu_phf_empty(&h));
        CHECK(!mu_phf_lookup(&h, 1u, NULL));
        mu_phf_destroy(&h);
    }

    /* bulk storage + generational handles */
    {
        mu_bulk_storage store;
        mu_bulk_storage_init(&store, sizeof(Example), 8);
        uint32_t id = mu_bulk_storage_alloc(&store);
        Example* e  = (Example*)mu_bulk_storage_ptr(&store, id);
        e->id = 5;
        mu_weak_handle h = mu_bulk_storage_make_handle(&store, id);
        CHECK(mu_bulk_storage_validate_handle(&store, h));
        mu_bulk_storage_free(&store, id);
        CHECK(mu_bulk_storage_resolve_handle(&store, h) == NULL);
        mu_bulk_storage_deinit(&store);
    }

    /* id pool */
    {
        mu_id_pool pool;
        mu_id_pool_init(&pool, 16);
        uint32_t first = 0;
        CHECK(mu_id_pool_create_range_id(&pool, &first, 4));
        CHECK(first == 0);
        CHECK(mu_id_pool_destroy_range_id(&pool, first, 4));
        mu_id_pool_deinit(&pool);
    }

    /* multi index */
    {
        mu_multi_index idx;
        mu_multi_index_init(&idx, 16, 16);
        mu_multi_index_add(&idx, 3u, 100u);
        mu_multi_index_add(&idx, 3u, 101u);
        uint32_t first = mu_multi_index_first(&idx, 3u);
        uint32_t n = 0;
        for (n = first; mu_multi_index_node_valid(&idx, n);
             n = mu_multi_index_next(&idx, first, n))
            ;
        CHECK(mu_multi_index_count_key(&idx, 3u) == 2);
        mu_multi_index_deinit(&idx);
    }

    /* length index */
    {
        MuLengthIndex li;
        mu_length_index_init(&li);
        mu_length_index_rebuild(&li);
        mu_length_index_destroy(&li);
    }

    /* ------------------------------------------------------------------ */
    printf("=== hierarchical bitset ===\n");
    /* ------------------------------------------------------------------ */
    {
        enum { HBITS = 65536u };
        static uint8_t hb_ref[HBITS];
        mu_hier_bitset bs;
        CHECK(mu_hier_bitset_init(&bs, (uint32_t)HBITS));
        CHECK(mu_hier_bitset_empty(&bs));
        CHECK(mu_hier_bitset_find_first(&bs) == MU_INVALID_INDEX);
        CHECK(mu_hier_bitset_any(&bs) == false);

        /* deterministic pseudo-random pattern (duplicates collapse) */
        uint32_t seed = 12345u;
        for (uint32_t i = 0; i < 500u; ++i)
        {
            seed       = seed * 1664525u + 1013904223u;
            uint32_t b = seed % (uint32_t)HBITS;
            hb_ref[b]  = 1;
            mu_hier_bitset_set(&bs, b);
        }

        uint32_t expected = 0;
        for (uint32_t i = 0; i < (uint32_t)HBITS; ++i)
            if (hb_ref[i])
                ++expected;
        CHECK(expected > 0);
        CHECK(mu_hier_bitset_any(&bs));
        CHECK(mu_hier_bitset_capacity(&bs) == (uint32_t)HBITS);
        CHECK(mu_hier_bitset_popcount(&bs) == expected);

        /* point queries match the reference */
        for (uint32_t i = 0; i < (uint32_t)HBITS; i += 97u)
            CHECK(mu_hier_bitset_test(&bs, i) == (hb_ref[i] != 0));

        /* in-bounds and out-of-bounds */
        CHECK(mu_hier_bitset_test(&bs, (uint32_t)HBITS) == false);
        mu_hier_bitset_set(&bs, (uint32_t)HBITS); /* ignored */

        /* full ascending iteration covers exactly the reference */
        uint32_t seen   = 0;
        uint32_t prev   = MU_INVALID_INDEX;
        bool     ordered = true;
        for (uint32_t i = mu_hier_bitset_find_first(&bs); i != MU_INVALID_INDEX;
             i           = mu_hier_bitset_find_next(&bs, i))
        {
            if (i >= (uint32_t)HBITS || !hb_ref[i])
                CHECK(false);
            if (prev != MU_INVALID_INDEX && i <= prev)
                ordered = false;
            prev = i;
            ++seen;
            if (seen > (uint32_t)HBITS)
            {
                CHECK(false);
                break;
            }
        }
        CHECK(ordered);
        CHECK(seen == expected);

        /* first / next anchors */
        uint32_t first_ref = MU_INVALID_INDEX;
        for (uint32_t i = 0; i < (uint32_t)HBITS; ++i)
            if (hb_ref[i])
            {
                first_ref = i;
                break;
            }
        CHECK(mu_hier_bitset_find_first(&bs) == first_ref);

        /* clearing every bit leaves an empty set (exercises the summary chain) */
        for (uint32_t i = 0; i < (uint32_t)HBITS; ++i)
            if (hb_ref[i])
            {
                mu_hier_bitset_reset(&bs, i);
                hb_ref[i] = 0;
            }
        CHECK(mu_hier_bitset_empty(&bs));
        CHECK(mu_hier_bitset_popcount(&bs) == 0);
        CHECK(mu_hier_bitset_find_first(&bs) == MU_INVALID_INDEX);

        mu_hier_bitset_clear(&bs);
        mu_hier_bitset_destroy(&bs);

        /* tiny domain => level_count == 1 (no summary level at all) */
        {
            mu_hier_bitset tiny;
            CHECK(mu_hier_bitset_init(&tiny, 100u));
            mu_hier_bitset_set(&tiny, 0u);
            mu_hier_bitset_set(&tiny, 63u);
            mu_hier_bitset_set(&tiny, 64u);
            mu_hier_bitset_set(&tiny, 99u);
            CHECK(mu_hier_bitset_popcount(&tiny) == 4);
            CHECK(mu_hier_bitset_find_first(&tiny) == 0u);
            CHECK(mu_hier_bitset_find_next(&tiny, 0u) == 63u);
            CHECK(mu_hier_bitset_find_next(&tiny, 63u) == 64u);
            CHECK(mu_hier_bitset_find_next(&tiny, 64u) == 99u);
            CHECK(mu_hier_bitset_find_next(&tiny, 99u) == MU_INVALID_INDEX);
            mu_hier_bitset_destroy(&tiny);
        }

        /* static variant: must not free caller memory */
        {
            uint32_t need = mu_hier_bitset_words_needed((uint32_t)HBITS);
            uint64_t* sbuf = (uint64_t*)MU_MALLOC((size_t)need * sizeof(uint64_t));
            CHECK(sbuf != NULL);
            if (sbuf)
            {
                mu_hier_bitset sbs;
                CHECK(mu_hier_bitset_init_static(&sbs, sbuf, need, (uint32_t)HBITS));
                mu_hier_bitset_set(&sbs, 1234u);
                mu_hier_bitset_set(&sbs, 65535u);
                CHECK(mu_hier_bitset_test(&sbs, 1234u));
                CHECK(mu_hier_bitset_popcount(&sbs) == 2);
                CHECK(mu_hier_bitset_find_first(&sbs) == 1234u);
                CHECK(mu_hier_bitset_find_next(&sbs, 1234u) == 65535u);
                CHECK(mu_hier_bitset_find_next(&sbs, 65535u) == MU_INVALID_INDEX);
                mu_hier_bitset_destroy(&sbs);
                MU_FREE(sbuf);
            }
        }
    }

    /* ------------------------------------------------------------------ */
    printf("=== chunked bitset ===\n");
    /* ------------------------------------------------------------------ */
    {
        enum { CBITS = 200000u };
        static uint8_t cb_ref[CBITS];
        mu_chunked_bitset bs;
        mu_chunked_bitset_init(&bs);
        CHECK(mu_chunked_bitset_empty(&bs));
        CHECK(mu_chunked_bitset_find_first(&bs, 0) == MU_INVALID_INDEX);

        uint32_t seed = 777u;
        for (uint32_t i = 0; i < 3000u; ++i)
        {
            seed       = seed * 1664525u + 1013904223u;
            uint32_t b = seed % (uint32_t)CBITS;
            cb_ref[b]  = 1;
            mu_chunked_bitset_set(&bs, b);
        }

        uint32_t expected = 0;
        for (uint32_t i = 0; i < (uint32_t)CBITS; ++i)
            if (cb_ref[i])
                ++expected;
        CHECK(expected > 0);
        CHECK(mu_chunked_bitset_any(&bs));
        CHECK(mu_chunked_bitset_popcount(&bs) == expected);
        CHECK(mu_chunked_bitset_capacity(&bs) > 0u);
        /* 512 bits/chunk over a 200000-bit domain => real growth happened */
        CHECK(mu_chunked_bitset_chunk_count(&bs) >= (uint32_t)(CBITS / 512u));

        for (uint32_t i = 0; i < (uint32_t)CBITS; i += 101u)
            CHECK(mu_chunked_bitset_test(&bs, i) == (cb_ref[i] != 0));
        CHECK(mu_chunked_bitset_test(&bs, (uint32_t)CBITS) == false);

        uint32_t seen    = 0;
        uint32_t prev    = MU_INVALID_INDEX;
        bool     ordered = true;
        for (uint32_t i = mu_chunked_bitset_find_first(&bs, 0); i != MU_INVALID_INDEX;
             i           = mu_chunked_bitset_find_next(&bs, i))
        {
            if (i >= (uint32_t)CBITS || !cb_ref[i])
                CHECK(false);
            if (prev != MU_INVALID_INDEX && i <= prev)
                ordered = false;
            prev = i;
            ++seen;
            if (seen > (uint32_t)CBITS)
            {
                CHECK(false);
                break;
            }
        }
        CHECK(ordered);
        CHECK(seen == expected);

        /* find_first from a mid-range start */
        uint32_t mid_ref = MU_INVALID_INDEX;
        for (uint32_t i = CBITS / 2u; i < (uint32_t)CBITS; ++i)
            if (cb_ref[i])
            {
                mid_ref = i;
                break;
            }
        CHECK(mu_chunked_bitset_find_first(&bs, CBITS / 2u) == mid_ref);

        /* reset everything */
        for (uint32_t i = 0; i < (uint32_t)CBITS; ++i)
            if (cb_ref[i])
            {
                mu_chunked_bitset_reset(&bs, i);
                cb_ref[i] = 0;
            }
        CHECK(mu_chunked_bitset_empty(&bs));
        CHECK(mu_chunked_bitset_popcount(&bs) == 0);
        CHECK(mu_chunked_bitset_find_first(&bs, 0) == MU_INVALID_INDEX);

        /* set again then clear wholesale */
        mu_chunked_bitset_set(&bs, 0u);
        mu_chunked_bitset_set(&bs, (uint32_t)CBITS - 1u);
        CHECK(mu_chunked_bitset_popcount(&bs) == 2);
        mu_chunked_bitset_clear(&bs);
        CHECK(mu_chunked_bitset_popcount(&bs) == 0);
        CHECK(mu_chunked_bitset_capacity(&bs) == 0u);
        mu_chunked_bitset_destroy(&bs);

        /* static variant: bounded, and must not free caller memory */
        {
            const uint32_t bits = 4096u;
            uint32_t nw    = mu_chunked_bitset_words_needed(bits);
            uint32_t sw    = mu_chunked_bitset_summary_words_needed(bits);
            uint64_t* pw   = (uint64_t*)MU_MALLOC((size_t)nw * sizeof(uint64_t));
            uint64_t* sm   = (uint64_t*)MU_MALLOC((size_t)sw * sizeof(uint64_t));
            CHECK(pw != NULL && sm != NULL);
            if (pw && sm)
            {
                mu_chunked_bitset sbs;
                uint32_t chunks = (bits + 511u) / 512u;
                CHECK(mu_chunked_bitset_init_static(&sbs, pw, sm, chunks, bits));
                mu_chunked_bitset_set(&sbs, 10u);
                mu_chunked_bitset_set(&sbs, 4095u);
                CHECK(mu_chunked_bitset_test(&sbs, 10u));
                CHECK(mu_chunked_bitset_test(&sbs, 4095u));
                CHECK(mu_chunked_bitset_popcount(&sbs) == 2);
                CHECK(mu_chunked_bitset_find_first(&sbs, 0) == 10u);
                CHECK(mu_chunked_bitset_find_first(&sbs, 4096u) == MU_INVALID_INDEX);
                mu_chunked_bitset_destroy(&sbs);
                MU_FREE(pw);
                MU_FREE(sm);
            }
        }
    }

    /* ------------------------------------------------------------------ */
    printf("=== bloom filter ===\n");
    /* ------------------------------------------------------------------ */
    {
        const uint64_t N = 1000ull;

        /* classic */
        {
            mu_bloom bf;
            CHECK(mu_bloom_init_per_item(&bf, N, 10u));
            CHECK(mu_bloom_bit_count(&bf) >= N * 10ull);
            CHECK(mu_bloom_hash_count_for(10u) == 7u);
            CHECK(mu_bloom_hash_count_for(1u) == 1u);

            for (uint64_t i = 0; i < N; ++i)
                mu_bloom_add(&bf, i + 0x9E3779B97F4A7C15ull);

            /* no false negatives, ever */
            for (uint64_t i = 0; i < N; ++i)
                if (!mu_bloom_test(&bf, i + 0x9E3779B97F4A7C15ull))
                    CHECK(false);

            /* false positives must stay rare */
            uint32_t fp = 0;
            for (uint64_t i = N; i < N * 2ull; ++i)
                if (mu_bloom_test(&bf, i + 0x9E3779B97F4A7C15ull))
                    ++fp;
            CHECK(fp < 150u);

            double fill = mu_bloom_fill_ratio(&bf);
            CHECK(fill > 0.0 && fill < 1.0);

            mu_bloom_clear(&bf);
            CHECK(mu_bloom_popcount(&bf) == 0);
            CHECK(mu_bloom_fill_ratio(&bf) == 0.0);
            mu_bloom_destroy(&bf);
        }

        /* blocked: same guarantees, one word touched per probe */
        {
            mu_bloom_blocked bf;
            CHECK(mu_bloom_blocked_init_per_item(&bf, N, 10u));

            for (uint64_t i = 0; i < N; ++i)
                mu_bloom_blocked_add(&bf, i + 0x9E3779B97F4A7C15ull);

            for (uint64_t i = 0; i < N; ++i)
                if (!mu_bloom_blocked_test(&bf, i + 0x9E3779B97F4A7C15ull))
                    CHECK(false);

            uint32_t fp = 0;
            for (uint64_t i = N; i < N * 2ull; ++i)
                if (mu_bloom_blocked_test(&bf, i + 0x9E3779B97F4A7C15ull))
                    ++fp;
            CHECK(fp < 150u);

            double fill = mu_bloom_blocked_fill_ratio(&bf);
            CHECK(fill > 0.0 && fill < 1.0);

            mu_bloom_blocked_clear(&bf);
            CHECK(mu_bloom_blocked_popcount(&bf) == 0);
            mu_bloom_blocked_destroy(&bf);
        }

        /* static variants must not free caller memory */
        {
            static uint64_t bloom_words[64];
            static uint64_t bloom_cells[64];
            mu_bloom  a;
            mu_bloom_blocked b;
            CHECK(mu_bloom_init_static(&a, bloom_words, 64u, 7u));
            mu_bloom_add(&a, 42ull);
            CHECK(mu_bloom_test(&a, 42ull));
            mu_bloom_destroy(&a);

            CHECK(mu_bloom_blocked_init_static(&b, bloom_cells, 64u, 7u));
            mu_bloom_blocked_add(&b, 42ull);
            CHECK(mu_bloom_blocked_test(&b, 42ull));
            mu_bloom_blocked_destroy(&b);
        }
    }

    /* ------------------------------------------------------------------ */
    printf("=== worklist ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_worklist w;
        uint32_t v = 0;

        CHECK(mu_worklist_init(&w, 8));
        CHECK(mu_worklist_empty(&w));
        CHECK(!mu_worklist_push_tail(&w, 8));
        CHECK(mu_worklist_push_tail(&w, 3));
        CHECK(mu_worklist_push_tail(&w, 5));
        CHECK(mu_worklist_push_head(&w, 1));
        CHECK(!mu_worklist_push_head(&w, 5));
        CHECK(mu_worklist_contains(&w, 1));
        CHECK(mu_worklist_contains(&w, 3));
        CHECK(mu_worklist_contains(&w, 5));
        CHECK(!mu_worklist_contains(&w, 2));
        CHECK(mu_worklist_count(&w) == 3);
        CHECK(mu_worklist_peek_head(&w, 99) == 1);
        CHECK(mu_worklist_peek_tail(&w, 99) == 5);
        CHECK(mu_worklist_at(&w, 1) == 3);

        CHECK(mu_worklist_pop_head(&w, &v) && v == 1);
        CHECK(mu_worklist_pop_tail(&w, &v) && v == 5);
        CHECK(mu_worklist_count(&w) == 1);
        CHECK(mu_worklist_at(&w, 0) == 3);

        mu_worklist_clear(&w);
        CHECK(mu_worklist_empty(&w));
        CHECK(!mu_worklist_contains(&w, 3));
        mu_worklist_destroy(&w);

        CHECK(mu_worklist_init(&w, 2));
        CHECK(mu_worklist_push_tail(&w, 0));
        CHECK(mu_worklist_push_tail(&w, 1));
        CHECK(mu_worklist_full(&w));
        CHECK(!mu_worklist_push_tail(&w, 1));
        CHECK(!mu_worklist_pop_head(NULL, &v));
        mu_worklist_destroy(&w);

        check_worklist_static();
        check_spatial_array();
    }

    /* ------------------------------------------------------------------ */
    printf("=== sparse array ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_sparse_array sa;
        uint32_t* p0;
        uint32_t* p1000;

        CHECK(mu_sparse_array_init(&sa, sizeof(uint32_t), 4));
        CHECK(mu_sparse_array_peek(&sa, 0) == NULL);

        p0 = (uint32_t*)mu_sparse_array_get(&sa, 0);
        CHECK(p0 != NULL && *p0 == 0);
        if (p0)
            *p0 = 111;

        CHECK(*(uint32_t*)mu_sparse_array_get(&sa, 3) == 0);
        CHECK(*(uint32_t*)mu_sparse_array_get(&sa, 4) == 0);
        p1000 = (uint32_t*)mu_sparse_array_get(&sa, 1000);
        CHECK(p1000 != NULL && *p1000 == 0);
        if (p1000)
            *p1000 = 1000;

        CHECK(mu_sparse_array_get(&sa, 0) == p0);
        CHECK(*(uint32_t*)mu_sparse_array_get(&sa, 0) == 111);
        CHECK(*(uint32_t*)mu_sparse_array_get(&sa, 1000) == 1000);
        CHECK(mu_sparse_array_peek(&sa, 1000) == p1000);
        CHECK(mu_sparse_array_peek(&sa, 8) == NULL);

        mu_sparse_array_clear(&sa);
        CHECK(mu_sparse_array_peek(&sa, 0) == NULL);
        CHECK(*(uint32_t*)mu_sparse_array_get(&sa, 42) == 0);
        mu_sparse_array_destroy(&sa);

        CHECK(!mu_sparse_array_init(&sa, 0, 4));
        CHECK(!mu_sparse_array_init(&sa, sizeof(uint32_t), 3));
    }

    /* ------------------------------------------------------------------ */
    printf("=== slab ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_slab_parent parent;
        mu_slab_child c1, c2;
        void* p[10];
        uint32_t i, j;

        CHECK(mu_slab_parent_init(&parent, 32, 4));
        CHECK(mu_slab_child_init_batch(&c1, &parent, 2));
        CHECK(mu_slab_child_init(&c2, &parent));

        for (i = 0; i < 10; ++i)
        {
            p[i] = mu_slab_zalloc(&c1);
            CHECK(p[i] != NULL);
            CHECK(((uintptr_t)p[i] % 16u) == 0);
            if (p[i])
            {
                ((uint32_t*)p[i])[0] = i;
                CHECK(((uint8_t*)p[i])[5] == 0);
            }
        }

        for (i = 0; i < 10; ++i)
            for (j = i + 1; j < 10; ++j)
                CHECK(p[i] != p[j]);

        mu_slab_free(&c1, p[0]);
        CHECK(mu_slab_alloc(&c1) == p[0]);

        mu_slab_free(&c2, p[1]);
        CHECK(mu_slab_alloc(&c2) == p[1]);

        mu_slab_free_shared(&parent, p[2]);
        p[2] = NULL;

        mu_slab_child_destroy(&c1);
        mu_slab_child_destroy(&c2);
        mu_slab_parent_destroy(&parent);

        CHECK(!mu_slab_parent_init(&parent, 0, 4));
        CHECK(!mu_slab_parent_init(&parent, 16, 0));
    }

    /* ------------------------------------------------------------------ */
    printf("=== deferred free ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_deferred_free q;
        int marker = 0;
        int freed = 0;
        uint32_t i;

        CHECK(mu_deferred_free_init(&q, 3, 4, deferred_count_free, &freed));
        CHECK(mu_deferred_free_push(&q, &marker));
        CHECK(mu_deferred_free_pending(&q) == 1);

        mu_deferred_free_advance(&q);
        CHECK(freed == 0);
        mu_deferred_free_advance(&q);
        CHECK(freed == 0);
        mu_deferred_free_advance(&q);
        CHECK(freed == 1);
        CHECK(mu_deferred_free_pending(&q) == 0);

        for (i = 0; i < 4; ++i)
            CHECK(mu_deferred_free_push(&q, &marker));
        CHECK(!mu_deferred_free_push(&q, &marker));
        CHECK(mu_deferred_free_pending(&q) == 4);

        mu_deferred_free_drain(&q);
        CHECK(freed == 5);
        CHECK(mu_deferred_free_pending(&q) == 0);
        mu_deferred_free_destroy(&q);

        {
            int* raw = (int*)MU_MALLOC(sizeof(int));
            CHECK(raw != NULL);
            if (raw)
            {
                CHECK(mu_deferred_free_init(&q, 1, 1, NULL, NULL));
                CHECK(mu_deferred_free_push(&q, raw));
                mu_deferred_free_advance(&q);
                mu_deferred_free_destroy(&q);
            }
        }
    }


    /* ------------------------------------------------------------------ */
    printf("=== rb tree ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_rb_tree tree;
        TestRbItem items[7];
        int keys[7] = {5, 3, 8, 1, 4, 7, 9};
        int i;
        int key;
        int prev = -1;
        int count = 0;

        mu_rb_tree_init(&tree);
        for (i = 0; i < 7; ++i)
        {
            items[i].key = keys[i];
            mu_rb_tree_insert(&tree, &items[i].node, test_rb_compare);
        }
        mu_rb_tree_validate(&tree);

        key = 4;
        CHECK(mu_rb_tree_search(&tree, &key, test_rb_search_compare) == &items[4].node);
        key = 6;
        CHECK(mu_rb_tree_search(&tree, &key, test_rb_search_compare) == NULL);
        CHECK(mu_rb_tree_first(&tree) == &items[3].node);
        CHECK(mu_rb_tree_last(&tree) == &items[6].node);

        mu_rb_tree_foreach(TestRbItem, item, &tree, node)
        {
            CHECK(item->key > prev);
            prev = item->key;
            ++count;
        }
        CHECK(count == 7);

        mu_rb_tree_remove(&tree, &items[4].node);
        mu_rb_tree_validate(&tree);
        key = 4;
        CHECK(mu_rb_tree_search(&tree, &key, test_rb_search_compare) == NULL);

        for (i = 0; i < 7; ++i)
            if (i != 4)
                mu_rb_tree_remove(&tree, &items[i].node);
        mu_rb_tree_validate(&tree);
        CHECK(mu_rb_tree_is_empty(&tree));
    }

    /* ------------------------------------------------------------------ */
    printf("=== interval tree ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_rb_tree tree;
        TestInterval intervals[3];
        mu_interval query = {3, 8};
        mu_interval_node* first;
        mu_interval_node* second;
        int count = 0;

        MU_MEMSET(intervals, 0, sizeof(intervals));
        mu_rb_tree_init(&tree);
        intervals[0].id = 1;
        intervals[0].node.interval.start = 1;
        intervals[0].node.interval.end = 4;
        intervals[1].id = 2;
        intervals[1].node.interval.start = 7;
        intervals[1].node.interval.end = 9;
        intervals[2].id = 3;
        intervals[2].node.interval.start = 12;
        intervals[2].node.interval.end = 15;
        mu_interval_tree_insert(&tree, &intervals[0].node);
        mu_interval_tree_insert(&tree, &intervals[1].node);
        mu_interval_tree_insert(&tree, &intervals[2].node);
        mu_rb_tree_validate(&tree);

        first = mu_interval_tree_first(&tree, query);
        CHECK(first == &intervals[0].node);
        second = mu_interval_node_next(first, query);
        CHECK(second == &intervals[1].node);
        CHECK(mu_interval_node_next(second, query) == NULL);

        mu_interval_tree_foreach(TestInterval, item, query, &tree, node)
        {
            ++count;
            CHECK(item->id == count);
        }
        CHECK(count == 2);

        mu_interval_tree_remove(&tree, &intervals[0].node);
        mu_rb_tree_validate(&tree);
        CHECK(mu_interval_tree_first(&tree, query) == &intervals[1].node);
        mu_interval_tree_remove(&tree, &intervals[1].node);
        mu_interval_tree_remove(&tree, &intervals[2].node);
        CHECK(mu_rb_tree_is_empty(&tree));
    }

    /* ------------------------------------------------------------------ */
    printf("=== slab manager ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_slab_manager mgr;
        mu_slab_manager_entry* e0;
        mu_slab_manager_entry* e1;
        mu_slab_manager_entry* e2;
        mu_slab_manager_entry* e3;
        mu_slab_manager_entry* e4;
        mu_slab_manager_entry* e24;

        CHECK(mu_slab_manager_init(&mgr, 4, 6, 2, true, NULL,
                                   test_slab_can_reclaim, test_slab_alloc,
                                   test_slab_free));
        e0 = mu_slab_manager_alloc(&mgr, 16, 0);
        e1 = mu_slab_manager_alloc(&mgr, 16, 0);
        e2 = mu_slab_manager_alloc(&mgr, 16, 0);
        e3 = mu_slab_manager_alloc(&mgr, 16, 0);
        CHECK(e0 && e1 && e2 && e3);
        if (e0 && e1 && e2 && e3)
        {
            CHECK(e0->slab == e1->slab);
            CHECK(e1->slab == e2->slab);
            CHECK(e2->slab == e3->slab);
        }

        MU_CONTAINER_OF(e0, TestSlabEntry, entry)->reclaimable = true;
        MU_CONTAINER_OF(e1, TestSlabEntry, entry)->reclaimable = false;
        mu_slab_manager_free(&mgr, e0);
        mu_slab_manager_free(&mgr, e1);
        CHECK(mu_slab_manager_reclaim(&mgr) == 1);
        CHECK(mu_slab_manager_alloc(&mgr, 16, 0) == e0);

        e4 = mu_slab_manager_alloc(&mgr, 16, 0);
        CHECK(e4 != NULL);
        e24 = mu_slab_manager_alloc(&mgr, 24, 1);
        CHECK(e24 != NULL);
        if (e24)
            CHECK(e24->slab->entry_size == 24);

        MU_CONTAINER_OF(e0, TestSlabEntry, entry)->reclaimable = true;
        MU_CONTAINER_OF(e1, TestSlabEntry, entry)->reclaimable = true;
        MU_CONTAINER_OF(e2, TestSlabEntry, entry)->reclaimable = true;
        MU_CONTAINER_OF(e3, TestSlabEntry, entry)->reclaimable = true;
        if (e4)
            MU_CONTAINER_OF(e4, TestSlabEntry, entry)->reclaimable = true;
        if (e24)
            MU_CONTAINER_OF(e24, TestSlabEntry, entry)->reclaimable = true;

        mu_slab_manager_free(&mgr, e0);
        mu_slab_manager_free(&mgr, e2);
        mu_slab_manager_free(&mgr, e3);
        if (e4)
            mu_slab_manager_free(&mgr, e4);
        if (e24)
            mu_slab_manager_free(&mgr, e24);
        mu_slab_manager_deinit(&mgr);
        CHECK(mgr.groups == NULL);
    }

    /* ------------------------------------------------------------------ */
    printf("=== id allocator ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_id_alloc ids;
        mu_id_alloc range;
        mu_id_alloc_mt mt;
        mu_id_alloc_sparse sparse;
        uint32_t i;
        uint32_t base;
        uint32_t count = 0;

        CHECK(mu_id_alloc_init(&ids, 8));
        for (i = 0; i < 8; ++i)
            CHECK(mu_id_alloc_alloc(&ids) == i);
        CHECK(mu_id_alloc_alloc(&ids) == 8);
        CHECK(mu_id_alloc_alloc(&ids) == 9);
        mu_id_alloc_free(&ids, 3);
        CHECK(mu_id_alloc_alloc(&ids) == 3);
        mu_id_alloc_reserve(&ids, 100);
        CHECK(mu_id_alloc_test(&ids, 100));
        mu_id_alloc_free(&ids, 100);
        CHECK(!mu_id_alloc_test(&ids, 100));
        mu_id_alloc_foreach(&ids, id)
        {
            CHECK(id < 200);
            ++count;
        }
        CHECK(count > 0);
        mu_id_alloc_fini(&ids);

        CHECK(mu_id_alloc_init(&range, 64));
        base = mu_id_alloc_alloc_range(&range, 33);
        CHECK(base == 0);
        CHECK(mu_id_alloc_test(&range, 0));
        CHECK(mu_id_alloc_test(&range, 32));
        CHECK(!mu_id_alloc_test(&range, 33));
        for (i = 0; i < 33; ++i)
            mu_id_alloc_free(&range, base + i);
        CHECK(mu_id_alloc_alloc_range(&range, 33) == 0);
        mu_id_alloc_fini(&range);

        CHECK(mu_id_alloc_mt_init(&mt, 8, true));
        CHECK(mu_id_alloc_mt_alloc(&mt) == 1);
        mu_id_alloc_mt_free(&mt, 0);
        mu_id_alloc_mt_free(&mt, 1);
        CHECK(mu_id_alloc_mt_alloc(&mt) == 1);
        mu_id_alloc_mt_fini(&mt);

        CHECK(mu_id_alloc_sparse_init(&sparse));
        CHECK(mu_id_alloc_sparse_alloc(&sparse) == 0);
        base = mu_id_alloc_sparse_alloc_range(&sparse, 33);
        CHECK(base != MU_INVALID_INDEX);
        if (base != MU_INVALID_INDEX)
            for (i = 0; i < 33; ++i)
                mu_id_alloc_sparse_free(&sparse, base + i);
        mu_id_alloc_sparse_reserve(&sparse, 1000000);
        mu_id_alloc_sparse_free(&sparse, 1000000);
        mu_id_alloc_sparse_fini(&sparse);
    }

    /* ------------------------------------------------------------------ */
    printf("=== sparse bitset ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_sparse_bitset bits;
        mu_sparse_bitset other;
        mu_sparse_bitset copy;
        uint32_t expected[3] = {5, 1024, 100000};
        uint32_t from = 0;
        int n = 0;

        mu_sparse_bitset_init(&bits);
        CHECK(mu_sparse_bitset_set(&bits, 5));
        CHECK(mu_sparse_bitset_set(&bits, 1024));
        CHECK(mu_sparse_bitset_set(&bits, 100000));
        CHECK(mu_sparse_bitset_count(&bits) == 3);
        CHECK(mu_sparse_bitset_test(&bits, 1024));
        CHECK(!mu_sparse_bitset_test(&bits, 1025));

        mu_sparse_bitset_foreach(&bits, bit)
        {
            CHECK(n < 3 && bit == expected[n]);
            ++n;
        }
        CHECK(n == 3);

        mu_sparse_bitset_clear(&bits, 1024);
        CHECK(mu_sparse_bitset_count(&bits) == 2);
        CHECK(mu_sparse_bitset_next_set(&bits, &from));
        CHECK(from == 5);

        mu_sparse_bitset_init(&other);
        CHECK(mu_sparse_bitset_set(&other, 200000));
        CHECK(mu_sparse_bitset_merge(&bits, &other) == 1);
        CHECK(mu_sparse_bitset_merge(&bits, &other) == 0);
        CHECK(mu_sparse_bitset_contains(&bits, &other));
        CHECK(mu_sparse_bitset_count(&bits) == 3);

        CHECK(mu_sparse_bitset_dup(&copy, &bits));
        CHECK(mu_sparse_bitset_count(&copy) == 3);
        CHECK(mu_sparse_bitset_test(&copy, 200000));

        mu_sparse_bitset_destroy(&copy);
        mu_sparse_bitset_destroy(&other);
        mu_sparse_bitset_clear_all(&bits);
        CHECK(mu_sparse_bitset_empty(&bits));
        mu_sparse_bitset_destroy(&bits);
    }

    /* ------------------------------------------------------------------ */
    printf("=== dag ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_dag dag;
        TestDagNode nodes[4];
        TestDagVisit visit;
        int i;

        mu_dag_init(&dag);
        MU_MEMSET(nodes, 0, sizeof(nodes));
        for (i = 0; i < 4; ++i)
        {
            nodes[i].id = i;
            mu_dag_init_node(&dag, &nodes[i].node);
        }

        CHECK(mu_dag_add_edge(&dag, &nodes[3].node, &nodes[2].node, 0));
        CHECK(mu_dag_add_edge(&dag, &nodes[2].node, &nodes[1].node, 0));
        CHECK(mu_dag_add_edge(&dag, &nodes[1].node, &nodes[0].node, 0));
        CHECK(dag.head == &nodes[3].node);
        CHECK(mu_dag_validate(&dag));

        MU_MEMSET(&visit, 0, sizeof(visit));
        CHECK(mu_dag_traverse_bottom_up(&dag, test_dag_visit, &visit));
        CHECK(visit.count == 4);
        for (i = 0; i < 4; ++i)
            CHECK(visit.ids[i] == i);

        mu_dag_prune_head(&dag, &nodes[3].node);
        CHECK(dag.head == &nodes[2].node);
        MU_MEMSET(&visit, 0, sizeof(visit));
        CHECK(mu_dag_traverse_bottom_up(&dag, test_dag_visit, &visit));
        CHECK(visit.count == 3);
        for (i = 0; i < 3; ++i)
            CHECK(visit.ids[i] == i);

        mu_dag_destroy(&dag);
        CHECK(dag.head == NULL && dag.nodes_head == NULL);

        {
            mu_dag cycle;
            TestDagNode a, b;
            mu_dag_init(&cycle);
            a.id = 0;
            b.id = 1;
            mu_dag_init_node(&cycle, &a.node);
            mu_dag_init_node(&cycle, &b.node);
            CHECK(mu_dag_add_edge(&cycle, &a.node, &b.node, 0));
            CHECK(mu_dag_add_edge(&cycle, &b.node, &a.node, 0));
            CHECK(!mu_dag_validate(&cycle));
            mu_dag_destroy(&cycle);
        }
    }

    /* ------------------------------------------------------------------ */
    printf("=== range remap ===\n");
    /* ------------------------------------------------------------------ */
    {
        mu_range_remap remap;
        const mu_range_entry* entry;

        mu_range_remap_init(&remap);
        CHECK(mu_range_remap_insert(&remap, 10, 20, (void*)(uintptr_t)1) != NULL);
        CHECK(mu_range_remap_insert(&remap, 30, 40, (void*)(uintptr_t)2) != NULL);
        CHECK(mu_range_remap_insert(&remap, 50, 60, (void*)(uintptr_t)3) != NULL);
        entry = mu_range_remap_find(&remap, 15);
        CHECK(entry && entry->ptr == (void*)(uintptr_t)1);
        CHECK(mu_range_remap_find(&remap, 25) == NULL);
        entry = mu_range_remap_find(&remap, 30);
        CHECK(entry && entry->ptr == (void*)(uintptr_t)2);
        entry = mu_range_remap_find(&remap, 60);
        CHECK(entry && entry->ptr == (void*)(uintptr_t)3);
        CHECK(mu_range_remap_find(&remap, 61) == NULL);

        CHECK(mu_range_remap_insert(&remap, 19, 25, (void*)(uintptr_t)4) == NULL);
        CHECK(mu_range_remap_insert(&remap, 21, 29, (void*)(uintptr_t)4) != NULL);
        entry = mu_range_remap_find(&remap, 25);
        CHECK(entry && entry->ptr == (void*)(uintptr_t)4);
        CHECK(mu_range_remap_insert(&remap, 10, 20, (void*)(uintptr_t)5) != NULL);
        entry = mu_range_remap_find(&remap, 12);
        CHECK(entry && entry->ptr == (void*)(uintptr_t)5);

        CHECK(mu_range_remap_insert_truncate(&remap, 30, 35, (void*)(uintptr_t)6) != NULL);
        CHECK(mu_range_remap_find(&remap, 36) == NULL);
        entry = mu_range_remap_find(&remap, 35);
        CHECK(entry && entry->ptr == (void*)(uintptr_t)6);

        CHECK(mu_range_remap_remove(&remap, 21, 29));
        CHECK(mu_range_remap_find(&remap, 25) == NULL);
        mu_range_remap_destroy(&remap);
    }

    /* ------------------------------------------------------------------ */
    printf("=== perf upgrades: sort, bitset-simd, hash, bloom256, simd8, spsc ===\n");
    /* ------------------------------------------------------------------ */
    {
        /* 1. mu_sort: insertion sort payload bugfix (n <= 32) + radix pass-skip + float radix */
        uint32_t sk[20], sp[20];
        for (uint32_t i = 0; i < 20u; ++i) {
            sk[i] = 20u - i;
            sp[i] = 1000u + (20u - i);
        }
        CHECK(mu_radix_sort_u32(sk, sp, 20u));
        for (uint32_t i = 0; i < 20u; ++i) {
            CHECK(sk[i] == i + 1u);
            CHECK(sp[i] == 1000u + i + 1u);
        }

        /* Uniform upper-bytes u64 radix sort (exercises 1-scan hist + zero-pass skip): */
        enum { RN = 512 };
        uint64_t rk64[RN], tk64[RN];
        uint32_t rp64[RN], tp64[RN];
        for (uint32_t i = 0; i < RN; ++i) {
            uint32_t v = (i * 73u + 19u) & 0x3FFFu; /* upper 6 bytes are all 0! */
            rk64[i] = (uint64_t)v;
            rp64[i] = v ^ 0x55AAu;
        }
        mu_radix_sort_u64_scratch(rk64, rp64, tk64, tp64, RN);
        for (uint32_t i = 1; i < RN; ++i) {
            CHECK(rk64[i - 1] <= rk64[i]);
            CHECK(rp64[i] == ((uint32_t)rk64[i] ^ 0x55AAu));
        }

        /* IEEE-754 float radix sort including negatives, -0.0f, +0.0f: */
        float fk[128];
        uint32_t fp_pay[128];
        for (uint32_t i = 0; i < 128u; ++i) {
            float sign = (i & 1u) ? -1.0f : 1.0f;
            fk[i] = sign * (float)((i * 37u) % 97u) * 0.25f;
            fp_pay[i] = i;
        }
        fk[0] = 0.0f;
        fk[1] = -0.0f;
        CHECK(mu_radix_sort_f32(fk, fp_pay, 128u));
        for (uint32_t i = 1; i < 128u; ++i)
            CHECK(fk[i - 1] <= fk[i]);

        /* 2. Large bitset SIMD popcount & binary-op counts (>= 300 words = 19,200 bits): */
        mu_bitset* b1 = mu_bitset_create_with_capacity(20000);
        mu_bitset* b2 = mu_bitset_create_with_capacity(20000);
        size_t exp_c1 = 0, exp_c2 = 0, exp_and = 0, exp_or = 0, exp_xor = 0, exp_diff = 0;
        for (size_t bit = 0; bit < 20000u; ++bit) {
            bool s1 = ((bit * 7u) % 13u) == 0u;
            bool s2 = ((bit * 11u) % 17u) == 0u;
            if (s1) { mu_bitset_set(b1, bit); ++exp_c1; }
            if (s2) { mu_bitset_set(b2, bit); ++exp_c2; }
            if (s1 && s2) ++exp_and;
            if (s1 || s2) ++exp_or;
            if (s1 != s2) ++exp_xor;
            if (s1 && !s2) ++exp_diff;
        }
        CHECK(mu_bitset_count(b1) == exp_c1);
        CHECK(mu_bitset_count(b2) == exp_c2);
        CHECK(mu_bitset_intersection_count(b1, b2) == exp_and);
        CHECK(mu_bitset_union_count(b1, b2) == exp_or);
        CHECK(mu_bitset_symmetric_difference_count(b1, b2) == exp_xor);
        CHECK(mu_bitset_difference_count(b1, b2) == exp_diff);
        CHECK(!mu_bitset_empty(b1));
        CHECK(mu_bitsets_intersect(b1, b2));
        mu_bitset_free(b1);
        mu_bitset_free(b2);

        /* 3. mu_hash32_t non-pow2 fastrange + Knuth Algorithm R backward-shift remove: */
        mu_hash32_t ht;
        mu_hash32_init(&ht, 97u); /* prime capacity tests non-pow2 fastrange32 */
        for (uint32_t i = 1; i <= 65u; ++i)
            mu_hash32_set(&ht, (uint64_t)i * 1000003ull, i * 10u);
        for (uint32_t i = 1; i <= 65u; i += 2u)
            mu_hash32_remove(&ht, (uint64_t)i * 1000003ull);
        for (uint32_t i = 1; i <= 65u; ++i) {
            uint32_t hv = 0;
            bool ok = mu_hash32_get(&ht, (uint64_t)i * 1000003ull, &hv);
            if (i & 1u) CHECK(!ok);
            else        CHECK(ok && hv == i * 10u);
        }
        mu_hash32_destroy(&ht);

        /* 4. mu_bloom_simd256 (256-bit split-block SIMD Bloom filter): */
        mu_bloom_simd256 bf256;
        CHECK(mu_bloom_simd256_init(&bf256, 2000u, 12u));
        for (uint64_t i = 0; i < 2000ull; ++i)
            mu_bloom_simd256_add(&bf256, i * 0x9E3779B97F4A7C15ull + 1ull);
        uint32_t fn256 = 0, fp256 = 0;
        for (uint64_t i = 0; i < 2000ull; ++i)
            if (!mu_bloom_simd256_test(&bf256, i * 0x9E3779B97F4A7C15ull + 1ull))
                ++fn256;
        for (uint64_t i = 2000ull; i < 12000ull; ++i)
            if (mu_bloom_simd256_test(&bf256, i * 0x9E3779B97F4A7C15ull + 1ull))
                ++fp256;
        CHECK(fn256 == 0u);
        CHECK(fp256 < 300u); /* < 3% FPR */
        mu_bloom_simd256_destroy(&bf256);

        /* 5. mu_simd: mu_f32x4_length fix + mu_f32x8 + 8x8 transpose: */
        mu_f32x4 v34 = mu_f32x4_set(3.0f, 4.0f, 0.0f, 0.0f);
        CHECK(fabsf(mu_f32x4_length(v34) - 5.0f) < 1e-5f);
        mu_f32x8 va = mu_f32x8_set(1, 2, 3, 4, 5, 6, 7, 8);
        mu_f32x8 vb = mu_f32x8_set1(2.0f);
        mu_f32x8 vc = mu_f32x8_fmadd(va, vb, mu_f32x8_set1(1.0f)); /* 2*i + 1 */
        CHECK(fabsf(mu_f32x8_hadd(vc) - 80.0f) < 1e-4f);
        mu_f32x8 rsq = mu_f32x8_rsqrt_nr(mu_f32x8_set1(4.0f));
        CHECK(fabsf(rsq.m[0] - 0.5f) < 1e-5f && fabsf(rsq.m[7] - 0.5f) < 1e-5f);
        mu_f32x8 mat[8];
        for (int r = 0; r < 8; ++r)
            mat[r] = mu_f32x8_set((float)(r*10+0), (float)(r*10+1), (float)(r*10+2), (float)(r*10+3),
                                  (float)(r*10+4), (float)(r*10+5), (float)(r*10+6), (float)(r*10+7));
        mu_f32x8_transpose8(mat);
        CHECK(mat[0].m[7] == 70.0f && mat[7].m[0] == 7.0f && mat[3].m[5] == 53.0f);

        /* 6. mu_spsc_queue shadow-cached batch push/pop across wrap-around: */
        mu_spsc q;
        CHECK(mu_spsc_init(&q, sizeof(uint32_t), 64u));
        uint32_t in_batch[40], out_batch[40];
        for (uint32_t round = 0; round < 5u; ++round) {
            for (uint32_t i = 0; i < 40u; ++i) in_batch[i] = round * 100u + i;
            CHECK(mu_spsc_push_batch(&q, in_batch, 40u) == 40u);
            CHECK(mu_spsc_pop_batch(&q, out_batch, 40u) == 40u);
            for (uint32_t i = 0; i < 40u; ++i) CHECK(out_batch[i] == round * 100u + i);
        }
        mu_spsc_destroy(&q);
    }

    /* ------------------------------------------------------------------ */
    printf("=== new high-perf ds: swiss_map, dense_map, eytzinger, stree16, xor8, 4-heap, timer_wheel, fast_codec ===\n");
    /* ------------------------------------------------------------------ */
    {
        /* 1. mu_swiss_map (supports 0 and UINT64_MAX keys + SIMD H2 probing): */
        mu_swiss_map sm;
        CHECK(mu_swiss_map_init(&sm, 32u));
        CHECK(mu_swiss_map_set(&sm, 0ull, 111ull));
        CHECK(mu_swiss_map_set(&sm, UINT64_MAX, 222ull));
        for (uint64_t k = 1; k <= 1500ull; ++k)
            CHECK(mu_swiss_map_set(&sm, k * 1315423911ull, k + 7ull));
        uint64_t sv = 0;
        CHECK(mu_swiss_map_get(&sm, 0ull, &sv) && sv == 111ull);
        CHECK(mu_swiss_map_get(&sm, UINT64_MAX, &sv) && sv == 222ull);
        for (uint64_t k = 1; k <= 1500ull; k += 3ull)
            CHECK(mu_swiss_map_remove(&sm, k * 1315423911ull));
        for (uint64_t k = 1; k <= 1500ull; ++k) {
            bool ok = mu_swiss_map_get(&sm, k * 1315423911ull, &sv);
            if (k % 3ull == 1ull) CHECK(!ok);
            else                  CHECK(ok && sv == k + 7ull);
        }
        mu_swiss_map_destroy(&sm);

        /* 2. mu_dense_map (SoA contiguous keys/values + O(1) swap-erase): */
        mu_dense_map dm;
        CHECK(mu_dense_map_init(&dm, 32u));
        for (uint64_t k = 0; k < 800ull; ++k)
            CHECK(mu_dense_map_set(&dm, k * 2654435761ull, k * 10ull));
        for (uint64_t k = 0; k < 800ull; k += 2ull)
            CHECK(mu_dense_map_remove(&dm, k * 2654435761ull));
        CHECK(dm.count == 400u);
        uint64_t dense_sum = 0;
        for (uint32_t i = 0; i < dm.count; ++i)
            dense_sum += dm.values[i];
        /* Sum of odd k*10 for k=1,3,...,799: 10 * (400^2) = 1,600,000 */
        CHECK(dense_sum == 1600000ull);
        for (uint64_t k = 1; k < 800ull; k += 2ull)
            CHECK(mu_dense_map_get(&dm, k * 2654435761ull, &sv) && sv == k * 10ull);
        mu_dense_map_destroy(&dm);

        /* 3. mu_eytzinger_u32 & mu_stree16_i32 static search trees: */
        uint32_t sorted_u32[500];
        int32_t  sorted_i32[500];
        for (uint32_t i = 0; i < 500u; ++i) {
            sorted_u32[i] = i * 3u + 5u;
            sorted_i32[i] = (int32_t)i * 3 - 500;
        }
        mu_eytzinger_u32 ey;
        mu_stree16_i32   st;
        CHECK(mu_eytzinger_u32_build(&ey, sorted_u32, 500u));
        CHECK(mu_stree16_i32_build(&st, sorted_i32, 500u));
        CHECK(mu_eytzinger_u32_lower_bound(&ey, 0u) == 0u);
        CHECK(mu_eytzinger_u32_lower_bound(&ey, 99999u) == 500u);
        CHECK(mu_stree16_i32_lower_bound(&st, -9999) == 0u);
        CHECK(mu_stree16_i32_lower_bound(&st, 99999) == 500u);
        for (uint32_t i = 0; i < 500u; ++i) {
            CHECK(mu_eytzinger_u32_contains(&ey, sorted_u32[i]));
            CHECK(!mu_eytzinger_u32_contains(&ey, sorted_u32[i] + 1u));
            CHECK(mu_eytzinger_u32_lower_bound(&ey, sorted_u32[i]) == i);
            CHECK(mu_eytzinger_u32_lower_bound(&ey, sorted_u32[i] + 1u) == i + 1u);

            CHECK(mu_stree16_i32_contains(&st, sorted_i32[i]));
            CHECK(!mu_stree16_i32_contains(&st, sorted_i32[i] + 1));
            CHECK(mu_stree16_i32_lower_bound(&st, sorted_i32[i]) == i);
            CHECK(mu_stree16_i32_lower_bound(&st, sorted_i32[i] + 1) == i + 1u);
        }
        mu_eytzinger_u32_destroy(&ey);
        mu_stree16_i32_destroy(&st);

        /* 4. mu_xor8_filter (Graf & Lemire ~9.84 bits/key static filter): */
        uint64_t xkeys[1024];
        for (uint32_t i = 0; i < 1024u; ++i)
            xkeys[i] = (uint64_t)i * 0x517cc1b727220a95ull + 17ull;
        mu_xor8_filter xf;
        CHECK(mu_xor8_build(&xf, xkeys, 1024u));
        CHECK(mu_xor8_byte_size(&xf) < 1400u); /* ~1.23 * 1024 + 32 */
        uint32_t xfn = 0, xfp = 0;
        for (uint32_t i = 0; i < 1024u; ++i)
            if (!mu_xor8_contains(&xf, xkeys[i])) ++xfn;
        for (uint32_t i = 1024u; i < 11024u; ++i)
            if (mu_xor8_contains(&xf, (uint64_t)i * 0x517cc1b727220a95ull + 17ull)) ++xfp;
        CHECK(xfn == 0u);
        CHECK(xfp < 100u); /* expected ~39 / 10,000 (~0.39%) */
        mu_xor8_destroy(&xf);

        /* 5. mu_four_heap (SoA 4-ary min-heap): */
        mu_four_heap fh;
        CHECK(mu_four_heap_init(&fh, 32u));
        for (uint32_t i = 0; i < 600u; ++i) {
            uint64_t k = ((uint64_t)i * 397ull + 23ull) % 600ull;
            CHECK(mu_four_heap_push(&fh, k, (uint32_t)k + 50u));
        }
        uint64_t prev_k = 0;
        for (uint32_t i = 0; i < 600u; ++i) {
            uint64_t hk = 0;
            uint32_t hv = 0;
            CHECK(mu_four_heap_pop(&fh, &hk, &hv));
            CHECK(hk >= prev_k && hv == (uint32_t)hk + 50u);
            prev_k = hk;
        }
        CHECK(mu_four_heap_empty(&fh));
        mu_four_heap_destroy(&fh);

        /* 6. mu_timer_wheel (O(1) 4-level x 64-slot hierarchical bitmasked wheel): */
        mu_timer_wheel tw;
        CHECK(mu_timer_wheel_init(&tw, 64u, 0ull));
        uint32_t t1 = mu_timer_schedule(&tw, 5ull, 10ull);
        uint32_t t2 = mu_timer_schedule(&tw, 130ull, 20ull);     /* level 1 */
        uint32_t t3 = mu_timer_schedule(&tw, 10000ull, 30ull);   /* level 2 */
        uint32_t t4 = mu_timer_schedule(&tw, 300000ull, 40ull);  /* level 3 */
        uint32_t tc = mu_timer_schedule(&tw, 500ull, 999ull);    /* cancelled! */
        CHECK(t1 != MU_TW_INVALID && t2 != MU_TW_INVALID && t3 != MU_TW_INVALID && t4 != MU_TW_INVALID);
        CHECK(mu_timer_cancel(&tw, tc));
        CHECK(!mu_timer_cancel(&tw, tc));
        TestTimerRecord trec = {0, 0};
        CHECK(mu_timer_wheel_advance_to(&tw, 200ull, test_timer_fire_cb, &trec) == 2u);
        CHECK(trec.count == 2u && trec.sum_user == 30ull);
        CHECK(mu_timer_wheel_advance_to(&tw, 350000ull, test_timer_fire_cb, &trec) == 2u);
        CHECK(trec.count == 4u && trec.sum_user == 100ull && tw.active_count == 0u);
        mu_timer_wheel_destroy(&tw);

        /* 7. mu_fast_codec (wyhash64, AES/CRC hash, fastmod, Morton 2D/3D, StreamVByte): */
        uint8_t msg[300];
        for (uint32_t i = 0; i < 300u; ++i) msg[i] = (uint8_t)(i * 13u + 7u);
        CHECK(mu_wyhash64(msg, 300u, 42ull) == mu_wyhash64(msg, 300u, 42ull));
        CHECK(mu_wyhash64(msg, 300u, 42ull) != mu_wyhash64(msg, 299u, 42ull));
        CHECK(mu_hash_bytes_fast(msg, 300u, 42ull) == mu_hash_bytes_fast(msg, 300u, 42ull));
        CHECK(mu_hash_bytes_fast(msg, 300u, 42ull) != mu_hash_bytes_fast(msg, 300u, 43ull));

        mu_fastdiv_u32 fd97 = mu_fastdiv_u32_make(97u);
        for (uint32_t x = 0; x < 5000u; ++x) {
            uint32_t val = x * 265443u + 17u;
            CHECK(mu_fastmod_u32(val, fd97) == (val % 97u));
            CHECK(mu_fast_is_divisible_u32(val, fd97) == ((val % 97u) == 0u));
        }

        uint32_t mc = mu_morton2d_encode(317u, 942u);
        uint16_t mx = 0, my = 0;
        mu_morton2d_decode(mc, &mx, &my);
        CHECK(mx == 317u && my == 942u);
        CHECK(mu_morton2d_inc_x(mc) == mu_morton2d_encode(318u, 942u));
        CHECK(mu_morton2d_inc_y(mc) == mu_morton2d_encode(317u, 943u));

        uint16_t m3x = 0, m3y = 0, m3z = 0;
        mu_morton3d_decode(mu_morton3d_encode(511u, 123u, 999u), &m3x, &m3y, &m3z);
        CHECK(m3x == 511u && m3y == 123u && m3z == 999u);

        uint32_t svb_in[131], svb_out[131];
        uint8_t  svb_buf[1024];
        for (uint32_t i = 0; i < 131u; ++i) {
            /* Mix 1B, 2B, 3B, and 4B values + non-multiple-of-4 length 131: */
            uint32_t mode = i & 3u;
            svb_in[i] = (mode == 0u) ? (i & 0x7Fu) :
                        (mode == 1u) ? (0x1200u + i) :
                        (mode == 2u) ? (0x340000u + i * 17u) : (0x89ABCDEFu ^ i);
        }
        size_t enc_len = mu_streamvbyte_encode_u32(svb_in, 131u, svb_buf);
        size_t dec_len = mu_streamvbyte_decode_u32(svb_buf, 131u, svb_out);
        CHECK(enc_len == dec_len && enc_len < 131u * 4u);
        for (uint32_t i = 0; i < 131u; ++i)
            CHECK(svb_out[i] == svb_in[i]);
    }

    /* --- new modules --- */
    check_mu_files();
    check_mu_grid();
    check_mu_timeline();
    check_mu_geom();
    check_mu_camera();
    check_mu_particles();
    check_mu_behavior_fsm();
    check_mu_behavior_btree();
    check_mu_behavior_utility();
    check_mu_blackboard();
    check_mu_stats();
    check_mu_small_vec();
    check_mu_sync();
    check_mu_influence();
    check_mu_mcts();
    check_mu_htn();
    check_mu_production();
    check_mu_petri();
    check_mu_ngram();
    check_mu_genetic();
    check_mu_possibility();
    check_mu_constraint();
    check_mu_stochastic_grammar();
    check_mu_lsystem();
    check_mu_reaction_diffusion();

    if (failures == 0)
        printf("\nAll checks passed.\n");
    else
        printf("\n%d check(s) failed.\n", failures);

    return failures == 0 ? 0 : 1;
}
