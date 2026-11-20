/*
 * hud.h — the frame-time overlay: a 96-frame graph and two lines of live text.
 *
 * One draw, 608 bytes of param slot, no texture and no upload: the text is
 * ASCII in the param block and shaders/hud.frag carries MicroProfile's 5x8
 * bitmap font to render it.
 */
#ifndef HUD_H
#define HUD_H

#include <stddef.h>

#include "mg.h"

#define HUD_SAMPLES 96

typedef struct HudRes {
    MgPipe  pipe;
    MgMat   mat;        /* no texture: both shaders are procedural */
    MgState state;      /* alpha blend, no depth                    */
    MgMesh  quad;       /* the unit quad: 4 vertices, 6 indices     */
} HudRes;

typedef struct Hud {
    float    hist[HUD_SAMPLES];
    uint32_t at, n;
} Hud;

/* Emits the panel into the pass currently being recorded. The panel spans the
 * top of the target, `w` x `h` in pixels. */
void hud_draw(Hud *hud, MgFrame *f, const HudRes *r,
              float w, float hgt, float frame_ms, float cpu_ms,
              const char *line1, const char *line2);

#endif // HUD_H
