/*
 * eco.h — a miniature living ecosystem, as mg draws.
 *
 * The island is a heightfield, the trees and plants are one mesh each animated
 * in the vertex shader, the grass is a single mesh with the wind in the shader,
 * the pond reflects the island through a second pass, and the creatures are
 * billboards that eat, sleep, breed and follow the sun.
 *
 * The simulation is pure CPU and owns no Vulkan: it emits draws, and `EcoRes`
 * is the handles the application already created.
 */
#ifndef ECO_H
#define ECO_H

#include <stdbool.h>
#include <stddef.h>

#include "mg.h"

typedef struct EcoRes {
    /* the scene pass */
    MgPipe  sky, terrain, grass, prop, creature, water;
    /* the reflection pass: the same shaders, compiled with -DMIRROR=1 */
    MgPipe  r_sky, r_terrain, r_grass, r_prop, r_creature;

    MgMat   mat;            /* no texture: every shader is procedural */
    MgMat   mat_water;      /* slot 0 = the reflection pass's output  */
    MgState st_sky;         /* depth read at the far plane, no write  */
    MgState st_solid;       /* depth read/write, cull back            */
    MgState st_solid_nocull;/* the same, for mirrored geometry        */
    MgState st_grass;       /* depth read/write, cull none            */
    MgState st_cutout;      /* alpha blend, depth read, cull none     */
    MgState st_water;       /* alpha blend, depth read, ordered       */

    MgMesh  mesh_sky;       /* 3 indices, nothing fetched             */
    MgMesh  mesh_quad;      /* the unit quad, for creatures           */
} EcoRes;

/* One frame of touch, in pixels, y down. */
typedef struct EcoInput {
    float x, y;
    bool  down;             /* a finger is on the glass  */
    bool  tap;              /* it went down this frame   */
    float drag_dx, drag_dy; /* movement since the last frame */
} EcoInput;

typedef struct Eco Eco;

Eco  *eco_create(MgDevice *d, float w, float h);
void  eco_destroy(Eco *e);

/* Writes this frame's constants (camera, sun, sky) with mg_frame_const(). */
void  eco_set_frame(Eco *e, MgFrame *f, float t, float w, float h);
void  eco_emit_reflection(Eco *e, MgFrame *f, const EcoRes *r);
void  eco_emit_scene(Eco *e, MgFrame *f, const EcoRes *r);

void  eco_update(Eco *e, float dt, float w, float h, EcoInput in);

/* Pre-rotation: the swapchain is created with preTransform = IDENTITY where the
 * surface allows, so the 3D projection carries the display rotation instead. */
void  eco_set_rotation(Eco *e, uint32_t deg);

/* How many creatures are alive, for the startup log. */
int   eco_population(const Eco *e);

/* One HUD line: the day, the clock, the population, and what you are holding. */
void  eco_status(const Eco *e, char *out, size_t n);

/* The reflection pass's water level, so the application can build the pass the
 * same way the shaders expect it. */
float eco_water_level(void);

/* The bisect switch (see eco.c): what to leave out of the frame. 1 sky,
 * 2 terrain, 4 grass, 8 props, 16 creatures, 32 pond, 64 reflection pass,
 * 128 HUD. Set with `adb shell setprop debug.eco.skip 0x40`. */
uint32_t eco_skip_mask(void);

#endif // ECO_H
