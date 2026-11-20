/*
 * main.c — a floating island, drawn like a cartoon.
 *
 * Everything happens in main(), top to bottom: options, window, meshes, scene,
 * then the frame loop. GPU work goes through vk.h and the window through
 * platform.h: this file makes no Vulkan or GLFW call. Shapes come from
 * mu_shapes, math from mu_math, motion from mu_easing, scatter from mu_pcg,
 * batch order from mu_sort, rock weathering from mu_noise_math, keys and frame
 * time from mu_input, the idle sleep from mu_time, messages from mu_log and
 * the bit count behind the bloom mip count from mu_common.
 *
 * The frame, in pass order:
 *
 *   petals     compute   advances the falling petals in place in the device arena
 *   shadow     graphics  sun depth, 2048^2, back faces only
 *   scene      graphics  4x MSAA, three targets resolved in the pass:
 *                          colour (HDR), normal + depth, outline id + ink weight
 *                        cel bands one pixel wide, banded highlight and rims,
 *                        ink hatching in shadow, then petals, then the sky
 *                        behind everything
 *   bloom      graphics  four levels down, three additive levels back up;
 *                        the levels are the mips of one image
 *   composite  graphics  ink lines from normal/depth/id, bloom, tone map,
 *                        grade, paper, sRGB — into the frame image
 *
 * and once, before the first frame:
 *
 *   paper      compute   bakes the paper texture the composite draws on
 *
 * Run it:
 *   ./toon                         window; drag to orbit, O ink, B bloom, H hatch,
 *                                  P paper, Space pause, F2 screenshot, Esc quit
 *   ./toon --shot out.png          no window: render --frames frames, save the last
 *   options: --width N --height N --msaa 1|2|4|8 --frames N --validate 0|1
 *            --stats 0|1, and where the camera and the clock start:
 *            --orbit DEGREES --elevation DEGREES --seconds N
 */

#include "external/mu_lib/mu/mu_macros.h" /* first: owns the POSIX feature test */
#include "external/mu_lib/mu/mu_common.h"
#include "external/mu_lib/mu/mu_math.h"
#include "external/mu_lib/mu/mu_noise_math.h" /* before mu_shapes.h: mu_shape_rock uses its fbm */
#define MU_NOISE_MATH_H /* what mu_shapes.h looks for; mu_noise_math.h no longer defines it (now #pragma once) */
#include "external/mu_lib/mu/mu_shapes.h"
#include "external/mu_lib/mu/mu_easing.h"
#include "external/mu_lib/mu/mu_input.h"
#include "external/mu_lib/mu/mu_log.h"
#include "external/mu_lib/mu/mu_pcg.h"
#include "external/mu_lib/mu/mu_sort.h"
#include "external/mu_lib/mu/mu_time.h"

#include "platform.h"
#include "vk.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/glfw/deps/stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SHADOW_SIZE  = 2048,
    PAPER_SIZE   = 256,
    BLOOM_LEVELS = 4,
    PETAL_COUNT  = 448,
    MAX_OBJECTS  = 512,
    MAX_BATCHES  = 64
};

/* ---- what the shaders read ------------------------------------------------ */

typedef enum Kind { KIND_TOON, KIND_TERRAIN, KIND_WATER, KIND_GLOW, KIND_FOLIAGE, KIND_CLOUD } Kind;

/* One drawn object; shaders/shared.slang has the same 128 bytes. */
typedef struct Instance {
    mu_mat4 model;
    mu_vec4 base;    /* rgb albedo (linear), a = Kind */
    mu_vec4 shade;   /* rgb multiplied into the albedo in shadow, a = hatch */
    mu_vec4 surface; /* specular strength, specular power, rim, emissive */
    mu_vec4 extra;   /* outline id, wind, phase, ink weight */
} Instance;

PUSH(ScenePush,
    mu_mat4  view_proj;
    mu_mat4  light_view_proj;
    mu_vec4  camera;    /* xyz eye, w seconds */
    mu_vec4  sun;       /* xyz towards the sun, w shadow texel in world units */
    mu_vec4  sun_color; /* rgb, w 1 / shadow map size */
    mu_vec4  sky_color; /* rgb, w hatch strength */
    uint64_t instances; /* device address of this draw's first Instance */
    uint32_t vertex_offset;
    uint32_t index_offset;
    uint32_t shadow_map;
    uint32_t shadow_sampler;
);

PUSH(SkyPush,
    mu_mat4 inverse_view_proj;
    mu_vec4 camera;
    mu_vec4 sun;
    mu_vec4 sun_color;
    mu_vec4 zenith;
    mu_vec4 horizon;
    mu_vec4 haze; /* rgb, w height of the cloud sea */
);

PUSH(FramePush,
    mu_vec4  texel; /* xy 1/size of the image read, zw 1/size of the frame */
    mu_vec4  ink;   /* rgb, a opacity */
    mu_vec4  edges; /* tap radius in pixels, depth, normal, id thresholds */
    mu_vec4  grade; /* exposure, bloom strength, saturation, vignette radius */
    mu_vec4  paper; /* grain, line wobble in pixels, seconds, bloom knee */
    uint32_t color_map;
    uint32_t aux_map;
    uint32_t id_map;
    uint32_t bloom_map;
    uint32_t paper_map;
    uint32_t linear_sampler;
    uint32_t mirror_sampler;
    float    bloom_threshold;
);

PUSH(ComputePush,
    mu_vec4  wind;   /* xyz drift per second, w seconds */
    mu_vec4  volume; /* xyz half extent of the petal box, w its floor */
    mu_vec4  origin; /* xyz centre of the box, w seconds since last frame */
    uint32_t storage_id;
    uint32_t petal_offset;
    uint32_t petal_count;
    uint32_t size;
);

/* ---- the scene as data ----------------------------------------------------- */

typedef enum MeshId {
    MESH_ISLAND, MESH_PLAZA, MESH_PEDESTAL, MESH_KNOT, MESH_RING,
    MESH_TRUNK, MESH_CANOPY, MESH_PINE_TRUNK, MESH_PINE, MESH_ROCK, MESH_BOULDER,
    MESH_POND, MESH_WALLS, MESH_ROOF, MESH_BOX, MESH_CAP, MESH_STEM,
    MESH_OCTA, MESH_ICOSA, MESH_DODECA, MESH_CLOUD, MESH_BALL,
    MESH_MOBIUS, MESH_SHELL, MESH_KLEIN, MESH_SPRING, MESH_POST, MESH_STAR,
    MESH_COUNT
} MeshId;

typedef enum MaterialId {
    MAT_GRASS, MAT_STONE, MAT_GOLD, MAT_BRASS, MAT_BARK, MAT_LEAF, MAT_LEAF_DEEP, MAT_BLOSSOM, MAT_PINE,
    MAT_ROCK, MAT_WATER, MAT_WALL, MAT_ROOF, MAT_WOOD, MAT_WINDOW, MAT_CAP, MAT_STEM,
    MAT_GEM_PINK, MAT_GEM_CYAN, MAT_GEM_VIOLET, MAT_GEM_AMBER, MAT_CLOUD, MAT_FIREFLY, MAT_LAMP,
    MAT_RIBBON, MAT_SHELL, MAT_GLASS, MAT_STEEL, MAT_FLOWER_SUN, MAT_FLOWER_ROSE, MAT_LILY, MAT_STAR,
    MAT_COUNT
} MaterialId;

/* ---- the images that are as large as the frame ------------------------------ */

/* The first three are what the scene leaves: HDR colour, normal + depth, outline id + ink weight. The MS ones are the
 * same three while the scene draws at more than 1x. They and the depth never leave the scene pass: transient, resolved
 * or discarded. The shot is the composite once more, for a file. The scene pass takes its three targets and its three
 * resolves as neighbours out of this list, which is why both triples stand in the same order. */
typedef enum TargetId { TARGET_COLOR, TARGET_AUX, TARGET_IDS, TARGET_MS_COLOR, TARGET_MS_AUX, TARGET_MS_IDS, TARGET_DEPTH, TARGET_SHOT, TARGET_COUNT } TargetId;

static const struct { const char *name; Format format; bool multisampled; } TARGETS[TARGET_COUNT] = {
    [TARGET_COLOR]    = { "colour", FMT_RGBA16F, false },   [TARGET_AUX]    = { "normal depth", FMT_RGBA16F, false },   [TARGET_IDS]    = { "id", FMT_RGBA8_UNORM, false },
    [TARGET_MS_COLOR] = { "ms colour", FMT_RGBA16F, true }, [TARGET_MS_AUX] = { "ms normal depth", FMT_RGBA16F, true }, [TARGET_MS_IDS] = { "ms id", FMT_RGBA8_UNORM, true },
    [TARGET_DEPTH]    = { "depth", FMT_D32F, true },        [TARGET_SHOT]   = { "shot", FMT_RGBA8_UNORM, false },
};

typedef enum MoveId { MOVE_STILL, MOVE_SPIN, MOVE_GYRO, MOVE_FLOAT, MOVE_ORBIT, MOVE_DRIFT, MOVE_HOVER, MOVE_BOUNCE, MOVE_COUNT } MoveId;

#define SRGB(hex) { (float)(((hex) >> 16) & 255) / 255.0f, (float)(((hex) >> 8) & 255) / 255.0f, (float)((hex) & 255) / 255.0f }
/* the exact inverse of the encode the composite ends with */
#define SRGB_TO_LINEAR(c) ((c) <= 0.04045f ? (c) / 12.92f : powf(((c) + 0.055f) / 1.055f, 2.4f))

/* Colours are written the way they are picked: sRGB. main() converts the
 * table to linear once. `shade` is the swatch the base is multiplied by in
 * shadow — never grey, so shadows keep a hue. */
typedef struct Material {
    mu_vec3 base;
    mu_vec3 shade; /* terrain: the rock colour under the grass */
    float   specular, shininess, rim, emissive, hatch, ink, wind;
    uint8_t kind, two_sided, casts;
} Material;

static const Material MATERIALS[MAT_COUNT] = {
    [MAT_GRASS]       = { SRGB(0x8fd15f), SRGB(0xc79c6e), .ink = 1.0f, .kind = KIND_TERRAIN, .casts = 1 },
    [MAT_STONE]       = { SRGB(0xeee3cc), SRGB(0xb0a8d6), .hatch = 0.7f, .ink = 1.0f, .casts = 1 },
    [MAT_GOLD]        = { SRGB(0xf9bc3c), SRGB(0xc9958f), .specular = 1.0f, .shininess = 42.0f, .rim = 1.0f, .ink = 1.0f, .casts = 1 },
    [MAT_BRASS]       = { SRGB(0xee9a3a), SRGB(0xc08a92), .specular = 0.8f, .shininess = 30.0f, .rim = 0.8f, .ink = 0.75f, .casts = 1 },
    [MAT_BARK]        = { SRGB(0x9a6748), SRGB(0xa99cc9), .hatch = 0.8f, .ink = 1.0f, .casts = 1 },
    [MAT_LEAF]        = { SRGB(0x66b85c), SRGB(0x8fb6c4), .rim = 0.5f, .ink = 1.0f, .wind = 0.05f, .kind = KIND_FOLIAGE, .casts = 1 },
    [MAT_LEAF_DEEP]   = { SRGB(0x3f9a63), SRGB(0x90b0cc), .rim = 0.5f, .ink = 1.0f, .wind = 0.05f, .kind = KIND_FOLIAGE, .casts = 1 },
    [MAT_BLOSSOM]     = { SRGB(0xfbb1c8), SRGB(0xc9a2dc), .rim = 0.6f, .ink = 1.0f, .wind = 0.06f, .kind = KIND_FOLIAGE, .casts = 1 },
    [MAT_PINE]        = { SRGB(0x2f8a63), SRGB(0x8ea9cf), .rim = 0.4f, .ink = 1.0f, .wind = 0.025f, .casts = 1 },
    [MAT_ROCK]        = { SRGB(0xb4bccb), SRGB(0xb9b2dd), .hatch = 1.0f, .ink = 1.0f, .casts = 1 },
    [MAT_WATER]       = { SRGB(0x52c8ec), SRGB(0xaab9e8), .specular = 0.7f, .shininess = 90.0f, .ink = 1.0f, .kind = KIND_WATER },
    [MAT_WALL]        = { SRGB(0xfff2d9), SRGB(0xb0a8d6), .hatch = 0.5f, .ink = 1.0f, .casts = 1 },
    [MAT_ROOF]        = { SRGB(0xe65a3a), SRGB(0xb898c4), .rim = 0.4f, .hatch = 0.4f, .ink = 1.0f, .casts = 1 },
    [MAT_WOOD]        = { SRGB(0x7d5240), SRGB(0xa99cc9), .hatch = 0.6f, .ink = 1.0f, .casts = 1 },
    [MAT_WINDOW]      = { SRGB(0xffd585), SRGB(0xffffff), .emissive = 2.6f, .ink = 1.0f, .kind = KIND_GLOW },
    [MAT_CAP]         = { SRGB(0xe8404f), SRGB(0xc29ac4), .specular = 0.6f, .shininess = 24.0f, .rim = 0.5f, .ink = 1.0f, .casts = 1 },
    [MAT_STEM]        = { SRGB(0xfff4e2), SRGB(0xb0a8d6), .ink = 1.0f, .casts = 1 },
    [MAT_GEM_PINK]    = { SRGB(0xff73a8), SRGB(0xb98fd9), .specular = 1.0f, .shininess = 70.0f, .rim = 1.4f, .emissive = 0.22f, .ink = 1.0f, .casts = 1 },
    [MAT_GEM_CYAN]    = { SRGB(0x52e3ea), SRGB(0x8fa6e2), .specular = 1.0f, .shininess = 70.0f, .rim = 1.4f, .emissive = 0.22f, .ink = 1.0f, .casts = 1 },
    [MAT_GEM_VIOLET]  = { SRGB(0xb880ff), SRGB(0x9a90e0), .specular = 1.0f, .shininess = 70.0f, .rim = 1.4f, .emissive = 0.22f, .ink = 1.0f, .casts = 1 },
    [MAT_GEM_AMBER]   = { SRGB(0xffc552), SRGB(0xc9958f), .specular = 1.0f, .shininess = 70.0f, .rim = 1.4f, .emissive = 0.22f, .ink = 1.0f, .casts = 1 },
    [MAT_CLOUD]       = { SRGB(0xffffff), SRGB(0xcdc2ee), .ink = 0.5f, .kind = KIND_CLOUD },
    [MAT_FIREFLY]     = { SRGB(0xffeaa8), SRGB(0xffffff), .emissive = 7.0f, .kind = KIND_GLOW },
    [MAT_LAMP]        = { SRGB(0xffd98f), SRGB(0xffffff), .emissive = 4.5f, .ink = 1.0f, .kind = KIND_GLOW },
    [MAT_RIBBON]      = { SRGB(0xff92ad), SRGB(0xbe9ad8), .specular = 0.7f, .shininess = 30.0f, .rim = 0.6f, .ink = 1.0f, .two_sided = 1, .casts = 1 },
    [MAT_SHELL]       = { SRGB(0xffe6d2), SRGB(0xc4a6d2), .specular = 0.5f, .shininess = 30.0f, .ink = 1.0f, .two_sided = 1, .casts = 1 },
    [MAT_GLASS]       = { SRGB(0x7fe0f3), SRGB(0x94a8e4), .specular = 1.0f, .shininess = 60.0f, .rim = 1.2f, .ink = 1.0f, .two_sided = 1, .casts = 1 },
    [MAT_STEEL]       = { SRGB(0xaab6ca), SRGB(0x9f98cc), .specular = 0.8f, .shininess = 50.0f, .rim = 0.5f, .ink = 1.0f, .casts = 1 },
    /* small things take a light line: at a few pixels across, full ink is all there would be */
    [MAT_FLOWER_SUN]  = { SRGB(0xffd96e), SRGB(0xd9a79f), .ink = 0.22f, .wind = 0.02f },
    [MAT_FLOWER_ROSE] = { SRGB(0xffa6c2), SRGB(0xd6b0e4), .ink = 0.22f, .wind = 0.02f },
    [MAT_LILY]        = { SRGB(0x4fbf62), SRGB(0x86b7c4), .ink = 0.6f },
    [MAT_STAR]        = { SRGB(0xffd35e), SRGB(0xe0a08c), .specular = 0.6f, .shininess = 40.0f, .rim = 1.0f, .emissive = 0.45f, .ink = 0.3f, .casts = 1 },
};

/* Motion is a row of coefficients, so animating an object is arithmetic, not
 * a switch: radians per second about Y, bob height, radians per second around
 * the island, radians per second of tumble, and squash-and-stretch amount. */
typedef struct Move {
    float spin, bob, orbit, tumble, bounce;
} Move;

static const Move MOVES[MOVE_COUNT] = {
    [MOVE_STILL]  = { 0 },
    [MOVE_SPIN]   = { .spin = 0.35f },
    [MOVE_GYRO]   = { .spin = 0.9f, .tumble = 0.45f },
    [MOVE_FLOAT]  = { .spin = 0.5f, .bob = 0.35f },
    [MOVE_ORBIT]  = { .spin = 1.1f, .bob = 0.55f, .orbit = 0.16f, .tumble = 0.7f },
    [MOVE_DRIFT]  = { .bob = 0.5f, .orbit = 0.012f },
    [MOVE_HOVER]  = { .bob = 0.8f, .orbit = 0.07f },
    [MOVE_BOUNCE] = { .bounce = 0.22f },
};

typedef struct Object {
    mu_vec3 position;
    mu_vec3 scale;
    float   yaw, tilt, phase;
    uint8_t mesh, material, move;
} Object;

typedef struct Mesh {
    Range    vertices, indices; /* in the device arena, read through binding 3 */
    uint32_t index_count;
    float    radius;            /* farthest vertex from the mesh origin, for culling */
} Mesh;

typedef struct Batch {
    uint32_t  mesh, first, count; /* a run of the sorted order that shares one mesh and one state */
    DrawState state;
} Batch;

#define PUT(...) objects[object_count++] = (Object){ __VA_ARGS__ }
/* the next float in [0, 1] from a mu_pcg32 */
#define ROLL(rng) ((float)mu_pcg32_next_u32(rng) * (1.0f / 4294967296.0f))

int main(int argc, char **argv)
{
    /* meshes that should look cut, not moulded: every face gets its own normal */
    static const uint8_t MESH_FLAT[MESH_COUNT] = {
        [MESH_PEDESTAL] = 1, [MESH_ROCK] = 1, [MESH_BOULDER] = 1, [MESH_ROOF] = 1, [MESH_OCTA] = 1, [MESH_ICOSA] = 1, [MESH_DODECA] = 1,
    };
    /* surface of revolution, bottom to top: the island's root, flanks, lip and lawn */
    static const mu_vec2 ISLAND_PROFILE[] = {
        { 0.0f, -7.6f }, { 0.6f, -6.5f }, { 1.2f, -5.4f }, { 1.8f, -4.5f }, { 3.0f, -3.3f }, { 4.2f, -2.4f }, { 5.4f, -1.4f },
        { 6.5f, -0.65f }, { 7.0f, -0.18f }, { 6.86f, 0.0f }, { 6.5f, 0.07f }, { 3.2f, 0.07f }, { 0.0f, 0.07f }, /* a level lawn at 0.07 */
    };
    static const mu_vec2 CAP_PROFILE[]  = { { 0.0f, 0.0f }, { 0.40f, 0.02f }, { 0.50f, 0.10f }, { 0.46f, 0.24f }, { 0.30f, 0.40f }, { 0.0f, 0.47f } };
    static const mu_vec2 STEM_PROFILE[] = { { 0.0f, 0.0f }, { 0.14f, 0.0f }, { 0.10f, 0.22f }, { 0.12f, 0.46f }, { 0.0f, 0.46f } };
    /* canopies and clouds are icospheres pushed into each other: mesh, centre, radius */
    static const struct { uint8_t mesh; float x, y, z, r; } BLOBS[] = {
        { MESH_CANOPY, 0.0f, 0.0f, 0.0f, 1.0f },    { MESH_CANOPY, 0.78f, 0.10f, 0.22f, 0.70f },  { MESH_CANOPY, -0.72f, 0.05f, -0.26f, 0.72f },
        { MESH_CANOPY, 0.10f, 0.58f, -0.10f, 0.72f }, { MESH_CANOPY, -0.16f, -0.02f, 0.76f, 0.62f }, { MESH_CANOPY, 0.22f, -0.06f, -0.80f, 0.60f },
        { MESH_CLOUD, -1.7f, 0.0f, 0.0f, 0.72f },   { MESH_CLOUD, -0.85f, 0.28f, 0.10f, 0.95f },  { MESH_CLOUD, 0.0f, 0.42f, 0.0f, 1.15f },
        { MESH_CLOUD, 0.95f, 0.22f, -0.10f, 0.95f }, { MESH_CLOUD, 1.75f, 0.0f, 0.05f, 0.70f },   { MESH_CLOUD, 0.35f, 0.0f, 0.62f, 0.80f },
        { MESH_CLOUD, -0.55f, -0.05f, -0.55f, 0.75f },
    };
    /* a branching trunk from a rewrite grammar: three boughs per fork */
    static const char *const       TREE_RULES[] = { "A=![&FA]/////[&FA]///////[&FA]" };
    static const mu_lsystem_config TREE_GRAMMAR = { .axiom = "FFA", .iterations = 3, .slices = 7, .radius = 0.17f, .length = 0.46f, .angle = 31.0f, .radius_shrink = 0.68f };
    /* where things stand on the lawn: x, z, size, turn */
    static const mu_vec4 BLOSSOM_TREES[] = { { -3.9f, -3.2f, 1.08f, 0.4f }, { 0.8f, -5.1f, 0.92f, 2.1f }, { -5.6f, 0.5f, 0.86f, 4.0f }, { 5.7f, 0.9f, 0.80f, 5.2f } };
    static const mu_vec4 LEAF_TREES[]    = { { -1.7f, -5.4f, 0.90f, 1.0f }, { 5.2f, -3.9f, 0.84f, 3.3f } };
    static const mu_vec4 PINES[]         = { { 2.7f, -5.2f, 1.10f, 0.0f }, { -5.5f, -1.9f, 0.95f, 0.0f }, { 6.0f, -1.5f, 0.85f, 0.0f }, { -5.2f, 3.0f, 0.72f, 0.0f }, { -2.9f, -4.6f, 0.78f, 0.0f } };
    /* scatter keeps clear of these: x, z, radius (plaza, pond, house, bottle) */
    static const mu_vec3 KEEP_OUT[] = { { 0.0f, 0.0f, 3.3f }, { -3.7f, 3.1f, 2.1f }, { 3.6f, -2.9f, 2.5f }, { 3.0f, 2.6f, 1.0f } };
    static const mu_vec3 POND = { -3.7f, 3.1f, 1.6f }; /* x, z, radius */
    /* the cottage: where it stands (x, z, turn) and what it is made of, in its own frame */
    static const mu_vec3 HOUSE = { 3.6f, -2.9f, -0.45f };
    static const struct { uint8_t mesh, material; mu_vec3 at, size; } HOUSE_PARTS[] = {
        { MESH_WALLS, MAT_WALL, { 0.0f, 0.66f, 0.0f }, { 2.0f, 1.25f, 1.6f } },
        { MESH_ROOF, MAT_ROOF, { 0.0f, 1.86f, 0.0f }, { 1.72f, 1.15f, 1.42f } },
        { MESH_BOX, MAT_WOOD, { -0.45f, 0.44f, 0.80f }, { 0.40f, 0.78f, 0.08f } },     /* door */
        { MESH_BOX, MAT_WOOD, { 0.62f, 2.05f, -0.30f }, { 0.28f, 0.85f, 0.28f } },     /* chimney */
        { MESH_BOX, MAT_WINDOW, { 0.42f, 0.78f, 0.80f }, { 0.42f, 0.40f, 0.06f } },    /* front window */
        { MESH_BOX, MAT_WINDOW, { -1.0f, 0.78f, 0.0f }, { 0.06f, 0.40f, 0.42f } },     /* side window */
    };
    /* floating gems: mesh, material, orbit radius, height, size */
    static const struct { uint8_t mesh, material; float radius, height, size; } GEMS[] = {
        { MESH_OCTA, MAT_GEM_PINK, 4.6f, 4.3f, 0.42f },   { MESH_ICOSA, MAT_GEM_CYAN, 5.6f, 3.4f, 0.38f },  { MESH_DODECA, MAT_GEM_VIOLET, 4.1f, 5.4f, 0.36f },
        { MESH_OCTA, MAT_GEM_AMBER, 6.4f, 2.6f, 0.34f },  { MESH_ICOSA, MAT_GEM_PINK, 7.2f, 4.9f, 0.30f },  { MESH_DODECA, MAT_GEM_CYAN, 3.4f, 6.3f, 0.30f },
        { MESH_STAR, MAT_STAR, 5.2f, 6.0f, 0.85f },
    };

    /* options */
    int         width = 1280, height = 720, msaa = 4, frame_limit = 0, start_seconds = 0, validate = 1, print_stats = 0, orbit_degrees = -12, elevation_degrees = 10;
    const char *shot_path = NULL;
    /* every number on the command line is clamped to what the program can do with it */
    struct { const char *flag; int *value, lowest, highest; } int_options[] = {
        { "--width", &width, 64, 8192 },          { "--height", &height, 64, 8192 }, { "--msaa", &msaa, 1, 8 },
        { "--frames", &frame_limit, 0, INT32_MAX }, { "--seconds", &start_seconds, 0, 36000 }, { "--validate", &validate, 0, 1 },
        { "--stats", &print_stats, 0, 1 },        { "--orbit", &orbit_degrees, -36000, 36000 }, { "--elevation", &elevation_degrees, 3, 68 },
    };

    /* platform */
    Platform     *platform;
    const MuInputState *input;
    Gpu          *g;
    GpuInfo       info;

    /* scene */
    Material      materials[MAT_COUNT];
    mu_shape      shapes[MESH_COUNT];
    mu_shape      part;
    Mesh          meshes[MESH_COUNT];
    Object        objects[MAX_OBJECTS];
    uint64_t      keys[MAX_OBJECTS];
    uint32_t      order[MAX_OBJECTS]; /* objects in batch order */
    Batch         batches[MAX_BATCHES];
    uint32_t      scene_first[MAX_BATCHES], scene_count[MAX_BATCHES], shadow_first[MAX_BATCHES], shadow_count[MAX_BATCHES];
    uint32_t      object_count = 0, batch_count = 0;
    mu_pcg32      rng;
    Range         geometry, petals, instances;
    mu_vec4       petal_seed[PETAL_COUNT];

    /* gpu objects */
    ShaderId      vs_scene, fs_scene, vs_shadow, vs_petal, fs_petal, vs_sky, fs_sky, vs_screen, fs_prefilter, fs_down, fs_up, fs_composite, cs_paper, cs_petals;
    Pipe          pipe_scene, pipe_shadow, pipe_petal, pipe_sky, pipe_prefilter, pipe_down, pipe_up, pipe_composite, pipe_paper, pipe_petals;
    Sampler       linear, mirror, nearest;
    Image         shadow_map, paper;
    Image         targets[TARGET_COUNT]; /* by TargetId */
    Image         bloom;                 /* one image: bloom level i is its mip i */
    Image         scene_targets[3], scene_resolves[3], outputs[2];
    ImageSub      source;
    Upload       *upload;
    Readback     *shot = NULL;
    Pass         *pass;
    DrawState     state_scene, state_shadow, state_sky, state_petal, state_add;

    /* per frame */
    ScenePush     scene;
    SkyPush       sky;
    FramePush     post;
    ComputePush   compute;
    Instance     *instance_data, record;
    mu_frustum    frustum;
    mu_mat4       view, proj, light_view, light_proj;
    mu_quat       turn, lean, rotation;
    mu_vec3       eye, target, light_eye, sun_dir, position, scale, up = { 0.0f, 1.0f, 0.0f };
    const Object *o;
    const Material *m;
    const Move   *move;
    uint64_t      instance_address;
    uint8_t      *pixels = NULL;
    float         seconds, dt, orbit, elevation, distance = 21.5f, shadow_reach = 11.5f;
    float         cycle, bob, angle, stretch, radius, x, z, size, squash, c, s, far_plane = 90.0f, near_plane = 0.5f;
    uint32_t      frame = 0, i, j, b, scene_total, shadow_total, output_count, source_width, source_height;
    uint32_t      window_width, window_height, bloom_levels = 0;
    bool          ink_on = true, bloom_on = true, hatch_on = true, paper_on = true, paused = false, capture, visible, clear, known, failed;

    /* ---- options ----------------------------------------------------------- */
    /* every option takes one value; anything else gets the usage line */
    for (i = 1; i < (uint32_t)argc; i += 2)
    {
        known = i + 1 < (uint32_t)argc && !strcmp(argv[i], "--shot");
        if (known)
            shot_path = argv[i + 1];
        for (j = 0; j < MU_ARRAY_COUNT(int_options) && i + 1 < (uint32_t)argc; ++j)
            if (!strcmp(argv[i], int_options[j].flag))
            {
                *int_options[j].value = MU_CLAMP(atoi(argv[i + 1]), int_options[j].lowest, int_options[j].highest);
                known                 = true;
            }
        if (!known)
        {
            fprintf(stderr, "usage: %s [--shot out.png] [--width N] [--height N] [--msaa 1|2|4|8] [--frames N]\n"
                            "       [--orbit DEGREES] [--elevation DEGREES] [--seconds N] [--validate 0|1] [--stats 0|1]\n", argv[0]);
            return 2;
        }
    }
    msaa = msaa >= 8 ? 8 : msaa >= 4 ? 4 : msaa >= 2 ? 2 : 1; /* a sample count is a power of two */
    /* a file to write means no window, and at least one frame to put in it */
    frame_limit = frame_limit ? frame_limit : shot_path ? 8 : INT32_MAX;

    /* ---- gpu, and a window unless we render to a file ------------------------ */
    g = gpu_create(&(GpuDesc){
        .name                     = "mu toon",
        .width                    = (uint32_t)width,
        .height                   = (uint32_t)height,
        .enable_validation        = validate != 0,
        .enable_graphics_profiler = true,
        .enable_pipeline_stats    = print_stats != 0,
        .vsync                    = true,
    });
    /* every device renders with 1 and 4 samples; 2 and 8 are this one's to offer */
    info = gpu_info(g);
    if (!(info.sample_counts & (uint32_t)msaa))
    {
        log_fatal("--msaa %d: this device offers only%s%s%s%s", msaa, info.sample_counts & SC_1 ? " 1" : "", info.sample_counts & SC_2 ? " 2" : "",
                  info.sample_counts & SC_4 ? " 4" : "", info.sample_counts & SC_8 ? " 8" : "");
        gpu_destroy(g);
        return 1;
    }
    /* With --shot the platform is headless: no display, and nothing is ever pressed. The loop below is the same. */
    platform = platform_create(&(PlatformDesc){ .title = "mu toon", .width = (uint32_t)width, .height = (uint32_t)height, .headless = shot_path != NULL });
    input    = platform_input(platform);
    /* the one setup step that reports instead of exiting: a device may refuse to present to a window system */
    if (!shot_path && !gpu_set_surface(g, platform_create_surface(platform, gpu_raw_instance(g))))
    {
        log_fatal("no window (%s). To render without one: %s --shot out.png", gpu_error(g), argv[0]);
        return 1;
    }
    info = gpu_info(g);
    log_info("%s, %ux%u, %dx MSAA, %s", info.name, info.width, info.height, msaa, shot_path ? "headless" : "window");

    /* ---- materials: sRGB swatches to linear light ---------------------------- */
    memcpy(materials, MATERIALS, sizeof materials);
    for (i = 0; i < MAT_COUNT; ++i)
        for (j = 0; j < 3; ++j)
        {
            materials[i].base[j]  = SRGB_TO_LINEAR(materials[i].base[j]);
            materials[i].shade[j] = SRGB_TO_LINEAR(materials[i].shade[j]);
        }

    /* ---- meshes: every one comes out of mu_shapes ---------------------------- */
    for (i = 0; i < MESH_COUNT; ++i)
        shapes[i] = mu_shape_empty();

    shapes[MESH_ISLAND] = mu_shape_lathe(ISLAND_PROFILE, (int)MU_ARRAY_COUNT(ISLAND_PROFILE), 96, 0);
    /* weather the rock: push the flanks in and out with value noise, more the
     * deeper they are, and leave the lawn level */
    for (i = 0; i < shapes[MESH_ISLAND].npoints; ++i)
    {
        float *p = shapes[MESH_ISLAND].verts[i].p;

        stretch = 1.0f + 0.20f * mu_noise_value3(p[0] * 0.7f, p[1] * 1.1f, p[2] * 0.7f, 7u) * mu_clamp(-p[1] * 1.6f, 0.0f, 1.0f)
                  + 0.07f * mu_noise_value3(p[0] * 2.3f, p[1] * 2.9f, p[2] * 2.3f, 11u) * mu_clamp(-p[1] * 3.0f, 0.0f, 1.0f);
        p[0] *= stretch;
        p[2] *= stretch;
    }

    shapes[MESH_PLAZA]      = mu_shape_cylinder(56, 1, 2.35f, 0.16f);
    shapes[MESH_PEDESTAL]   = mu_shape_prism(8, 0.78f, 1.0f);
    shapes[MESH_KNOT]       = mu_shape_trefoil_knot(240, 16, 1.25f, 0.21f);
    shapes[MESH_RING]       = mu_shape_torus(64, 10, 1.0f, 0.035f);
    shapes[MESH_TRUNK]      = mu_shape_lsystem(&TREE_GRAMMAR, TREE_RULES, 1);
    shapes[MESH_PINE_TRUNK] = mu_shape_cylinder(10, 1, 0.13f, 1.0f);
    shapes[MESH_ROCK]       = mu_shape_rock(1, 1.0f, 0.42f, 3u);
    shapes[MESH_BOULDER]    = mu_shape_rock(2, 1.0f, 0.55f, 9u);
    shapes[MESH_POND]       = mu_shape_disk(1.0f, 64, 1);
    shapes[MESH_WALLS]      = mu_shape_rounded_box(4, 1.0f, 0.09f);
    shapes[MESH_ROOF]       = mu_shape_cone(4, 1, 1.0f, 1.0f);
    shapes[MESH_BOX]        = mu_shape_cube(1, 1.0f);
    shapes[MESH_CAP]        = mu_shape_lathe(CAP_PROFILE, (int)MU_ARRAY_COUNT(CAP_PROFILE), 28, 0);
    shapes[MESH_STEM]       = mu_shape_lathe(STEM_PROFILE, (int)MU_ARRAY_COUNT(STEM_PROFILE), 16, 0);
    shapes[MESH_OCTA]       = mu_shape_octahedron(1.0f);
    shapes[MESH_ICOSA]      = mu_shape_icosahedron(1.0f);
    shapes[MESH_DODECA]     = mu_shape_dodecahedron(1.0f);
    shapes[MESH_BALL]       = mu_shape_icosphere(2, 1.0f);
    shapes[MESH_MOBIUS]     = mu_shape_mobius(120, 8, 1.0f, 0.55f);
    shapes[MESH_SHELL]      = mu_shape_seashell(28, 110, 3, 0.012f, 0.2f, 1.6f);
    shapes[MESH_KLEIN]      = mu_shape_klein_bottle(72, 36, 0.24f);
    shapes[MESH_SPRING]     = mu_shape_spring(200, 8, 0.22f, 0.13f, 5.0f, 0.035f);
    shapes[MESH_POST]       = mu_shape_capsule(10, 4, 0.045f, 1.0f);
    shapes[MESH_STAR]       = mu_shape_supershape(&(mu_shape_superformula){ 5.0f, 0.3f, 0.4f, 0.4f, 2.0f, 0.5f, 0.5f, 0.5f }, 80, 40, 1.0f);

    mu_shape_rotate(&shapes[MESH_ROOF], up, mu_radians(45.0f)); /* a four-sided cone, turned square to the walls */
    mu_shape_flip(&shapes[MESH_POND]);                          /* mu_shape_disk winds its fan facing down */
    /* a pine is three cones on a stick */
    for (i = 0; i < 3; ++i)
    {
        part = mu_shape_cone(16, 1, 1.0f - 0.24f * (float)i, 1.35f - 0.15f * (float)i);
        mu_shape_translate(&part, (mu_vec3){ 0.0f, 1.25f + 0.78f * (float)i, 0.0f });
        mu_shape_merge(&shapes[MESH_PINE], &part);
        mu_shape_free(&part);
    }
    for (i = 0; i < MU_ARRAY_COUNT(BLOBS); ++i)
    {
        part = mu_shape_icosphere(2, BLOBS[i].r);
        mu_shape_translate(&part, (mu_vec3){ BLOBS[i].x, BLOBS[i].y, BLOBS[i].z });
        mu_shape_merge(&shapes[BLOBS[i].mesh], &part);
        mu_shape_free(&part);
    }

    /* normals, bounds, and the bytes into the device arena in one upload batch */
    upload = upload_begin(g);
    for (i = 0; i < MESH_COUNT; ++i)
    {
        mu_shape *shape = &shapes[i];

        if (MESH_FLAT[i])
        {
            mu_shape_unweld(shape);
            mu_shape_compute_normals_flat(shape);
        }
        else
            mu_shape_compute_normals(shape);

        meshes[i].radius = 0.0f;
        for (j = 0; j < shape->npoints; ++j)
            meshes[i].radius = MU_MAX(meshes[i].radius, mu_vec3_length(shape->verts[j].p));
        meshes[i].index_count = shape->ntriangles * 3;
        meshes[i].vertices    = arena_alloc(g, ARENA_DEVICE, shape->npoints * sizeof *shape->verts, 16);
        meshes[i].indices     = arena_alloc(g, ARENA_DEVICE, meshes[i].index_count * sizeof *shape->triangles, 16);
        upload_buffer(upload, meshes[i].vertices, (DataSpan){ shape->verts, (uint32_t)meshes[i].vertices.size });
        upload_buffer(upload, meshes[i].indices, (DataSpan){ shape->triangles, (uint32_t)meshes[i].indices.size });
        mu_shape_free(shape);
    }

    /* petals: 16 bytes each, simulated on the GPU from here on */
    mu_pcg32_init(&rng, 2026u, 11u);
    for (i = 0; i < PETAL_COUNT; ++i)
    {
        petal_seed[i][0] = (ROLL(&rng) * 2.0f - 1.0f) * 9.0f;
        petal_seed[i][1] = ROLL(&rng) * 9.0f - 1.0f;
        petal_seed[i][2] = (ROLL(&rng) * 2.0f - 1.0f) * 9.0f;
        petal_seed[i][3] = ROLL(&rng);
    }
    petals = arena_alloc(g, ARENA_DEVICE, sizeof petal_seed, 16);
    upload_buffer(upload, petals, BYTE_SPAN(petal_seed));
    upload_end(g, upload);
    /* everything the vertex shaders pull is one stretch of the arena */
    geometry = (Range){ meshes[0].vertices.buffer, meshes[0].vertices.offset, petals.offset + petals.size - meshes[0].vertices.offset };

    /* ---- the scene: objects placed by hand, by table and by dice --------------- */
    PUT({ 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, .mesh = MESH_ISLAND, .material = MAT_GRASS);
    PUT({ 0.0f, 0.13f, 0.0f }, { 1.0f, 1.0f, 1.0f }, .mesh = MESH_PLAZA, .material = MAT_STONE);
    PUT({ 0.0f, 0.70f, 0.0f }, { 1.0f, 1.0f, 1.0f }, .yaw = 0.39f, .mesh = MESH_PEDESTAL, .material = MAT_STONE);
    PUT({ 0.0f, 2.75f, 0.0f }, { 1.0f, 1.0f, 1.0f }, .mesh = MESH_KNOT, .material = MAT_GOLD, .move = MOVE_SPIN);
    PUT({ 0.0f, 2.75f, 0.0f }, { 2.15f, 2.15f, 2.15f }, .tilt = 1.2f, .mesh = MESH_RING, .material = MAT_BRASS, .move = MOVE_GYRO);
    PUT({ 0.0f, 2.75f, 0.0f }, { 2.45f, 2.45f, 2.45f }, .yaw = 1.6f, .tilt = 0.5f, .phase = 1.7f, .mesh = MESH_RING, .material = MAT_BRASS, .move = MOVE_GYRO);

    /* trees: an L-system trunk under a canopy of merged spheres */
    for (i = 0; i < MU_ARRAY_COUNT(BLOSSOM_TREES) + MU_ARRAY_COUNT(LEAF_TREES); ++i)
    {
        const float *tree = i < MU_ARRAY_COUNT(BLOSSOM_TREES) ? BLOSSOM_TREES[i] : LEAF_TREES[i - MU_ARRAY_COUNT(BLOSSOM_TREES)];
        uint8_t      leaf = i < MU_ARRAY_COUNT(BLOSSOM_TREES) ? MAT_BLOSSOM : (i & 1 ? MAT_LEAF : MAT_LEAF_DEEP);

        size = tree[2];
        PUT({ tree[0], 0.0f, tree[1] }, { size, size, size }, .yaw = tree[3], .phase = tree[3], .mesh = MESH_TRUNK, .material = MAT_BARK);
        PUT({ tree[0], 2.35f * size, tree[1] }, { 1.35f * size, 1.12f * size, 1.35f * size }, .yaw = tree[3], .phase = tree[3] * 3.0f, .mesh = MESH_CANOPY, .material = leaf);
    }
    for (i = 0; i < MU_ARRAY_COUNT(PINES); ++i)
    {
        size = PINES[i][2];
        PUT({ PINES[i][0], 0.5f * size, PINES[i][1] }, { size, size, size }, .mesh = MESH_PINE_TRUNK, .material = MAT_BARK);
        PUT({ PINES[i][0], 0.0f, PINES[i][1] }, { size * 0.82f, size, size * 0.82f }, .phase = (float)i, .mesh = MESH_PINE, .material = MAT_PINE);
    }

    /* the cottage: walls, roof, door, chimney, and two lit windows */
    /* Parts are placed in the cottage's own frame (x across the front, z out
     * of the door) and turned with it: HOUSE_PARTS is the whole building. */
    for (i = 0; i < MU_ARRAY_COUNT(HOUSE_PARTS); ++i)
        PUT({ HOUSE[0] + cosf(HOUSE[2]) * HOUSE_PARTS[i].at[0] + sinf(HOUSE[2]) * HOUSE_PARTS[i].at[2], HOUSE_PARTS[i].at[1],
              HOUSE[1] - sinf(HOUSE[2]) * HOUSE_PARTS[i].at[0] + cosf(HOUSE[2]) * HOUSE_PARTS[i].at[2] },
            { HOUSE_PARTS[i].size[0], HOUSE_PARTS[i].size[1], HOUSE_PARTS[i].size[2] }, .yaw = HOUSE[2], .mesh = HOUSE_PARTS[i].mesh, .material = HOUSE_PARTS[i].material);

    /* the pond inside a stone kerb, with lily pads: the same disk, smaller and green */
    PUT({ POND[0], 0.085f, POND[1] }, { POND[2], 1.0f, POND[2] }, .mesh = MESH_POND, .material = MAT_WATER);
    PUT({ POND[0], 0.09f, POND[1] }, { POND[2], 2.6f, POND[2] }, .mesh = MESH_RING, .material = MAT_STONE);
    for (i = 0; i < 4; ++i)
    {
        angle = 0.6f + 1.9f * (float)i;
        PUT({ POND[0] + POND[2] * (0.34f + 0.11f * (float)i) * cosf(angle), 0.092f, POND[1] + POND[2] * (0.34f + 0.11f * (float)i) * sinf(angle) },
            { 0.30f - 0.035f * (float)i, 1.0f, 0.30f - 0.035f * (float)i }, .yaw = angle, .mesh = MESH_POND, .material = MAT_LILY);
    }
    /* the shell, the bottle and the ribbon: the shapes no (u,v) square gives you */
    PUT({ -0.9f, 0.08f, 4.3f }, { 0.95f, 0.95f, 0.95f }, .yaw = 2.2f, .mesh = MESH_SHELL, .material = MAT_SHELL);
    PUT({ 3.0f, 0.2f, 2.6f }, { 0.55f, 0.32f, 0.55f }, .mesh = MESH_PEDESTAL, .material = MAT_STONE);
    /* the figure-8 bottle lies flat as generated: stood on edge it is a ring you can read */
    PUT({ 3.0f, 1.12f, 2.6f }, { 0.95f, 0.95f, 0.95f }, .yaw = 0.7f, .tilt = 1.5708f, .mesh = MESH_KLEIN, .material = MAT_GLASS, .move = MOVE_SPIN);
    PUT({ -5.6f, 4.9f, -2.6f }, { 1.1f, 1.1f, 1.1f }, .tilt = 0.9f, .phase = 0.3f, .mesh = MESH_MOBIUS, .material = MAT_RIBBON, .move = MOVE_FLOAT);
    /* a gem on a spring, by the door: squash and stretch from an elastic ease */
    PUT({ 1.5f, 0.08f, -2.2f }, { 1.0f, 1.0f, 1.0f }, .phase = 0.2f, .mesh = MESH_SPRING, .material = MAT_STEEL, .move = MOVE_BOUNCE);
    PUT({ 1.5f, 0.98f, -2.2f }, { 0.24f, 0.24f, 0.24f }, .phase = 0.2f, .mesh = MESH_ICOSA, .material = MAT_GEM_VIOLET, .move = MOVE_BOUNCE);

    /* four lamps around the plaza */
    for (i = 0; i < 4; ++i)
    {
        angle = 0.785f + 1.571f * (float)i;
        PUT({ 2.85f * cosf(angle), 0.6f, 2.85f * sinf(angle) }, { 1.0f, 1.0f, 1.0f }, .mesh = MESH_POST, .material = MAT_STEEL);
        PUT({ 2.85f * cosf(angle), 1.25f, 2.85f * sinf(angle) }, { 0.15f, 0.15f, 0.15f }, .phase = (float)i, .mesh = MESH_BALL, .material = MAT_LAMP);
    }

    /* dice: rocks, mushrooms and flowers wherever the lawn is free */
    for (i = 0; i < 46; ++i)
    {
        do
        {
            radius = 0.8f + 5.4f * sqrtf(ROLL(&rng));
            angle  = ROLL(&rng) * 6.2832f;
            x      = radius * cosf(angle);
            z      = radius * sinf(angle);
            clear  = true;
            for (j = 0; j < MU_ARRAY_COUNT(KEEP_OUT); ++j)
                clear = clear && (x - KEEP_OUT[j][0]) * (x - KEEP_OUT[j][0]) + (z - KEEP_OUT[j][1]) * (z - KEEP_OUT[j][1]) > KEEP_OUT[j][2] * KEEP_OUT[j][2];
        } while (!clear);
        size = 0.6f + 0.6f * ROLL(&rng);

        if (i < 9)
            PUT({ x, 0.12f * size, z }, { 0.42f * size, 0.30f * size, 0.36f * size }, .yaw = angle * 3.0f, .mesh = MESH_ROCK, .material = MAT_ROCK);
        else if (i < 17)
        {
            PUT({ x, 0.05f, z }, { size * 0.5f, size * 0.6f, size * 0.5f }, .mesh = MESH_STEM, .material = MAT_STEM);
            PUT({ x, 0.05f + 0.25f * size, z }, { size * 0.58f, size * 0.58f, size * 0.58f }, .yaw = angle, .mesh = MESH_CAP, .material = MAT_CAP);
        }
        else
            /* a flower is a five-pointed supershape, flattened, nodding in the wind */
            PUT({ x, 0.12f + 0.05f * size, z }, { 0.27f * size, 0.13f * size, 0.27f * size }, .yaw = angle, .phase = angle, .mesh = MESH_STAR, .material = i & 1 ? MAT_FLOWER_SUN : MAT_FLOWER_ROSE);
    }

    /* in the air: gems on orbits, boulders adrift, clouds, fireflies */
    for (i = 0; i < MU_ARRAY_COUNT(GEMS); ++i)
        PUT({ GEMS[i].radius * cosf(0.9f * (float)i), GEMS[i].height, GEMS[i].radius * sinf(0.9f * (float)i) }, { GEMS[i].size, GEMS[i].size * 1.25f, GEMS[i].size },
            .tilt = 0.35f, .phase = 0.37f * (float)i, .mesh = GEMS[i].mesh, .material = GEMS[i].material, .move = MOVE_ORBIT);
    for (i = 0; i < 5; ++i)
    {
        angle = 1.1f + 1.257f * (float)i;
        size  = 0.55f + 0.22f * (float)(i % 3);
        PUT({ (9.4f + (float)(i % 2)) * cosf(angle), -1.6f + 1.1f * (float)(i % 3), (9.4f + (float)(i % 2)) * sinf(angle) }, { size * 1.3f, size, size * 1.15f },
            .yaw = angle, .tilt = 0.2f, .phase = 0.5f * (float)i, .mesh = MESH_BOULDER, .material = MAT_ROCK, .move = MOVE_DRIFT);
    }
    for (i = 0; i < 9; ++i)
    {
        angle = 0.4f + 0.698f * (float)i;
        size  = 1.5f + 0.7f * (float)(i % 3);
        PUT({ (15.0f + 4.0f * (float)(i % 3)) * cosf(angle), -6.0f + 3.2f * (float)(i % 4), (15.0f + 4.0f * (float)(i % 3)) * sinf(angle) }, { size, size * 0.72f, size },
            .yaw = angle + 1.57f, .phase = 0.7f * (float)i, .mesh = MESH_CLOUD, .material = MAT_CLOUD, .move = MOVE_DRIFT);
    }
    for (i = 0; i < 26; ++i)
    {
        radius = 1.5f + 5.5f * ROLL(&rng);
        angle  = ROLL(&rng) * 6.2832f;
        PUT({ radius * cosf(angle), 0.9f + 2.6f * ROLL(&rng), radius * sinf(angle) }, { 0.045f, 0.045f, 0.045f },
            .phase = angle * 2.0f, .mesh = MESH_BALL, .material = MAT_FIREFLY, .move = MOVE_HOVER);
    }
    MU_ASSERT(object_count <= MAX_OBJECTS);

    /* ---- shaders, pipes, samplers ------------------------------------------- */
    vs_scene     = shader_load(g, &(ShaderDesc){ .path = "shaders/scene.slang", .entry = "vs_scene", .stage = STAGE_VERTEX });
    fs_scene     = shader_load(g, &(ShaderDesc){ .path = "shaders/scene.slang", .entry = "fs_scene", .stage = STAGE_FRAGMENT });
    vs_shadow    = shader_load(g, &(ShaderDesc){ .path = "shaders/scene.slang", .entry = "vs_shadow", .stage = STAGE_VERTEX });
    vs_petal     = shader_load(g, &(ShaderDesc){ .path = "shaders/scene.slang", .entry = "vs_petal", .stage = STAGE_VERTEX });
    fs_petal     = shader_load(g, &(ShaderDesc){ .path = "shaders/scene.slang", .entry = "fs_petal", .stage = STAGE_FRAGMENT });
    vs_sky       = shader_load(g, &(ShaderDesc){ .path = "shaders/sky.slang", .entry = "vs_sky", .stage = STAGE_VERTEX });
    fs_sky       = shader_load(g, &(ShaderDesc){ .path = "shaders/sky.slang", .entry = "fs_sky", .stage = STAGE_FRAGMENT });
    vs_screen    = shader_load(g, &(ShaderDesc){ .path = "shaders/frame.slang", .entry = "vs_screen", .stage = STAGE_VERTEX });
    fs_prefilter = shader_load(g, &(ShaderDesc){ .path = "shaders/frame.slang", .entry = "fs_bloom_prefilter", .stage = STAGE_FRAGMENT });
    fs_down      = shader_load(g, &(ShaderDesc){ .path = "shaders/frame.slang", .entry = "fs_bloom_down", .stage = STAGE_FRAGMENT });
    fs_up        = shader_load(g, &(ShaderDesc){ .path = "shaders/frame.slang", .entry = "fs_bloom_up", .stage = STAGE_FRAGMENT });
    fs_composite = shader_load(g, &(ShaderDesc){ .path = "shaders/frame.slang", .entry = "fs_composite", .stage = STAGE_FRAGMENT });
    cs_paper     = shader_load(g, &(ShaderDesc){ .path = "shaders/compute.slang", .entry = "cs_paper", .stage = STAGE_COMPUTE });
    cs_petals    = shader_load(g, &(ShaderDesc){ .path = "shaders/compute.slang", .entry = "cs_petals", .stage = STAGE_COMPUTE });

    pipe_scene     = pipe_create(g, &(PipeDesc){ .name = "scene", .vertex = vs_scene, .fragment = fs_scene });
    pipe_shadow    = pipe_create(g, &(PipeDesc){ .name = "shadow", .vertex = vs_shadow }); /* no fragment shader: depth only */
    pipe_petal     = pipe_create(g, &(PipeDesc){ .name = "petal", .vertex = vs_petal, .fragment = fs_petal });
    pipe_sky       = pipe_create(g, &(PipeDesc){ .name = "sky", .vertex = vs_sky, .fragment = fs_sky });
    pipe_prefilter = pipe_create(g, &(PipeDesc){ .name = "bloom prefilter", .vertex = vs_screen, .fragment = fs_prefilter });
    pipe_down      = pipe_create(g, &(PipeDesc){ .name = "bloom down", .vertex = vs_screen, .fragment = fs_down });
    pipe_up        = pipe_create(g, &(PipeDesc){ .name = "bloom up", .vertex = vs_screen, .fragment = fs_up });
    pipe_composite = pipe_create(g, &(PipeDesc){ .name = "composite", .vertex = vs_screen, .fragment = fs_composite });
    pipe_paper     = pipe_create(g, &(PipeDesc){ .name = "paper", .compute = cs_paper });
    pipe_petals    = pipe_create(g, &(PipeDesc){ .name = "petals", .compute = cs_petals });

    linear  = sampler_linear_clamp(g);
    nearest = sampler_nearest_clamp(g);
    mirror  = sampler_create(g, &(SamplerDesc){ .min_filter = FILT_LINEAR, .mag_filter = FILT_LINEAR, .addr_u = ADDR_MIRROR, .addr_v = ADDR_MIRROR, .addr_w = ADDR_MIRROR });

    shadow_map = image_create(g, &(ImageDesc){ .name = "shadow map", .width = SHADOW_SIZE, .height = SHADOW_SIZE, .format = FMT_D32F, .usage = IMG_ATTACH | IMG_SAMPLED });
    paper      = image_storage(g, PAPER_SIZE, PAPER_SIZE, FMT_RGBA8_UNORM);

    /* The scene uses the reverse-Z presets as they are. The shadow map is
     * standard depth (clear 1, LESS) and draws back faces, so lit surfaces
     * never fight their own shadow. The sky sits on the far plane, where
     * reverse-Z depth is 0: GEQUAL passes only where nothing was drawn. */
    state_scene                = draw_state_opaque((uint8_t)msaa, CULL_BACK);
    state_shadow               = draw_state_depth(SC_1);
    state_shadow.depth_compare = CMP_LESS;
    state_shadow.cull          = CULL_FRONT;
    state_sky                  = draw_state_opaque((uint8_t)msaa, CULL_NONE);
    state_sky.depth_compare    = CMP_GEQUAL;
    state_sky.depth_write      = 0;
    state_petal                = draw_state_opaque((uint8_t)msaa, CULL_NONE);
    state_petal.write_mask     = 0x000f; /* colour only: petals leave no normal, depth or id, so they get no ink */
    state_add                  = (DrawState){ .blend = BLEND_ADD, .write_mask = 0xffff };

    /* ---- targets: no size of their own; vk.c makes them again when the frame's changes --- */
    for (i = 0; i < TARGET_COUNT; ++i)
        targets[i] = image_create(g, &(ImageDesc){
            .name      = TARGETS[i].name,
            .format    = TARGETS[i].format,
            .usage     = TARGETS[i].multisampled ? IMG_ATTACH : IMG_ATTACH | IMG_SAMPLED | IMG_XFER_SRC,
            .samples   = TARGETS[i].multisampled ? (SampleCount)msaa : SC_1,
            .transient = TARGETS[i].multisampled,
        });
    /* at 1x there is nothing to resolve: the scene draws the final targets directly */
    memcpy(scene_targets, &targets[msaa > 1 ? TARGET_MS_COLOR : TARGET_COLOR], sizeof scene_targets);
    memcpy(scene_resolves, msaa > 1 ? &targets[TARGET_COLOR] : (Image[3]){ IMAGE_NIL, IMAGE_NIL, IMAGE_NIL }, sizeof scene_resolves);
    /* bloom starts at half the frame, and of its levels it has the ones that size has */
    bloom = image_create(g, &(ImageDesc){ .name = "bloom", .frame_shift = 1, .mips = BLOOM_LEVELS, .format = FMT_RGBA16F, .usage = IMG_ATTACH | IMG_SAMPLED });

    /* ---- batches: sort once by the key the API hands out ----------------------- */
    /* Same pipe and state end up adjacent, and inside a state the same mesh:
     * one draw per batch, instanced. The objects stay where they are; `order`
     * is the sequence to walk them in. */
    for (i = 0; i < object_count; ++i)
    {
        state_scene.cull = materials[objects[i].material].two_sided ? CULL_NONE : CULL_BACK;
        keys[i]          = draw_sort_key(pipe_scene.index, state_scene) << 8 | objects[i].mesh;
        order[i]         = i;
    }
    mu_sort_indices_u64(keys, order, object_count);
    for (i = 0; i < object_count; ++i)
    {
        if (i == 0 || keys[order[i]] != keys[order[i - 1]])
        {
            MU_ASSERT(batch_count < MAX_BATCHES);
            state_scene.cull       = materials[objects[order[i]].material].two_sided ? CULL_NONE : CULL_BACK;
            batches[batch_count++] = (Batch){ .mesh = objects[order[i]].mesh, .first = i, .state = state_scene };
        }
        batches[batch_count - 1].count++;
    }
    log_info("%u objects in %u batches, %u meshes, %llu bytes of geometry", object_count, batch_count, (unsigned)MESH_COUNT, (unsigned long long)geometry.size);

    /* ---- paper: baked once, outside any frame; it is submitted ahead of the first one --- */
    memset(&compute, 0, sizeof compute);
    compute.storage_id = image_storage_id(g, paper);
    compute.size       = PAPER_SIZE;
    pass = begin_pass(g, &(PassDesc){ .name = "paper", .kind = PASS_COMPUTE, .pipe = pipe_paper, .images = { use_storage(paper) }, .image_count = 1 });
    dispatch(pass, BYTE_SPAN(compute), PAPER_SIZE / 8, PAPER_SIZE / 8, 1);
    end_pass(pass);

    /* ---- frames ------------------------------------------------------------ */
    seconds   = (float)start_seconds;
    orbit     = mu_radians((float)orbit_degrees);
    elevation = mu_radians((float)elevation_degrees);
    mu_vec3_normalize(sun_dir, (mu_vec3){ -0.58f, 0.52f, 0.60f });
    while (frame < (uint32_t)frame_limit && platform_poll(platform))
    {
        /* -- input: platform_poll() put what happened since the last frame into `input` -- */
        /* a file render advances exactly one 60 Hz step per frame, so it is repeatable */
        dt         = shot_path ? 1.0f / 60.0f : input->dt;
        ink_on    ^= mu_input_key_pressed(input, MU_KEY_O);
        bloom_on  ^= mu_input_key_pressed(input, MU_KEY_B);
        hatch_on  ^= mu_input_key_pressed(input, MU_KEY_H);
        paper_on  ^= mu_input_key_pressed(input, MU_KEY_P);
        paused    ^= mu_input_key_pressed(input, MU_KEY_SPACE);
        dt         = paused ? 0.0f : dt;
        seconds   += dt;
        orbit     += (float)mu_input_mouse_down(input, MU_MOUSE_LEFT) * input->mouse_dx * -0.006f
                     + ((float)mu_input_key_down(input, MU_KEY_RIGHT) - (float)mu_input_key_down(input, MU_KEY_LEFT)) * input->dt * 0.9f + dt * 0.035f;
        elevation  = mu_clamp(elevation + (float)mu_input_mouse_down(input, MU_MOUSE_LEFT) * input->mouse_dy * 0.004f
                                  + ((float)mu_input_key_down(input, MU_KEY_UP) - (float)mu_input_key_down(input, MU_KEY_DOWN)) * input->dt * 0.6f, 0.04f, 1.2f);
        capture    = mu_input_key_pressed(input, MU_KEY_F2) || (shot_path && frame + 1 == (uint32_t)frame_limit);
        if (mu_input_key_pressed(input, MU_KEY_ESCAPE))
            break;

        /* a Wayland window is as large as the frames it is shown: the backend is told what size the window asks for */
        platform_size(platform, &window_width, &window_height);
        gpu_resize(g, window_width, window_height);
        if (!frame_begin(g))
        {
            mu_sleep_ms(16); /* minimised: nothing to draw into */
            continue;
        }

        /* -- the frame's size. The targets have it already: frame_begin() made them again if it changed -- */
        info         = gpu_info(g);
        /* a mip chain ends at 1x1: under 16 pixels the frame has fewer than BLOOM_LEVELS levels, and so has bloom */
        bloom_levels = MU_MIN((uint32_t)BLOOM_LEVELS, 32u - mu_leading_zeroes_u32(MU_MAX(MU_MAX(info.width, info.height) >> 1, 1u)));

        /* -- camera and sun: what every scene draw shares, built in the block the shaders read -- */
        memset(&scene, 0, sizeof scene);
        mu_vec3_make(target, 0.0f, 1.0f, 0.0f);
        mu_vec3_make(eye, sinf(orbit) * cosf(elevation) * distance, target[1] + sinf(elevation) * distance, cosf(orbit) * cosf(elevation) * distance);
        mu_mat4_look_at(view, eye, target, up);
        /* far and near swapped: reverse-Z, the depth convention of the draw_state presets */
        mu_mat4_perspective_zo(proj, mu_radians(36.0f), (float)info.width / (float)info.height, far_plane, near_plane);
        mu_mat4_mul(proj, view, scene.view_proj);
        mu_frustum_from_view_proj(&frustum, scene.view_proj);
        mu_vec3_to_vec4(scene.camera, eye, seconds);

        mu_vec3_scale(light_eye, sun_dir, 40.0f);
        mu_mat4_look_at(light_view, light_eye, (mu_vec3){ 0.0f, 0.0f, 0.0f }, up);
        mu_mat4_ortho(light_proj, -shadow_reach, shadow_reach, -shadow_reach, shadow_reach, 1.0f, 80.0f);
        /* mu_mat4_ortho is OpenGL's, z in [-1,1]; Vulkan clips to [0,1]: z' = (z + w) / 2 */
        for (i = 0; i < 4; ++i)
            light_proj[4 * i + 2] = 0.5f * light_proj[4 * i + 2] + 0.5f * light_proj[4 * i + 3];
        mu_mat4_mul(light_proj, light_view, scene.light_view_proj);
        mu_vec3_to_vec4(scene.sun, sun_dir, 2.0f * shadow_reach / (float)SHADOW_SIZE);
        mu_vec4_make(scene.sun_color, 1.06f, 0.98f, 0.86f, 1.0f / (float)SHADOW_SIZE);
        mu_vec4_make(scene.sky_color, 0.55f, 0.72f, 1.0f, (float)hatch_on);
        scene.shadow_map     = image_sampled_id(g, shadow_map);
        scene.shadow_sampler = sampler_id(g, nearest);

        /* -- instances: animate, cull, and write straight into the transient arena -- */
        /* Two lists in one allocation: what the camera sees, then what casts a
         * shadow. Objects are walked in batch order, so each list is in it too. */
        instances        = arena_alloc(g, ARENA_TRANSIENT, 2 * object_count * sizeof(Instance), 16);
        instance_data    = (Instance *)((uint8_t *)buffer_info(g, instances.buffer).mapped + instances.offset);
        instance_address = buffer_info(g, instances.buffer).address + instances.offset;
        scene_total      = 0;
        shadow_total     = object_count;
        for (b = 0; b < batch_count; ++b)
        {
            scene_first[b]  = scene_total;
            shadow_first[b] = shadow_total;
            for (i = batches[b].first; i < batches[b].first + batches[b].count; ++i)
            {
                o    = &objects[order[i]];
                m    = &materials[o->material];
                move = &MOVES[o->move];

                /* every motion is the same five terms; MOVES decides how much of each */
                cycle   = seconds * 0.21f + o->phase;
                bob     = mu_ease_in_out_sine(mu_ease_pingpong(cycle)) - 0.5f;
                angle   = seconds * move->orbit;
                c       = cosf(angle);
                s       = sinf(angle);
                /* squash and stretch: an elastic settle once per beat, volume kept */
                stretch = 1.0f + move->bounce * (mu_ease_out_elastic(mu_ease_loop(seconds * 0.45f + o->phase)) - 1.0f + 0.35f * sinf(seconds * 5.0f + o->phase) * (1.0f - mu_ease_loop(seconds * 0.45f + o->phase)));
                squash  = 1.0f / sqrtf(stretch);
                mu_vec3_make(position, c * o->position[0] + s * o->position[2], o->position[1] * (o->move == MOVE_BOUNCE ? stretch : 1.0f) + move->bob * bob, c * o->position[2] - s * o->position[0]);
                mu_vec3_make(scale, o->scale[0] * squash, o->scale[1] * stretch, o->scale[2] * squash);
                mu_quat_axis_angle(turn, up, o->yaw + seconds * move->spin - angle);
                mu_quat_axis_angle(lean, (mu_vec3){ 1.0f, 0.0f, 0.0f }, o->tilt + seconds * move->tumble);
                mu_quat_mul(rotation, turn, lean);
                mu_mat4_trs(record.model, position, rotation, scale);

                radius  = meshes[o->mesh].radius * MU_MAX(scale[0], MU_MAX(scale[1], scale[2])) + m->wind;
                visible = mu_frustum_test_sphere(&frustum, position, radius);

                /* Built on the stack and stored to both lists; a list keeps the
                 * record only if its counter moves on. The mapping is written,
                 * never read back. */
                mu_vec3_to_vec4(record.base, m->base, (float)m->kind);
                mu_vec3_to_vec4(record.shade, m->shade, m->hatch);
                mu_vec4_make(record.surface, m->specular, m->shininess, m->rim, m->emissive);
                mu_vec4_make(record.extra, (float)(i + 1), m->wind, o->phase, m->ink);
                instance_data[scene_total]  = record;
                instance_data[shadow_total] = record;
                scene_total  += visible;
                shadow_total += m->casts;
            }
            scene_count[b]  = scene_total - scene_first[b];
            shadow_count[b] = shadow_total - shadow_first[b];
        }

        /* -- petals: compute, in place in the device arena ------------------------ */
        memset(&compute, 0, sizeof compute);
        mu_vec4_make(compute.wind, 0.55f, 0.0f, 0.25f, seconds);
        mu_vec4_make(compute.volume, 9.5f, 8.0f, 9.5f, -1.5f);
        mu_vec4_make(compute.origin, 0.0f, 0.0f, 0.0f, dt);
        compute.petal_offset = (uint32_t)petals.offset;
        compute.petal_count  = PETAL_COUNT;
        pass = begin_pass(g, &(PassDesc){ .name = "petals", .kind = PASS_COMPUTE, .pipe = pipe_petals, .buffers = { use_rw(petals) }, .buffer_count = 1 });
        pass_note(pass, (PassNote){ .workgroup = { 64, 1, 1 } }); /* cs_petals; --stats 1 prints it beside the pass's time */
        dispatch(pass, BYTE_SPAN(compute), (PETAL_COUNT + 63) / 64, 1, 1);
        end_pass(pass);

        /* -- shadow: depth from the sun ------------------------------------------- */
        pass = begin_pass(g, &(PassDesc){
            .name         = "shadow",
            .pipe         = pipe_shadow,
            .state        = state_shadow,
            .depth        = attach_clear_depth(shadow_map, 1.0f),
            .has_depth    = true,
            .buffers      = { use_read(geometry) },
            .buffer_count = 1,
        });
        for (b = 0; b < batch_count; ++b)
        {
            /* a mesh with no inside has no back faces to rely on */
            state_shadow.cull   = batches[b].state.cull == CULL_NONE ? CULL_NONE : CULL_FRONT;
            scene.instances     = instance_address + shadow_first[b] * sizeof(Instance);
            scene.vertex_offset = (uint32_t)meshes[batches[b].mesh].vertices.offset;
            scene.index_offset  = (uint32_t)meshes[batches[b].mesh].indices.offset;
            set_state(pass, state_shadow);
            draw_indexed(pass, BYTE_SPAN(scene), meshes[batches[b].mesh].index_count, shadow_count[b]);
        }
        end_pass(pass);

        /* -- scene: three multisampled targets, resolved by the pass ----------------- */
        /* The sky covers every pixel the geometry leaves, so nothing needs clearing
         * but depth (reverse-Z: 0 is the far plane). */
        pass = begin_pass(g, &(PassDesc){
            .name   = "scene",
            .pipe   = pipe_scene,
            .state  = batches[0].state,
            .colors = {
                { .image = SUB(scene_targets[0]), .load = DONT_CARE, .store = msaa > 1 ? STORE_DISCARD : STORE, .resolve = SUB(scene_resolves[0]) },
                { .image = SUB(scene_targets[1]), .load = DONT_CARE, .store = msaa > 1 ? STORE_DISCARD : STORE, .resolve = SUB(scene_resolves[1]) },
                { .image = SUB(scene_targets[2]), .load = DONT_CARE, .store = msaa > 1 ? STORE_DISCARD : STORE, .resolve = SUB(scene_resolves[2]) },
            },
            .color_count  = 3,
            .depth        = { .image = SUB(targets[TARGET_DEPTH]), .load = CLEAR, .store = STORE_DISCARD },
            .has_depth    = true,
            .images       = { use_sampled(shadow_map) },
            .image_count  = 1,
            .buffers      = { use_read(geometry) },
            .buffer_count = 1,
        });
        pass_scope(pass, "objects"); /* --stats 1 lists the GPU time of the pass's three parts under it */
        for (b = 0; b < batch_count; ++b)
        {
            scene.instances     = instance_address + scene_first[b] * sizeof(Instance);
            scene.vertex_offset = (uint32_t)meshes[batches[b].mesh].vertices.offset;
            scene.index_offset  = (uint32_t)meshes[batches[b].mesh].indices.offset;
            set_state(pass, batches[b].state);
            draw_indexed(pass, BYTE_SPAN(scene), meshes[batches[b].mesh].index_count, scene_count[b]);
        }

        /* petals: six vertices each, built in the vertex shader from the simulated bytes */
        pass_scope(pass, "petals");
        scene.vertex_offset = (uint32_t)petals.offset;
        bind_pipe(pass, pipe_petal);
        set_state(pass, state_petal);
        draw(pass, BYTE_SPAN(scene), 6, PETAL_COUNT);

        /* sky last: one triangle on the far plane, shaded only where nothing else is */
        pass_scope(pass, "sky");
        memset(&sky, 0, sizeof sky);
        mu_mat4_inverse(scene.view_proj, sky.inverse_view_proj);
        mu_vec4_copy(sky.camera, scene.camera);
        mu_vec4_copy(sky.sun, scene.sun);
        mu_vec4_make(sky.sun_color, 1.0f, 0.90f, 0.72f, 0.0f);
        mu_vec4_make(sky.zenith, 0.115f, 0.33f, 0.62f, 0.0f);
        mu_vec4_make(sky.horizon, 0.93f, 0.66f, 0.48f, 0.0f);
        mu_vec4_make(sky.haze, 0.93f, 0.66f, 0.62f, -14.0f);
        bind_pipe(pass, pipe_sky);
        set_state(pass, state_sky);
        draw(pass, BYTE_SPAN(sky), 3, 1);
        end_pass(pass);

        /* -- bloom: down four levels, then back up adding each onto the next ------------ */
        /* A level is a mip of `bloom`. A pass renders into one mip and samples its neighbour through
         * that mip's own id: the image's id reaches every mip, the one being rendered into included. */
        memset(&post, 0, sizeof post);
        post.linear_sampler  = sampler_id(g, linear);
        post.mirror_sampler  = sampler_id(g, mirror);
        post.bloom_threshold = 1.0f;
        post.paper[3]        = 0.5f;
        for (i = 0; i < bloom_levels; ++i)
        {
            source         = i ? SUB_MIP(bloom, i - 1, 1) : SUB(targets[TARGET_COLOR]);
            source_width   = MU_MAX(info.width >> i, 1u);
            source_height  = MU_MAX(info.height >> i, 1u);
            post.texel[0]  = 1.0f / (float)source_width;
            post.texel[1]  = 1.0f / (float)source_height;
            post.color_map = image_sub_sampled_id(g, source);
            pass = begin_pass(g, &(PassDesc){
                .name        = i ? "bloom down" : "bloom prefilter",
                .pipe        = i ? pipe_down : pipe_prefilter,
                .colors      = { attach_sub(SUB_MIP(bloom, i, 1), DONT_CARE, STORE) },
                .color_count = 1,
                .images      = { use_image(source, ACC_READ, IMAGE_SAMPLED) },
                .image_count = 1,
            });
            pass_note(pass, (PassNote){ .samples = 13 }); /* downsample13 */
            draw(pass, BYTE_SPAN(post), 3, 1);
            end_pass(pass);
        }
        for (i = bloom_levels - 1; i > 0; --i)
        {
            source         = SUB_MIP(bloom, i, 1);
            post.texel[0]  = 1.0f / (float)MU_MAX(info.width >> (i + 1), 1u);
            post.texel[1]  = 1.0f / (float)MU_MAX(info.height >> (i + 1), 1u);
            post.color_map = image_sub_sampled_id(g, source);
            pass = begin_pass(g, &(PassDesc){
                .name        = "bloom up",
                .pipe        = pipe_up,
                .state       = state_add,
                .colors      = { attach_sub(SUB_MIP(bloom, i - 1, 1), LOAD, STORE) },
                .color_count = 1,
                .images      = { use_image(source, ACC_READ, IMAGE_SAMPLED) },
                .image_count = 1,
            });
            pass_note(pass, (PassNote){ .samples = 9 }); /* mu_bloom_upsample9 */
            draw(pass, BYTE_SPAN(post), 3, 1);
            end_pass(pass);
        }

        /* -- composite: ink, bloom, tone, paper — into the frame, and into the shot when asked -- */
        mu_vec4_make(post.texel, 1.0f / (float)info.width, 1.0f / (float)info.height, 1.0f / (float)info.width, 1.0f / (float)info.height);
        mu_vec4_make(post.ink, 0.045f, 0.030f, 0.085f, (float)ink_on);
        /* lines keep their weight on any display: the tap radius follows the frame height */
        mu_vec4_make(post.edges, MU_MAX((float)info.height / 720.0f, 1.0f), 0.045f, 0.16f, 0.06f);
        mu_vec4_make(post.grade, 1.0f, bloom_on ? 0.55f : 0.0f, 1.08f, 0.42f);
        mu_vec4_make(post.paper, paper_on ? 0.11f : 0.0f, paper_on ? 1.1f : 0.0f, seconds, 0.5f);
        post.color_map = image_sampled_id(g, targets[TARGET_COLOR]);
        post.aux_map   = image_sampled_id(g, targets[TARGET_AUX]);
        post.id_map    = image_sampled_id(g, targets[TARGET_IDS]);
        post.bloom_map = image_sub_sampled_id(g, SUB(bloom)); /* mip 0: the levels added up */
        post.paper_map = image_sampled_id(g, paper);
        outputs[0]     = frame_image(g);
        outputs[1]     = targets[TARGET_SHOT];
        output_count   = 1 + capture;
        for (i = 0; i < output_count; ++i)
        {
            pass = begin_pass(g, &(PassDesc){
                .name        = "composite",
                .pipe        = pipe_composite,
                .colors      = { attach_write(outputs[i]) },
                .color_count = 1,
                .images      = { use_sampled(targets[TARGET_COLOR]), use_sampled(targets[TARGET_AUX]), use_sampled(targets[TARGET_IDS]), use_sampled(bloom), use_sampled(paper) },
                .image_count = 5,
            });
            pass_note(pass, (PassNote){ .samples = 13 }); /* paper, colour, bloom, and five each of normal-depth and id for the ink */
            draw(pass, BYTE_SPAN(post), 3, 1);
            end_pass(pass);
        }
        frame_end(g);

        /* the frame is on its way to the screen; the copy for the file is its own small submit */
        if (capture)
        {
            pixels = realloc(pixels, (size_t)info.width * info.height * 4);
            shot   = readback_image(g, SUB(targets[TARGET_SHOT]), (DataSpan){ pixels, info.width * info.height * 4 });
            readback_ready(g, shot, true); /* true: wait for it */
            readback_release(g, shot);
            stbi_write_png(shot_path ? shot_path : "toon_shot.png", (int)info.width, (int)info.height, 4, pixels, (int)info.width * 4);
            log_info("wrote %s (%ux%u, frame %u, t = %.2f s)", shot_path ? shot_path : "toon_shot.png", info.width, info.height, frame, (double)seconds);
        }
        frame++;
    }

    /* ---- done -------------------------------------------------------------- */
    gpu_wait_idle(g);
    if (print_stats)
        gpu_dump_stats(g, NULL);
    /* a validation message is a bug in this file or in vk.c: say so and fail the run */
    failed = gpu_error(g)[0] != 0;
    if (failed)
        log_error("the backend reported: %s", gpu_error(g));
    free(pixels);
    gpu_destroy(g); /* takes every image, buffer, sampler, pipe and the surface with it */
    platform_destroy(platform);
    return failed;
}
