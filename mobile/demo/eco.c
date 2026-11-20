/*
 * eco.c — a miniature living ecosystem, expressed in mg draws.
 *
 * The world is a heightfield island with a pond on it. Everything visual is
 * procedural: the trees are one 26-triangle mesh drawn 40 times with per-draw
 * params, the grass is one mesh of 900 blades with the wind in the vertex
 * shader, the pond is one disc whose reflection is a second pass, and the
 * creatures are billboards that eat, sleep, breed and go home at night.
 *
 * The simulation is pure CPU — no Vulkan, no allocation in the frame — and the
 * whole world costs about 280 draws: 90 for the reflection pass and the rest
 * for the scene. That is the point of the demo: a *frame* has a budget, and
 * this one spends it on things that move.
 */

#include "eco.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <sys/system_properties.h>

/* A bisect switch, so one build can answer "which layer costs the frame":
 *   adb shell setprop debug.eco.skip 0x4   (skip the grass) and relaunch.
 * 1 sky, 2 terrain, 4 grass, 8 props, 16 creatures, 32 pond, 64 reflection
 * pass, 128 HUD. Zero (the default) draws everything. */
uint32_t eco_skip_mask(void)
{
    static uint32_t m = 0xFFFFFFFFu;
    if (m == 0xFFFFFFFFu) {
        char b[32] = {0};
        __system_property_get("debug.eco.skip", b);
        m = b[0] ? (uint32_t)strtoul(b, NULL, 0) : 0u;
        if (m) __android_log_print(ANDROID_LOG_INFO, "mg-demo", "eco: skip mask 0x%x", m);
    }
    return m;
}

// ============================================================
// 0. THE WORLD'S CONSTANTS
// ============================================================

#define ISLAND_R     9.5f
#define POND_X       2.4f
#define POND_Z       1.4f
#define POND_R       2.6f
#define WATER_Y      0.15f

#define MAX_CRITTERS 96
#define MAX_PROPS    96
#define GRASS_BLADES 900
#define MAX_HOMES    24
#define DAY_SECONDS  180.0f

#define PICK_PX      46.0f      /* how close a tap must be to grab a creature */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }

/* smoothstep that also works when a > b (a falling edge) */
static float smoothstepf(float a, float b, float x)
{
    float t = (x - a) / (b - a);
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rng_u(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
static float rng_f(void) { return (float)(rng_u() & 0xFFFFFFu) / 16777216.0f; }
static float rng_r(float a, float b) { return a + (b - a) * rng_f(); }

// ============================================================
// 1. SPECIES
// ============================================================

typedef enum { K_MU, K_PIP, K_FENN, K_GLIM, K_TORT, K_COUNT } Kind;

static const struct KindDef {
    const char *name;
    float r, g, b;
    float size;       /* billboard height in world units */
    float speed;      /* units per second                 */
    float graze;      /* how much one meal fills          */
    float snooze;     /* how eagerly it sleeps at night   */
} KINDS[K_COUNT] = {
    { "mu",   0.55f, 0.86f, 0.48f, 0.62f, 1.35f, 0.55f, 0.35f },
    { "pip",  0.96f, 0.85f, 0.35f, 0.42f, 1.85f, 0.45f, 0.25f },
    { "fenn", 0.45f, 0.66f, 0.96f, 0.70f, 1.55f, 0.60f, 0.55f },
    { "glim", 0.92f, 0.52f, 0.88f, 0.48f, 1.20f, 0.40f, 0.70f },
    { "tort", 0.72f, 0.62f, 0.42f, 0.82f, 0.85f, 0.70f, 0.15f },
};

typedef enum { S_ROAM, S_EAT, S_SLEEP } CState;

typedef struct Critter {
    uint8_t  kind, state;
    bool     alive;
    float    x, z, tx, tz;
    float    facing;
    float    hunger, energy, age, timer, breed;
    float    home_x, home_z;
    bool     has_home;
} Critter;

typedef struct Prop {
    float x, y, z, scale, yaw, sway, phase;
    float r, g, b, growth;
    int   kind;         /* 0 tree, 1 bush, 2 rock */
    float fruit;        /* trees only: 0..3 ripe */
} Prop;

struct Eco {
    MgDevice *dev;
    float w, h, t;

    /* camera */
    float yaw, pitch, dist;
    float tx, ty, tz;
    float vp[16];
    float eye[3], right[3], up[3], fwd[3];
    float sun[3];

    /* world */
    Critter crit[MAX_CRITTERS];
    Prop    props[MAX_PROPS];
    int     n_props, pop, homes, collected, eaten, born, died;
    int     held;                 /* species being carried, -1 for none */
    int     day;
    float   tod;                  /* 0..1 through the day */

    /* touch */
    bool  dragging;
    float down_x, down_y;

    uint32_t rot;             /* display pre-rotation, degrees */

    /* meshes */
    MgMesh m_terrain, m_grass, m_tree, m_bush, m_rock, m_water, m_sea;
};

// ============================================================
// 2. THE ISLAND
// ============================================================

static float island_h(float x, float z)
{
    const float r  = sqrtf(x * x + z * z);
    float h = 2.05f * smoothstepf(ISLAND_R, ISLAND_R * 0.25f, r);          /* the dome   */
    h += 0.65f * sinf(x * 0.62f + 0.7f) * cosf(z * 0.55f - 0.3f) *
         smoothstepf(ISLAND_R * 0.95f, ISLAND_R * 0.30f, r);               /* the hills  */
    h += 0.10f * sinf(x * 2.7f) * sinf(z * 2.3f + 1.2f);                   /* the texture*/
    const float dx = x - POND_X, dz = z - POND_Z;
    const float dp = sqrtf(dx * dx + dz * dz);
    h -= 1.95f * smoothstepf(POND_R * 1.45f, POND_R * 0.10f, dp);          /* the basin  */
    h -= 0.60f * smoothstepf(ISLAND_R * 1.02f, ISLAND_R * 0.82f, r);       /* the skirt  */
    return h;
}

static bool on_land(float x, float z)
{
    const float dx = x - POND_X, dz = z - POND_Z;
    return island_h(x, z) > WATER_Y + 0.10f && (dx * dx + dz * dz) > (POND_R * 1.02f) * (POND_R * 1.02f);
}

static void oct_encode(float x, float y, float z, float out[2])
{
    const float l1 = fabsf(x) + fabsf(y) + fabsf(z);
    if (l1 < 1e-6f) { out[0] = 0.0f; out[1] = 0.0f; return; }
    x /= l1; y /= l1;
    if (z < 0.0f) {
        const float ox = (1.0f - fabsf(y)) * (x < 0.0f ? -1.0f : 1.0f);
        const float oy = (1.0f - fabsf(x)) * (y < 0.0f ? -1.0f : 1.0f);
        x = ox; y = oy;
    }
    out[0] = x; out[1] = y;
}

// ============================================================
// 3. MESHES — built once, at load, never touched again
// ============================================================

#ifndef ECO_TEST

static MgMesh build_terrain(MgDevice *d)
{
    const int N = 40;
    const float S = ISLAND_R * 1.18f;
    const int nv = (N + 1) * (N + 1);
    const int ni = N * N * 6;
    float    *pos = malloc(sizeof(float) * 3 * nv);
    float    *nrm = malloc(sizeof(float) * 2 * nv);
    uint16_t *idx = malloc(sizeof(uint16_t) * ni);

    for (int j = 0; j <= N; j++) {
        for (int i = 0; i <= N; i++) {
            const int v = j * (N + 1) + i;
            const float x = -S + 2.0f * S * (float)i / (float)N;
            const float z = -S + 2.0f * S * (float)j / (float)N;
            const float y = island_h(x, z);
            pos[v * 3 + 0] = x;
            pos[v * 3 + 1] = y;
            pos[v * 3 + 2] = z;

            const float e = S / (float)N;
            const float hx = island_h(x + e, z) - island_h(x - e, z);
            const float hz = island_h(x, z + e) - island_h(x, z - e);
            float n[3] = { -hx, 2.0f * e, -hz };
            const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            oct_encode(n[0] / l, n[1] / l, n[2] / l, &nrm[v * 2]);
        }
    }
    int k = 0;
    for (int j = 0; j < N; j++) {
        for (int i = 0; i < N; i++) {
            const uint16_t a = (uint16_t)(j * (N + 1) + i);
            const uint16_t b = (uint16_t)(a + 1);
            const uint16_t c = (uint16_t)(a + (N + 1));
            const uint16_t e = (uint16_t)(c + 1);
            idx[k++] = a; idx[k++] = c; idx[k++] = b;
            idx[k++] = b; idx[k++] = c; idx[k++] = e;
        }
    }

    MgMesh m = mg_mesh(d, &(MgMeshDesc){
        .positions = pos, .vertex_count = (uint32_t)nv,
        .attributes = nrm, .attribute_stride = 8,
        .indices = idx, .index_count = (uint32_t)ni, .indices_u32 = false,
    });
    free(pos); free(nrm); free(idx);
    return m;
}

static MgMesh build_grass(MgDevice *d)
{
    float    *pos = malloc(sizeof(float) * 3 * 4 * GRASS_BLADES);
    float    *atr = malloc(sizeof(float) * 2 * 4 * GRASS_BLADES);
    uint16_t *idx = malloc(sizeof(uint16_t) * 6 * GRASS_BLADES);
    int blades = 0;

    for (int tries = 0; blades < GRASS_BLADES && tries < GRASS_BLADES * 12; tries++) {
        const float a = rng_r(0.0f, 6.2831853f);
        const float r = sqrtf(rng_f()) * ISLAND_R * 0.95f;
        const float x = cosf(a) * r, z = sinf(a) * r;
        if (!on_land(x, z)) continue;

        const float y = island_h(x, z);
        if (y < WATER_Y + 0.18f) continue;

        const float hgt = rng_r(0.30f, 0.62f);
        const float wid = 0.035f + 0.02f * hgt;
        const float lean = rng_r(-0.12f, 0.12f);
        const float phase = rng_f();
        const float dx = cosf(a) * wid, dz = sinf(a) * wid;

        const int v = blades * 4;
        pos[(v + 0) * 3 + 0] = x - dx; pos[(v + 0) * 3 + 1] = y;        pos[(v + 0) * 3 + 2] = z - dz;
        pos[(v + 1) * 3 + 0] = x + dx; pos[(v + 1) * 3 + 1] = y;        pos[(v + 1) * 3 + 2] = z + dz;
        pos[(v + 2) * 3 + 0] = x + dx + lean; pos[(v + 2) * 3 + 1] = y + hgt; pos[(v + 2) * 3 + 2] = z + dz + lean;
        pos[(v + 3) * 3 + 0] = x - dx + lean; pos[(v + 3) * 3 + 1] = y + hgt; pos[(v + 3) * 3 + 2] = z - dz + lean;

        atr[(v + 0) * 2 + 0] = phase; atr[(v + 0) * 2 + 1] = 0.0f;
        atr[(v + 1) * 2 + 0] = phase; atr[(v + 1) * 2 + 1] = 0.0f;
        atr[(v + 2) * 2 + 0] = phase; atr[(v + 2) * 2 + 1] = 1.0f;
        atr[(v + 3) * 2 + 0] = phase; atr[(v + 3) * 2 + 1] = 1.0f;

        uint16_t *o = &idx[blades * 6];
        o[0] = (uint16_t)(v + 0); o[1] = (uint16_t)(v + 1); o[2] = (uint16_t)(v + 2);
        o[3] = (uint16_t)(v + 0); o[4] = (uint16_t)(v + 2); o[5] = (uint16_t)(v + 3);
        blades++;
    }

    MgMesh m = mg_mesh(d, &(MgMeshDesc){
        .positions = pos, .vertex_count = (uint32_t)(blades * 4),
        .attributes = atr, .attribute_stride = 8,
        .indices = idx, .index_count = (uint32_t)(blades * 6), .indices_u32 = false,
    });
    free(pos); free(atr); free(idx);
    return m;
}

/* A tree: a 4-sided tapered trunk and a two-ring crown with an apex. 21
 * vertices, 26 triangles — and 40 of them cost 40 draws and 40 param slots. */
static MgMesh build_tree(MgDevice *d)
{
    float pos[3 * 24], nrm[2 * 24];
    uint16_t idx[3 * 32];
    int nv = 0, ni = 0;

    /* trunk, 4 sides, y 0..0.55 */
    const float tr = 0.10f, tr2 = 0.075f;
    for (int s = 0; s < 4; s++) {
        const float a = (float)s / 4.0f * 6.2831853f + 0.785f;
        const float ca = cosf(a), sa = sinf(a);
        pos[nv * 3 + 0] = ca * tr;  pos[nv * 3 + 1] = 0.0f; pos[nv * 3 + 2] = sa * tr;
        oct_encode(ca, 0.15f, sa, &nrm[nv * 2]); nv++;
        pos[nv * 3 + 0] = ca * tr2; pos[nv * 3 + 1] = 0.55f; pos[nv * 3 + 2] = sa * tr2;
        oct_encode(ca, 0.15f, sa, &nrm[nv * 2]); nv++;
    }
    for (int s = 0; s < 4; s++) {
        const int a = s * 2, b = (s * 2 + 2) % 8, c = s * 2 + 1, e = (s * 2 + 3) % 8;
        idx[ni++] = (uint16_t)a; idx[ni++] = (uint16_t)b; idx[ni++] = (uint16_t)c;
        idx[ni++] = (uint16_t)c; idx[ni++] = (uint16_t)b; idx[ni++] = (uint16_t)e;
    }

    /* crown: ring at y=0.50 r=0.62, ring at y=0.95 r=0.42, apex at 1.28 */
    const int base = nv;
    for (int s = 0; s < 6; s++) {
        const float a = (float)s / 6.0f * 6.2831853f;
        const float ca = cosf(a), sa = sinf(a);
        pos[nv * 3 + 0] = ca * 0.62f; pos[nv * 3 + 1] = 0.50f; pos[nv * 3 + 2] = sa * 0.62f;
        oct_encode(ca, 0.55f, sa, &nrm[nv * 2]); nv++;
    }
    for (int s = 0; s < 6; s++) {
        const float a = (float)s / 6.0f * 6.2831853f;
        const float ca = cosf(a), sa = sinf(a);
        pos[nv * 3 + 0] = ca * 0.42f; pos[nv * 3 + 1] = 0.95f; pos[nv * 3 + 2] = sa * 0.42f;
        oct_encode(ca, 0.8f, sa, &nrm[nv * 2]); nv++;
    }
    pos[nv * 3 + 0] = 0.0f; pos[nv * 3 + 1] = 1.28f; pos[nv * 3 + 2] = 0.0f;
    oct_encode(0.0f, 1.0f, 0.0f, &nrm[nv * 2]);
    const int apex = nv++;

    for (int s = 0; s < 6; s++) {
        const int a = base + s, b = base + (s + 1) % 6;
        const int c = base + 6 + s, e = base + 6 + (s + 1) % 6;
        idx[ni++] = (uint16_t)a; idx[ni++] = (uint16_t)c; idx[ni++] = (uint16_t)b;
        idx[ni++] = (uint16_t)b; idx[ni++] = (uint16_t)c; idx[ni++] = (uint16_t)e;
        idx[ni++] = (uint16_t)c; idx[ni++] = (uint16_t)apex; idx[ni++] = (uint16_t)e;
    }

    MgMesh m = mg_mesh(d, &(MgMeshDesc){
        .positions = pos, .vertex_count = (uint32_t)nv,
        .attributes = nrm, .attribute_stride = 8,
        .indices = idx, .index_count = (uint32_t)ni, .indices_u32 = false,
    });
    return m;
}

/* A bush: the crown of the tree, without the trunk. */
static MgMesh build_bush(MgDevice *d)
{
    float pos[3 * 13], nrm[2 * 13];
    uint16_t idx[3 * 18];
    int nv = 0, ni = 0;

    for (int s = 0; s < 6; s++) {
        const float a = (float)s / 6.0f * 6.2831853f;
        const float ca = cosf(a), sa = sinf(a);
        pos[nv * 3 + 0] = ca * 0.40f; pos[nv * 3 + 1] = 0.05f; pos[nv * 3 + 2] = sa * 0.40f;
        oct_encode(ca, 0.6f, sa, &nrm[nv * 2]); nv++;
    }
    for (int s = 0; s < 6; s++) {
        const float a = (float)s / 6.0f * 6.2831853f;
        const float ca = cosf(a), sa = sinf(a);
        pos[nv * 3 + 0] = ca * 0.26f; pos[nv * 3 + 1] = 0.30f; pos[nv * 3 + 2] = sa * 0.26f;
        oct_encode(ca, 0.9f, sa, &nrm[nv * 2]); nv++;
    }
    pos[nv * 3 + 0] = 0.0f; pos[nv * 3 + 1] = 0.48f; pos[nv * 3 + 2] = 0.0f;
    oct_encode(0.0f, 1.0f, 0.0f, &nrm[nv * 2]);
    const int apex = nv++;

    for (int s = 0; s < 6; s++) {
        const int a = s, b = (s + 1) % 6, c = 6 + s, e = 6 + (s + 1) % 6;
        idx[ni++] = (uint16_t)a; idx[ni++] = (uint16_t)c; idx[ni++] = (uint16_t)b;
        idx[ni++] = (uint16_t)b; idx[ni++] = (uint16_t)c; idx[ni++] = (uint16_t)e;
        idx[ni++] = (uint16_t)c; idx[ni++] = (uint16_t)apex; idx[ni++] = (uint16_t)e;
    }

    return mg_mesh(d, &(MgMeshDesc){
        .positions = pos, .vertex_count = (uint32_t)nv,
        .attributes = nrm, .attribute_stride = 8,
        .indices = idx, .index_count = (uint32_t)ni, .indices_u32 = false,
    });
}

/* A rock: an octahedron, squashed. */
static MgMesh build_rock(MgDevice *d)
{
    const float pos[18] = {
        0.0f, 0.34f, 0.0f,  -0.30f, 0.0f, -0.22f,  0.30f, 0.0f, -0.22f,
        0.30f, 0.0f, 0.22f,  -0.30f, 0.0f, 0.22f,   0.0f, -0.10f, 0.0f,
    };
    const uint16_t idx[24] = {
        0, 2, 1, 0, 3, 2, 0, 4, 3, 0, 1, 4,
        5, 1, 2, 5, 2, 3, 5, 3, 4, 5, 4, 1,
    };
    float nrm[12];
    for (int s = 0; s < 6; s++) {
        const float *p = &pos[s * 3];
        const float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2] + 1e-6f);
        oct_encode(p[0] / l, p[1] / l + 0.4f, p[2] / l, &nrm[s * 2]);
    }
    return mg_mesh(d, &(MgMeshDesc){
        .positions = pos, .vertex_count = 6,
        .attributes = nrm, .attribute_stride = 8,
        .indices = idx, .index_count = 24, .indices_u32 = false,
    });
}

/* A flat disc at the water level: a fan, and all of it is a fan, because the
 * waves are in the vertex shader. One builder serves the pond (2.6 units) and
 * the sea around the island (36 units); the shader takes the centre and the
 * radius from the draw's params, which is also how it knows where the shore is. */
static MgMesh build_disc(MgDevice *d, float cx, float cz, float rad)
{
    enum { SEG = 48 };
    float pos[3 * (SEG + 2)];
    float nrm[2 * (SEG + 2)];
    uint16_t idx[3 * SEG];
    pos[0] = cx; pos[1] = WATER_Y; pos[2] = cz;
    oct_encode(0.0f, 1.0f, 0.0f, &nrm[0]);
    for (int s = 0; s <= SEG; s++) {
        const float a = (float)s / (float)SEG * 6.2831853f;
        const int v = 1 + s;
        pos[v * 3 + 0] = cx + cosf(a) * rad;
        pos[v * 3 + 1] = WATER_Y;
        pos[v * 3 + 2] = cz + sinf(a) * rad;
        oct_encode(0.0f, 1.0f, 0.0f, &nrm[v * 2]);
    }
    for (int s = 0; s < SEG; s++) {
        idx[s * 3 + 0] = 0;
        idx[s * 3 + 1] = (uint16_t)(1 + s);
        idx[s * 3 + 2] = (uint16_t)(2 + s);
    }
    return mg_mesh(d, &(MgMeshDesc){
        .positions = pos, .vertex_count = (uint32_t)(SEG + 2),
        .attributes = nrm, .attribute_stride = 8,
        .indices = idx, .index_count = (uint32_t)(SEG * 3), .indices_u32 = false,
    });
}

#endif // !ECO_TEST

// ============================================================
// 4. THE MATHS THE CAMERA NEEDS
// ============================================================

static void m4_identity(float *m) { memset(m, 0, 16 * sizeof(float)); m[0] = m[5] = m[10] = m[15] = 1.0f; }

/* Column-major, like GLSL: element (row r, column c) lives at m[c * 4 + r].
 * That is the order m4_look() and m4_persp() write, and the order a mat4
 * uniform reads, so the product has to be taken in the same order too: out =
 * a * b means out(r,c) = sum_k a(r,k) * b(k,c) = sum_k a[k*4+r] * b[c*4+k]. */
static void m4_mul(float *out, const float *a, const float *b)
{
    float t[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                           a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    memcpy(out, t, sizeof t);
}

/* Vulkan clip space: y down, depth 0..1. */
static void m4_persp(float *m, float fovy, float aspect, float zn, float zf)
{
    memset(m, 0, 16 * sizeof(float));
    const float t = 1.0f / tanf(fovy * 0.5f);
    m[0]  = t / aspect;
    m[5]  = -t;
    m[10] = zf / (zn - zf);
    m[11] = -1.0f;
    m[14] = zn * zf / (zn - zf);
}

static void m4_look(float *m, const float eye[3], const float at[3], const float upv[3])
{
    float f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    f[0] /= l; f[1] /= l; f[2] /= l;

    float s[3] = { f[1] * upv[2] - f[2] * upv[1], f[2] * upv[0] - f[0] * upv[2], f[0] * upv[1] - f[1] * upv[0] };
    l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    s[0] /= l; s[1] /= l; s[2] /= l;

    const float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };

    m[0] = s[0];  m[1] = u[0];  m[2] = -f[0];  m[3]  = 0.0f;
    m[4] = s[1];  m[5] = u[1];  m[6] = -f[1];  m[7]  = 0.0f;
    m[8] = s[2];  m[9] = u[2];  m[10] = -f[2]; m[11] = 0.0f;
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] =  (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    m[15] = 1.0f;
}

static void cam_update(Eco *e, float w, float h)
{
    e->w = w;
    e->h = h;
    const float cp = cosf(e->pitch), sp = sinf(e->pitch);
    e->eye[0] = e->tx + sinf(e->yaw) * cp * e->dist;
    e->eye[1] = e->ty + sp * e->dist;
    e->eye[2] = e->tz + cosf(e->yaw) * cp * e->dist;

    const float upf[3] = { 0.0f, 1.0f, 0.0f };
    float at[3] = { e->tx, e->ty, e->tz };
    float view[16], proj[16];
    m4_look(view, e->eye, at, upf);
    /* A portrait screen is the constraint, not the vertical: the horizontal
     * field of view is what has to hold the island, so the vertical one is
     * opened up until it does. */
    m4_persp(proj, 58.0f * 3.14159265f / 180.0f, w / (h > 0.0f ? h : 1.0f), 0.5f, 400.0f);
    m4_mul(e->vp, proj, view);

    /* Display pre-rotation, applied to the projection: the swapchain is
     * IDENTITY and the vertex shader turns the frame, for free (mg.h's note on
     * pre_rotate). The HUD is drawn in pixels and is unaffected. */
    if (e->rot % 360u != 0u) {
        const float c  = (e->rot == 180u) ? -1.0f : (e->rot == 90u || e->rot == 270u) ? 0.0f : 1.0f;
        const float sn = (e->rot == 90u) ? 1.0f : (e->rot == 270u) ? -1.0f : 0.0f;
        float r[16];
        m4_identity(r);
        r[0] = c;  r[1] = sn;
        r[4] = sn; r[5] = -c;
        float tmp[16];
        m4_mul(tmp, r, e->vp);
        memcpy(e->vp, tmp, sizeof tmp);
    }

    /* camera basis, for billboards and for picking */
    e->fwd[0] = at[0] - e->eye[0]; e->fwd[1] = at[1] - e->eye[1]; e->fwd[2] = at[2] - e->eye[2];
    float l = sqrtf(e->fwd[0] * e->fwd[0] + e->fwd[1] * e->fwd[1] + e->fwd[2] * e->fwd[2]);
    for (int i = 0; i < 3; i++) e->fwd[i] /= l;

    e->right[0] =  e->fwd[2]; e->right[1] = 0.0f; e->right[2] = -e->fwd[0];
    l = sqrtf(e->right[0] * e->right[0] + e->right[2] * e->right[2] + 1e-6f);
    for (int i = 0; i < 3; i++) e->right[i] /= l;

    e->up[0] = e->right[1] * e->fwd[2] - e->right[2] * e->fwd[1];
    e->up[1] = e->right[2] * e->fwd[0] - e->right[0] * e->fwd[2];
    e->up[2] = e->right[0] * e->fwd[1] - e->right[1] * e->fwd[0];
}

/* Screen pixel -> a ray onto the island (the plane y = WATER_Y is enough for
 * placing things; the ground height is looked up after). */
static void screen_ray(const Eco *e, float px, float py, float out_dir[3])
{
    const float nx = px / (e->w > 0.0f ? e->w : 1.0f) * 2.0f - 1.0f;
    const float ny = py / (e->h > 0.0f ? e->h : 1.0f) * 2.0f - 1.0f;
    const float tanh = tanf(58.0f * 3.14159265f / 180.0f * 0.5f);
    const float aspect = e->w / (e->h > 0.0f ? e->h : 1.0f);

    for (int i = 0; i < 3; i++)
        out_dir[i] = e->fwd[i] + e->right[i] * (nx * tanh * aspect) + e->up[i] * (-ny * tanh);

    const float l = sqrtf(out_dir[0] * out_dir[0] + out_dir[1] * out_dir[1] + out_dir[2] * out_dir[2]);
    for (int i = 0; i < 3; i++) out_dir[i] /= l;
}

static bool ground_hit(const Eco *e, float px, float py, float out[2])
{
    float d[3];
    screen_ray(e, px, py, d);
    if (fabsf(d[1]) < 1e-4f) return false;
    const float t = (WATER_Y - e->eye[1]) / d[1];
    if (t <= 0.0f) return false;
    const float x = e->eye[0] + d[0] * t;
    const float z = e->eye[2] + d[2] * t;
    if (x * x + z * z > (ISLAND_R * 1.1f) * (ISLAND_R * 1.1f)) return false;
    out[0] = x;
    out[1] = z;
    return true;
}

/* World point -> screen pixels, for picking creatures. */
static bool world_to_screen(const Eco *e, float x, float y, float z, float *ox, float *oy)
{
    const float *m = e->vp;
    const float cx = m[0] * x + m[4] * y + m[8]  * z + m[12];
    const float cy = m[1] * x + m[5] * y + m[9]  * z + m[13];
    const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= 0.001f) return false;
    *ox = (cx / cw * 0.5f + 0.5f) * e->w;
    *oy = (cy / cw * 0.5f + 0.5f) * e->h;
    return true;
}

// ============================================================
// 5. THE SIMULATION
// ============================================================

static int free_slot(const Eco *e)
{
    for (int i = 0; i < MAX_CRITTERS; i++) if (!e->crit[i].alive) return i;
    return -1;
}

static void spawn(Eco *e, int kind, float x, float z, bool home)
{
    if (e->pop >= MAX_CRITTERS) return;
    const int s = free_slot(e);
    if (s < 0) return;

    Critter *c = &e->crit[s];
    memset(c, 0, sizeof *c);
    c->alive = true;
    c->kind = (uint8_t)kind;
    c->state = S_ROAM;
    c->x = x;
    c->z = z;
    c->tx = x;
    c->tz = z;
    c->facing = rng_f() < 0.5f ? -1.0f : 1.0f;
    c->hunger = rng_r(0.05f, 0.4f);
    c->energy = rng_r(0.5f, 0.9f);
    c->age = home ? rng_r(8.0f, 16.0f) : 0.0f;
    c->timer = rng_r(0.0f, 3.0f);
    c->home_x = x;
    c->home_z = z;
    c->has_home = home;
    e->pop++;
}

static void pick_target(Eco *e, Critter *c)
{
    (void)e;
    const float spread = c->has_home ? 2.6f : ISLAND_R * 0.85f;
    const float cxw = c->has_home ? c->home_x : 0.0f;
    const float czw = c->has_home ? c->home_z : 0.0f;

    for (int tries = 0; tries < 12; tries++) {
        const float x = cxw + rng_r(-spread, spread);
        const float z = czw + rng_r(-spread, spread);
        if (!on_land(x, z)) continue;
        c->tx = x;
        c->tz = z;
        return;
    }
    c->tx = cxw;
    c->tz = czw;
}

static bool eat_spot(Eco *e, Critter *c)
{
    /* fruit first: the nearest tree with something ripe on it */
    int best = -1;
    float bestd = 1e9f;
    for (int i = 0; i < e->n_props; i++) {
        const Prop *p = &e->props[i];
        if (p->kind != 0 || p->fruit < 0.6f) continue;
        const float dx = p->x - c->x, dz = p->z - c->z;
        const float d2 = dx * dx + dz * dz;
        if (d2 < bestd) { bestd = d2; best = i; }
    }
    if (best >= 0) {
        c->tx = e->props[best].x;
        c->tz = e->props[best].z;
        return true;
    }
    /* otherwise graze wherever there is grass */
    for (int tries = 0; tries < 10; tries++) {
        const float a = rng_r(0.0f, 6.2831853f);
        const float r = sqrtf(rng_f()) * ISLAND_R * 0.9f;
        const float x = cosf(a) * r, z = sinf(a) * r;
        if (!on_land(x, z)) continue;
        c->tx = x;
        c->tz = z;
        return true;
    }
    return false;
}

static void critter_step(Eco *e, Critter *c, float dt, bool night)
{
    const struct KindDef *k = &KINDS[c->kind];
    c->age += dt;
    c->timer -= dt;
    c->breed -= dt;

    c->hunger += dt * (c->state == S_EAT ? 0.01f : 0.035f);
    if (c->state == S_SLEEP) c->energy += dt * 0.16f * (0.6f + k->snooze);
    else                     c->energy -= dt * 0.022f;
    c->hunger = clampf(c->hunger, 0.0f, 1.6f);
    c->energy = clampf(c->energy, 0.0f, 1.0f);

    switch (c->state) {
    case S_ROAM:
        if (night && c->energy < 0.75f && rng_f() < 0.4f) {
            /* sleep at the foot of the nearest tree */
            int best = -1; float bd = 1e9f;
            for (int i = 0; i < e->n_props; i++) {
                const Prop *p = &e->props[i];
                if (p->kind == 2) continue;
                const float dx = p->x - c->x, dz = p->z - c->z;
                const float d2 = dx * dx + dz * dz;
                if (d2 < bd) { bd = d2; best = i; }
            }
            if (best >= 0) {
                c->state = S_SLEEP;
                c->tx = e->props[best].x + rng_r(-0.4f, 0.4f);
                c->tz = e->props[best].z + rng_r(-0.4f, 0.4f);
            }
        } else if (c->hunger > 0.55f && eat_spot(e, c)) {
            c->state = S_EAT;
        } else if (c->timer <= 0.0f) {
            pick_target(e, c);
            c->timer = rng_r(2.5f, 6.0f);
        }
        break;

    case S_EAT: {
        const float dx = c->tx - c->x, dz = c->tz - c->z;
        if (dx * dx + dz * dz < 0.16f) {
            c->hunger -= 0.45f + k->graze * 0.4f;
            c->hunger = clampf(c->hunger, 0.0f, 1.6f);
            e->eaten++;
            for (int i = 0; i < e->n_props; i++) {
                Prop *p = &e->props[i];
                if (p->kind != 0 || p->fruit < 0.6f) continue;
                const float ddx = p->x - c->x, ddz = p->z - c->z;
                if (ddx * ddx + ddz * ddz < 1.6f) { p->fruit -= 1.0f; break; }
            }
            c->state = S_ROAM;
            c->timer = rng_r(1.5f, 4.0f);
        } else if (night && c->energy < 0.4f) {
            c->state = S_ROAM;
            c->timer = 0.0f;
        }
        break;
    }

    case S_SLEEP:
        if (!night || c->energy > 0.97f) {
            c->state = S_ROAM;
            c->timer = 0.0f;
            pick_target(e, c);
        }
        break;
    }

    /* ---- move ---- */
    const float dx = c->tx - c->x, dz = c->tz - c->z;
    const float d = sqrtf(dx * dx + dz * dz);
    if (d > 0.02f) {
        float sp = k->speed * (c->state == S_SLEEP ? 0.55f : 1.0f);
        if (c->state == S_EAT && c->hunger > 0.9f) sp *= 1.3f;   /* hungry ones hurry */
        float step = sp * dt;
        if (step > d) step = d;
        c->x += dx / d * step;
        c->z += dz / d * step;
        if (dx > 0.01f) c->facing = 1.0f;
        else if (dx < -0.01f) c->facing = -1.0f;
    }

    /* ---- stay on the land, out of the pond ---- */
    if (!on_land(c->x, c->z)) {
        const float l = sqrtf(c->x * c->x + c->z * c->z) + 1e-4f;
        c->x -= c->x / l * 0.08f;
        c->z -= c->z / l * 0.08f;
        c->timer = 0.0f;
    }

    /* ---- breed: two well-fed adults of the same species, close together ---- */
    if (c->age > 16.0f && c->breed <= 0.0f && c->energy > 0.7f && c->hunger < 0.35f) {
        for (int i = 0; i < MAX_CRITTERS; i++) {
            Critter *o = &e->crit[i];
            if (o == c || !o->alive || o->kind != c->kind) continue;
            if (o->age < 16.0f || o->breed > 0.0f) continue;
            const float ox = o->x - c->x, oz = o->z - c->z;
            if (ox * ox + oz * oz > 1.2f) continue;
            spawn(e, c->kind, (c->x + o->x) * 0.5f, (c->z + o->z) * 0.5f, false);
            c->breed = 30.0f;
            o->breed = 30.0f;
            c->energy -= 0.2f;
            o->energy -= 0.2f;
            c->hunger += 0.2f;
            o->hunger += 0.2f;
            e->born++;
            break;
        }
    }

    /* ---- old age, or starvation ---- */
    if (c->age > 300.0f || c->hunger > 1.5f) {
        c->alive = false;
        e->pop--;
        e->died++;
    }
}

// ============================================================
// 6. CREATE / UPDATE / STATUS
// ============================================================

Eco *eco_create(MgDevice *d, float w, float h)
{
    Eco *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->dev = d;
    e->w = w;
    e->h = h;
    e->yaw = 0.7f;
    e->pitch = 0.46f;
    e->dist = 30.0f;
    e->tx = 0.0f;
    e->ty = 0.6f;
    e->tz = 0.0f;
    e->held = -1;
    e->day = 1;

#ifndef ECO_TEST
    e->m_terrain = build_terrain(d);
    e->m_grass   = build_grass(d);
    e->m_tree    = build_tree(d);
    e->m_bush    = build_bush(d);
    e->m_rock    = build_rock(d);
    e->m_water   = build_disc(d, POND_X, POND_Z, POND_R * 1.02f);
    e->m_sea     = build_disc(d, 0.0f, 0.0f, 160.0f);
#endif

    /* trees, bushes and rocks, placed on land with a deterministic layout */
    for (int tries = 0; tries < 4000 && e->n_props < MAX_PROPS; tries++) {
        const float a = rng_r(0.0f, 6.2831853f);
        const float r = sqrtf(rng_f()) * ISLAND_R * 0.9f;
        const float x = cosf(a) * r, z = sinf(a) * r;
        if (!on_land(x, z)) continue;

        Prop *p = &e->props[e->n_props];
        memset(p, 0, sizeof *p);
        p->x = x;
        p->z = z;
        p->y = island_h(x, z) - 0.05f;
        p->yaw = rng_r(0.0f, 6.2831853f);
        p->phase = rng_r(0.0f, 6.2831853f);
        p->sway = rng_r(0.7f, 1.4f);

        const float roll = rng_f();
        if (roll < 0.55f) {
            p->kind = 0;
            p->scale = rng_r(1.7f, 2.9f);
            p->r = rng_r(0.22f, 0.38f);
            p->g = rng_r(0.42f, 0.62f);
            p->b = rng_r(0.18f, 0.30f);
            p->fruit = rng_r(0.0f, 3.0f);
        } else if (roll < 0.85f) {
            p->kind = 1;
            p->scale = rng_r(0.9f, 1.6f);
            p->r = rng_r(0.30f, 0.45f);
            p->g = rng_r(0.50f, 0.70f);
            p->b = rng_r(0.25f, 0.40f);
        } else {
            p->kind = 2;
            p->scale = rng_r(0.5f, 1.1f);
            p->r = rng_r(0.38f, 0.52f);
            p->g = rng_r(0.36f, 0.50f);
            p->b = rng_r(0.34f, 0.46f);
            p->sway = 0.0f;
        }
        p->growth = 0.85f + 0.15f * p->scale / 3.0f;
        e->n_props++;
    }

    for (int i = 0; i < 14; i++) spawn(e, (int)(rng_u() % K_COUNT), 0.0f, 0.0f, false);
    for (int i = 0; i < MAX_CRITTERS; i++) {
        Critter *c = &e->crit[i];
        if (!c->alive) continue;
        pick_target(e, c);
        c->x = c->tx;
        c->z = c->tz;
    }
    return e;
}

void eco_destroy(Eco *e) { free(e); }

void eco_set_rotation(Eco *e, uint32_t deg) { e->rot = deg; }

int eco_population(const Eco *e) { return e->pop; }

void eco_update(Eco *e, float dt, float w, float h, EcoInput in)
{
    if (dt > 0.1f) dt = 0.1f;
    cam_update(e, w, h);        /* picking needs this frame's matrices */
    e->t += dt;
    /* The clock starts at 08:00, not at midnight: the first frame a user sees
     * should be the island in daylight, not a dark sky. */
    const float tod = fmodf(e->t / DAY_SECONDS + 0.34f, 1.0f);
    if (tod < e->tod) e->day++;          /* the clock wrapped past midnight */
    e->tod = tod;

    /* ---- camera: drag orbits, otherwise a slow turn ---- */
    if (in.down && (in.drag_dx != 0.0f || in.drag_dy != 0.0f)) {
        e->yaw -= in.drag_dx * 0.006f;
        e->pitch = clampf(e->pitch + in.drag_dy * 0.004f, 0.28f, 1.15f);
        e->dragging = true;
    } else {
        e->dragging = false;
        e->yaw += dt * 0.045f;
    }

    /* ---- tap: collect a creature, or put one down ---- */
    if (in.tap) {
        int best = -1;
        float bd = PICK_PX * PICK_PX;
        for (int i = 0; i < MAX_CRITTERS; i++) {
            const Critter *c = &e->crit[i];
            if (!c->alive) continue;
            float sx, sy;
            const float y = island_h(c->x, c->z) + KINDS[c->kind].size * 0.5f;
            if (!world_to_screen(e, c->x, y, c->z, &sx, &sy)) continue;
            const float dx = sx - in.x, dy = sy - in.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < bd) { bd = d2; best = i; }
        }
        if (best >= 0) {
            e->crit[best].alive = false;
            e->pop--;
            e->held = e->crit[best].kind;
            e->collected++;
        } else if (e->held >= 0) {
            float g[2];
            if (ground_hit(e, in.x, in.y, g) && on_land(g[0], g[1])) {
                spawn(e, e->held, g[0], g[1], true);
                e->homes++;
                e->held = -1;
            }
        }
    }

    /* ---- the world ---- */
    const bool night = e->tod < 0.20f || e->tod > 0.80f;
    for (int i = 0; i < MAX_CRITTERS; i++)
        if (e->crit[i].alive) critter_step(e, &e->crit[i], dt, night);

    for (int i = 0; i < e->n_props; i++) {
        Prop *p = &e->props[i];
        if (p->kind == 0 && p->fruit < 3.0f) p->fruit += dt * 0.12f;
    }
}

void eco_status(const Eco *e, char *out, size_t n)
{
    const int hh = (int)(e->tod * 24.0f);
    const int mm = (int)((e->tod * 24.0f - (float)hh) * 60.0f);
    if (e->held >= 0)
        snprintf(out, n, "d%d %02d:%02d pop %d - tap to place", e->day, hh, mm, e->pop);
    else
        snprintf(out, n, "d%d %02d:%02d pop %d home %d", e->day, hh, mm, e->pop, e->homes);
}

float eco_water_level(void) { return WATER_Y; }

// ============================================================
// 7. THE FRAME
// ============================================================

/* 208 bytes: the whole frame ABI in shaders/frame_abi.glsl, written once. */
typedef struct FrameUbo {
    float viewproj[16];
    float time[4];
    float sunv[4];
    float light[4];
    float sky[4];
    float horizon[4];
    float campos[4];
    float camright[4];
    float camup[4];
    float sunndc[4];
} FrameUbo;

static const float k_no_param[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

static void emit(MgFrame *f, MgPipe pipe, MgMat mat, MgMesh mesh, MgState st,
                 const void *data, uint32_t bytes, uint32_t flags)
{
    mg_draw(f, &(MgDrawDesc){
        .pipe = pipe, .mat = mat, .mesh = mesh, .state = st,
        .params = mg_const(f, data ? data : k_no_param, data ? bytes : sizeof k_no_param),
        .instances = 1,
        .flags = flags,
    });
}

void eco_set_frame(Eco *e, MgFrame *f, float t, float w, float h)
{
    cam_update(e, w, h);

    FrameUbo u;
    memset(&u, 0, sizeof u);
    memcpy(u.viewproj, e->vp, sizeof u.viewproj);
    u.time[0] = t;
    u.time[1] = w / (h > 0.0f ? h : 1.0f);
    u.time[2] = w;
    u.time[3] = h;

    /* ---- where the sun is: a full circle, rising at 06:00 --------------- */
    const float ang = (e->tod - 0.25f) * 6.2831853f;
    float sd[3] = { cosf(ang) * 0.82f, sinf(ang), -0.34f };
    float l = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
    for (int i = 0; i < 3; i++) sd[i] /= l;
    e->sun[0] = sd[0]; e->sun[1] = sd[1]; e->sun[2] = sd[2];

    const float elev = sd[1];
    const float day  = smoothstepf(-0.10f, 0.26f, elev);
    const float glow = expf(-(elev - 0.02f) * (elev - 0.02f) / 0.045f);   /* dawn/dusk */
    const float night = 1.0f - day;

    /* at night the light is the anti-sun: the moon, opposite the sun ---------- */
    const float sign = elev < 0.0f ? -1.0f : 1.0f;
    u.sunv[0] = sd[0] * sign;
    u.sunv[1] = sd[1] * sign;
    u.sunv[2] = sd[2] * sign;
    u.sunv[3] = mixf(0.20f, 1.0f, day);

    u.light[0] = mixf(0.50f, 1.00f, day);
    u.light[1] = mixf(0.62f, 0.96f, day);
    u.light[2] = mixf(1.00f, 0.88f, day);
    u.light[3] = mixf(0.155f, 0.330f, day);          /* ambient                 */

    u.sky[0] = mixf(0.030f, 0.235f, day) + glow * 0.30f;
    u.sky[1] = mixf(0.040f, 0.480f, day) + glow * 0.12f;
    u.sky[2] = mixf(0.090f, 0.920f, day) + glow * 0.06f;
    u.sky[3] = mixf(0.30f, 0.16f, day);              /* fog                      */

    u.horizon[0] = mixf(0.055f, 0.680f, day) + glow * 0.85f;
    u.horizon[1] = mixf(0.075f, 0.820f, day) + glow * 0.42f;
    u.horizon[2] = mixf(0.150f, 0.950f, day) + glow * 0.16f;
    u.horizon[3] = WATER_Y;                          /* the water plane          */

    u.campos[0] = e->eye[0]; u.campos[1] = e->eye[1]; u.campos[2] = e->eye[2];
    u.camright[0] = e->right[0]; u.camright[1] = e->right[1]; u.camright[2] = e->right[2];
    u.camup[0] = e->up[0]; u.camup[1] = e->up[1]; u.camup[2] = e->up[2];

    /* ---- project the sun (or the moon) onto the screen ------------------ */
    {
        const float dist = 90.0f;
        const float px = e->eye[0] + u.sunv[0] * dist;
        const float py = e->eye[1] + u.sunv[1] * dist;
        const float pz = e->eye[2] + u.sunv[2] * dist;
        const float *m = u.viewproj;
        const float cx = m[0] * px + m[4] * py + m[8]  * pz + m[12];
        const float cy = m[1] * px + m[5] * py + m[9]  * pz + m[13];
        const float cw = m[3] * px + m[7] * py + m[11] * pz + m[15];
        if (cw > 0.001f) {
            u.sunndc[0] = (cx / cw) * 0.5f + 0.5f;
            u.sunndc[1] = (cy / cw) * 0.5f + 0.5f;
        } else {
            u.sunndc[0] = 3.0f;     /* behind the camera: off screen */
            u.sunndc[1] = 3.0f;
        }
        u.sunndc[2] = 0.055f;
        u.sunndc[3] = night;
    }

    mg_frame_const(f, &u, sizeof u);
}

/* The world, minus the pond surface itself — the pond is never in its own
 * reflection, and in the scene pass it is drawn last, over everything. */
static void emit_world(Eco *e, MgFrame *f, const EcoRes *r, bool mirror)
{
    const MgPipe p_sky     = mirror ? r->r_sky     : r->sky;
    const MgPipe p_terrain = mirror ? r->r_terrain : r->terrain;
    const MgPipe p_grass   = mirror ? r->r_grass   : r->grass;
    const MgPipe p_prop    = mirror ? r->r_prop    : r->prop;
    const MgPipe p_creat   = mirror ? r->r_creature: r->creature;

    /* mirrored geometry reverses its winding, so the reflection pass cannot
     * cull: every face would be a back face. */
    const MgState st_solid = mirror ? r->st_solid_nocull : r->st_solid;
    const uint32_t skip = eco_skip_mask();

    if (!(skip & 1u))  emit(f, p_sky, r->mat, r->mesh_sky, r->st_sky, NULL, 0, 0);
    if (!(skip & 2u))  emit(f, p_terrain, r->mat, e->m_terrain, st_solid, NULL, 0, 0);
    if (!(skip & 4u))  emit(f, p_grass, r->mat, e->m_grass, r->st_grass, NULL, 0, 0);

    for (int i = 0; i < (skip & 8u ? 0 : e->n_props); i++) {
        const Prop *p = &e->props[i];
        float pp[12] = {
            p->x, p->y, p->z, p->scale,
            p->yaw, p->sway, p->phase, (float)p->kind,
            p->r, p->g, p->b, p->growth,
        };
        const MgMesh mesh = p->kind == 0 ? e->m_tree : (p->kind == 1 ? e->m_bush : e->m_rock);
        emit(f, p_prop, r->mat, mesh, st_solid, pp, sizeof pp, 0);
    }

    /* ---- creatures, back to front, because alpha is order ---------------- */
    int   idx[MAX_CRITTERS];
    float dep[MAX_CRITTERS];
    int   n = 0;
    for (int i = 0; i < MAX_CRITTERS; i++) {
        const Critter *c = &e->crit[i];
        if (!c->alive) continue;
        const float dx = c->x - e->eye[0];
        const float dy = island_h(c->x, c->z) + KINDS[c->kind].size * 0.5f - e->eye[1];
        const float dz = c->z - e->eye[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        int j = n++;
        while (j > 0 && dep[j - 1] < d2) {           /* insertion sort, far first */
            dep[j] = dep[j - 1];
            idx[j] = idx[j - 1];
            j--;
        }
        dep[j] = d2;
        idx[j] = i;
    }

    if (skip & 16u) n = 0;
    for (int k = 0; k < n; k++) {
        const Critter *c = &e->crit[idx[k]];
        const struct KindDef *kd = &KINDS[c->kind];
        const float y = island_h(c->x, c->z) - 0.04f;
        const float phase = c->x * 1.7f + c->z * 2.3f;
        float cp[12] = {
            c->x, y, c->z, kd->size * 0.5f,
            kd->r, kd->g, kd->b, 1.0f,
            phase, (float)c->kind,
            c->state == S_SLEEP ? 0.25f : 1.0f,
            c->facing,
        };
        emit(f, p_creat, r->mat, r->mesh_quad, r->st_cutout, cp, sizeof cp, MG_DRAW_ORDERED);
    }
}

void eco_emit_reflection(Eco *e, MgFrame *f, const EcoRes *r)
{
    emit_world(e, f, r, true);
}

void eco_emit_scene(Eco *e, MgFrame *f, const EcoRes *r)
{
    emit_world(e, f, r, false);

    if (!(eco_skip_mask() & 512u)) {
        static int dbg;
        if (dbg++ % 120 == 0) {
            int shown = 0;
            for (int i = 0; i < MAX_CRITTERS && shown < 3; i++) {
                const Critter *c = &e->crit[i];
                if (!c->alive) continue;
                float sx = -1, sy = -1;
                const float y = island_h(c->x, c->z) + KINDS[c->kind].size * 0.5f;
                const bool on = world_to_screen(e, c->x, y, c->z, &sx, &sy);
                __android_log_print(ANDROID_LOG_INFO, "mg-demo",
                                    "crit %d kind %d at %.2f %.2f %.2f -> screen %.0f %.0f (%s) size %.2f", i,
                                    (int)c->kind, (double)c->x, (double)y, (double)c->z, (double)sx, (double)sy,
                                    on ? "in front" : "BEHIND", (double)KINDS[c->kind].size);
                shown++;
            }
            __android_log_print(ANDROID_LOG_INFO, "mg-demo", "right %.2f %.2f %.2f eye %.1f %.1f %.1f",
                                (double)e->right[0], (double)e->right[1], (double)e->right[2],
                                (double)e->eye[0], (double)e->eye[1], (double)e->eye[2]);
        }
    }

    /* ---- water, last: it samples the reflection pass's output -------------
     * The sea first, then the pond on top of it: both are the same shader, and
     * the only difference is the centre and radius in the param slot. */
    if (!(eco_skip_mask() & 32u)) {
        /* 3 cm below the pond, so the two discs never fight for the same depth:
         * the sea is occluded everywhere the pond has already been drawn. */
        const float sp[4] = { 0.0f, 0.0f, 160.0f, WATER_Y - 0.03f };
        const float wp[4] = { POND_X, POND_Z, POND_R, WATER_Y };
        emit(f, r->water, r->mat_water, e->m_sea, r->st_water, sp, sizeof sp, MG_DRAW_ORDERED);
        emit(f, r->water, r->mat_water, e->m_water, r->st_water, wp, sizeof wp, MG_DRAW_ORDERED);
    }
}
