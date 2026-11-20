/*
 * hud.c — the frame-time HUD as mg draws.
 *
 * The layout is written down in shaders/hud.frag; this file fills it in. The
 * whole overlay is one draw call and 608 bytes of param slot per frame.
 */

#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define HUD_TEXT1   2     /* 24 characters = 6 vec4 */
#define HUD_TEXT2   8
#define HUD_GRAPH  14     /* 96 samples = 24 vec4   */
#define HUD_LINE_CHARS 24

typedef struct { float v[4]; } Vec4;

typedef struct {
    Vec4 p[38];           /* p[0..1] header, p[2..13] text, p[14..37] graph */
} HudParams;

/* ASCII into the param block: one character per float, 0 terminates. */
static void put_text(HudParams *hp, int base, const char *s)
{
    for (int i = 0; i < HUD_LINE_CHARS; i++) hp->p[base + (i >> 2)].v[i & 3] = 0.0f;
    if (!s) return;
    for (int i = 0; i < HUD_LINE_CHARS && s[i]; i++)
        hp->p[base + (i >> 2)].v[i & 3] = (float)(unsigned char)s[i];
}

void hud_draw(Hud *h, MgFrame *f, const HudRes *r,
              float w, float hgt, float frame_ms, float cpu_ms,
              const char *line1, const char *line2)
{
    if (w <= 0.0f || hgt <= 0.0f) return;

    HudParams hp;
    memset(&hp, 0, sizeof hp);

    const float margin = fmaxf(hgt * 0.012f, 8.0f);
    hp.p[0].v[0] = margin;
    hp.p[0].v[1] = margin;
    hp.p[0].v[2] = w - 2.0f * margin;
    hp.p[0].v[3] = hgt * 0.115f;

    hp.p[1].v[0] = 3.0f;              /* line 1 scale: 5x8 glyphs at 3x */
    hp.p[1].v[1] = 2.0f;
    hp.p[1].v[2] = frame_ms;
    hp.p[1].v[3] = frame_ms > 0.01f ? 1000.0f / frame_ms : 0.0f;

    char l1[40], l2[40];
    snprintf(l1, sizeof l1, "%s", line1 ? line1 : "");
    snprintf(l2, sizeof l2, "%s", line2 ? line2 : "");
    (void)cpu_ms;                     /* the caller formats its own line 2 */
    put_text(&hp, HUD_TEXT1, l1);
    put_text(&hp, HUD_TEXT2, l2);

    /* the rolling graph: real frame intervals, full scale 33.3 ms (30 fps) */
    if (h->n < HUD_SAMPLES) h->n++;
    h->hist[h->at] = frame_ms;
    h->at = (h->at + 1) % HUD_SAMPLES;
    for (uint32_t i = 0; i < HUD_SAMPLES; i++) {
        const uint32_t s = (h->at + i) % HUD_SAMPLES;
        float v = (i < h->n) ? h->hist[s] / 33.3333f : 0.5f;
        hp.p[HUD_GRAPH + (i >> 2)].v[i & 3] = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
    }

    mg_draw(f, &(MgDrawDesc){
        .pipe = r->pipe, .mat = r->mat, .mesh = r->quad, .state = r->state,
        .params = mg_const(f, &hp, sizeof hp), .instances = 1,
        .flags = MG_DRAW_ORDERED,     /* the overlay lands last, and always last */
    });
}
