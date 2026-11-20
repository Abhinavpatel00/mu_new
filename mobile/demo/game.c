/*
 * game.c — a small game and the frame-time HUD, expressed in mg draws.
 *
 * Everything here is pixels and floats. The only API calls are mg_draw() and
 * mg_const(): bricks, paddle and ball are one draw each, the HUD is one more,
 * and the whole debug overlay — live text and a 96-frame graph — costs 608
 * bytes of param slot per frame: no texture, no upload, no copy.
 *
 * The text is ASCII, handed to shaders/hud.frag as character codes; that shader
 * carries MicroProfile's 5x8 bitmap font and samples it per pixel.
 */

#include "game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define COLS 7
#define ROWS 5

// ============================================================
// 1. HUD PARAMS — the shader's struct, filled on the CPU
// ============================================================

#define HUD_TEXT1   2     // 24 characters = 6 vec4
#define HUD_TEXT2   8
#define HUD_GRAPH  14     // 96 samples = 24 vec4
#define HUD_LINE_CHARS 24

typedef struct { float v[4]; } Vec4;

typedef struct {
    Vec4 p[38];         // p[0..1] header, p[2..13] text, p[14..37] graph
} HudParams;

// ASCII into the param block: one character per float, 0 terminates.
static void put_text(HudParams *hp, int base, const char *s)
{
    for (int i = 0; i < HUD_LINE_CHARS; i++) hp->p[base + (i >> 2)].v[i & 3] = 0.0f;
    for (int i = 0; i < HUD_LINE_CHARS && s[i]; i++)
        hp->p[base + (i >> 2)].v[i & 3] = (float)(unsigned char)s[i];
}

// ============================================================
// 2. LAYOUT
// ============================================================

static void layout(Game *g, float w, float h)
{
    const float u = h / 1000.0f;         // one design unit, so it scales
    g->w = w;
    g->h = h;

    g->paddle_h = 16.0f * u;
    g->paddle_y = h - 70.0f * u;
    g->paddle_w = w * 0.24f;
    if (g->paddle_w < 90.0f * u)  g->paddle_w = 90.0f * u;
    if (g->paddle_w > 240.0f * u) g->paddle_w = 240.0f * u;
    g->ball_r   = 9.0f * u;

    g->brick_gap = 6.0f * u;
    g->brick_h   = h * 0.026f;
    // below the HUD panel, which is the top h/8 of the screen
    g->brick_y0  = h * 0.012f + h * 0.115f + h * 0.035f;
    if (g->brick_y0 < 8.0f) g->brick_y0 = 8.0f;

    const float area_w = w - 2.0f * (w * 0.06f);
    g->brick_w = (area_w - (COLS - 1) * g->brick_gap) / (float)COLS;

    if (g->paddle_x <= 0.0f) g->paddle_x = w * 0.5f;
    if (g->bx <= 0.0f)       g->bx = g->paddle_x;
    if (g->by <= 0.0f)       g->by = g->paddle_y - g->ball_r - 1.0f;
}

// ============================================================
// 3. GAME
// ============================================================

static float ball_speed(const Game *g)
{
    return g->h * 0.42f * (1.0f + 0.08f * (float)(g->level - 1));
}

static void stick(Game *g)
{
    g->live = false;
    g->bx = g->paddle_x;
    g->by = g->paddle_y - g->ball_r - 1.0f;
    g->bvx = g->bvy = 0.0f;
}

static void launch(Game *g)
{
    const float sp = ball_speed(g);
    // a cheap deterministic "random": alternates sides as the score grows
    const float side = ((g->score / 10 + g->lives) & 1) ? 1.0f : -1.0f;
    g->bvx = sp * 0.42f * side;
    g->bvy = -sp * 0.91f;
    g->live = true;
}

static void refill(Game *g)
{
    memset(g->brick, 1, sizeof g->brick);
    g->bricks_left = COLS * ROWS;
}

void game_reset(Game *g)
{
    if (g->w <= 0.0f) layout(g, 720.0f, 1548.0f);
    refill(g);
    g->score = 0;
    g->lives = 3;
    g->level = 1;
    g->over = false;
    g->won = false;
    g->flash = 0.0f;
    g->paddle_x = g->w * 0.5f;
    stick(g);
}

static void brick_rect(const Game *g, int c, int r, float *x0, float *y0)
{
    *x0 = g->w * 0.06f + (float)c * (g->brick_w + g->brick_gap);
    *y0 = g->brick_y0 + (float)r * (g->brick_h + g->brick_gap * 0.8f);
}

// One integration step small enough that the ball cannot pass a brick.
static void step(Game *g, float dt)
{
    g->bx += g->bvx * dt;
    g->by += g->bvy * dt;

    const float r = g->ball_r;

    if (g->bx - r < 0.0f)     { g->bx = r;          g->bvx =  fabsf(g->bvx); }
    if (g->bx + r > g->w)     { g->bx = g->w - r;   g->bvx = -fabsf(g->bvx); }
    if (g->by - r < 0.0f)     { g->by = r;          g->bvy =  fabsf(g->bvy); }

    // ---- bricks ----
    for (int i = 0; i < COLS * ROWS; i++) {
        if (!g->brick[i]) continue;
        const int c = i % COLS, rw = i / COLS;
        float x0, y0;
        brick_rect(g, c, rw, &x0, &y0);
        const float x1 = x0 + g->brick_w, y1 = y0 + g->brick_h;

        const float ox = fminf(g->bx + r, x1) - fmaxf(g->bx - r, x0);
        const float oy = fminf(g->by + r, y1) - fmaxf(g->by - r, y0);
        if (ox <= 0.0f || oy <= 0.0f) continue;

        g->brick[i] = 0;
        g->bricks_left--;
        g->score += 10 + 5 * rw;
        g->flash = 1.0f;

        if (ox < oy) {
            g->bvx = -g->bvx;
            g->bx += (g->bx < x0 + g->brick_w * 0.5f) ? -ox : ox;
        } else {
            g->bvy = -g->bvy;
            g->by += (g->by < y0 + g->brick_h * 0.5f) ? -oy : oy;
        }
        break;                                  // one brick per step, always
    }

    // ---- paddle ----
    if (g->bvy > 0.0f && g->by + r >= g->paddle_y && g->by - r <= g->paddle_y + g->paddle_h &&
        g->bx >= g->paddle_x - g->paddle_w * 0.5f - r &&
        g->bx <= g->paddle_x + g->paddle_w * 0.5f + r) {
        const float sp = sqrtf(g->bvx * g->bvx + g->bvy * g->bvy) * 1.02f;
        const float t  = (g->bx - g->paddle_x) / (g->paddle_w * 0.5f);   // -1..1
        const float tx = fmaxf(-1.0f, fminf(1.0f, t));
        g->bvx = sp * 0.72f * tx;
        g->bvy = -sqrtf(fmaxf(sp * sp - g->bvx * g->bvx, 1.0f));
        g->by  = g->paddle_y - r - 1.0f;
        g->flash = fmaxf(g->flash, 0.5f);
    }

    // ---- lost the ball ----
    if (g->by - r > g->h) {
        g->lives--;
        if (g->lives <= 0) {
            g->over = true;
            stick(g);
        } else {
            stick(g);
        }
    }
}

void game_update(Game *g, float w, float h, float dt,
                 float finger_x, bool finger_down, bool tapped)
{
    if (w != g->w || h != g->h) layout(g, w, h);
    if (dt > 0.05f) dt = 0.05f;              // a hitch must not teleport the ball

    g->flash *= 0.88f;

    if (tapped) {
        if (g->over) { game_reset(g); launch(g); }
        else if (!g->live) launch(g);
    }

    // paddle follows the finger, with a speed limit so it cannot teleport
    if (finger_down) {
        g->touch_down = true;
        g->touch_x = finger_x;
    } else {
        g->touch_down = false;
    }
    if (g->touch_down) {
        const float maxstep = 3200.0f * (g->h / 1000.0f) * dt;
        float d = g->touch_x - g->paddle_x;
        if (d >  maxstep) d =  maxstep;
        if (d < -maxstep) d = -maxstep;
        g->paddle_x += d;
    }
    const float half = g->paddle_w * 0.5f;
    if (g->paddle_x < half)      g->paddle_x = half;
    if (g->paddle_x > g->w-half) g->paddle_x = g->w - half;

    if (!g->live || g->over) { g->bx = g->paddle_x; g->by = g->paddle_y - g->ball_r - 1.0f; return; }

    const float sp = sqrtf(g->bvx * g->bvx + g->bvy * g->bvy);
    int n = 1 + (int)(sp * dt / (g->ball_r * 0.8f));
    if (n > 8) n = 8;
    const float sub = dt / (float)n;
    for (int i = 0; i < n; i++) step(g, sub);

    if (g->bricks_left == 0 && !g->over) {   // cleared: next level, faster
        g->level++;
        refill(g);
        stick(g);
    }
}

// ============================================================
// 4. DRAW
// ============================================================

static void sprite(MgFrame *f, const GameRes *r,
                   float cx, float cy, float hw, float hh,
                   float radius, float border, float glow,
                   float rr, float gg, float bb, float aa)
{
    const float p[12] = { cx, cy, hw, hh, rr, gg, bb, aa, radius, border, 0.0f, glow };
    mg_draw(f, &(MgDrawDesc){
        .pipe = r->sprite, .mat = r->mat, .mesh = r->quad, .state = r->state,
        .params = mg_const(f, p, sizeof p), .instances = 1,
        .flags = MG_DRAW_ORDERED,     // a game layer is painter's order, always
    });
}

void game_draw(Game *g, MgFrame *f, const GameRes *r, float frame_ms, float cpu_ms)
{
    if (g->w <= 0.0f) return;

    const float u = g->h / 1000.0f;
    const float flash = g->flash;

    // ---- bricks ----
    static const float row_rgb[ROWS][3] = {
        { 0.98f, 0.38f, 0.42f }, { 0.99f, 0.66f, 0.30f }, { 0.95f, 0.90f, 0.38f },
        { 0.38f, 0.88f, 0.55f }, { 0.42f, 0.70f, 0.98f },
    };
    for (int i = 0; i < COLS * ROWS; i++) {
        if (!g->brick[i]) continue;
        float x0, y0;
        brick_rect(g, i % COLS, i / COLS, &x0, &y0);
        const float b = 1.0f + flash * 0.25f;
        sprite(f, r, x0 + g->brick_w * 0.5f, y0 + g->brick_h * 0.5f,
               g->brick_w * 0.5f, g->brick_h * 0.5f,
               4.0f * u, 1.6f * u, 0.55f,
               row_rgb[i / COLS][0] * b, row_rgb[i / COLS][1] * b, row_rgb[i / COLS][2] * b, 1.0f);
    }

    // ---- paddle ----
    sprite(f, r, g->paddle_x, g->paddle_y + g->paddle_h * 0.5f,
           g->paddle_w * 0.5f, g->paddle_h * 0.5f,
           g->paddle_h * 0.5f, 2.0f * u, 0.9f,
           0.80f, 0.88f, 1.0f, 1.0f);

    // ---- ball (a ring while it waits for a tap) ----
    const float pulse = g->live ? 1.0f : 0.55f + 0.45f * sinf((float)g->hist_at * 0.25f);
    sprite(f, r, g->bx, g->by, g->ball_r, g->ball_r,
           g->ball_r, 3.0f * u, 1.0f * pulse + flash,
           1.0f, 1.0f, 1.0f, g->live ? 1.0f : 0.85f);

    // ---- HUD --------------------------------------------------------------
    HudParams hp;
    memset(&hp, 0, sizeof hp);

    const float margin = fmaxf(g->h * 0.012f, 8.0f);
    hp.p[0].v[0] = margin;
    hp.p[0].v[1] = margin;
    hp.p[0].v[2] = g->w - 2.0f * margin;
    hp.p[0].v[3] = g->h * 0.115f;

    const float fps = frame_ms > 0.01f ? 1000.0f / frame_ms : 0.0f;

    char line1[40], line2[40];
    snprintf(line1, sizeof line1, "%4.1f ms %3.0f fps", (double)frame_ms, (double)fps);
    snprintf(line2, sizeof line2, "cpu %.1f ms  score %d", (double)cpu_ms, g->score);
    put_text(&hp, HUD_TEXT1, line1);
    put_text(&hp, HUD_TEXT2, line2);

    hp.p[1].v[0] = 3.0f;          // text scale: 5x8 glyphs at 3x
    hp.p[1].v[1] = 2.0f;
    hp.p[1].v[2] = frame_ms;
    hp.p[1].v[3] = fps;

    // the rolling graph: 96 frame intervals, full scale 33.3 ms (30 fps)
    if (g->hist_n < GAME_MAX_SAMPLES) g->hist_n++;
    g->hist[g->hist_at] = frame_ms;
    g->hist_at = (g->hist_at + 1) % GAME_MAX_SAMPLES;
    for (uint32_t i = 0; i < GAME_MAX_SAMPLES; i++) {
        const uint32_t s = (g->hist_at + i) % GAME_MAX_SAMPLES;
        float v = (i < g->hist_n) ? g->hist[s] / 33.3333f : 0.5f;
        hp.p[HUD_GRAPH + (i >> 2)].v[i & 3] = v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
    }

    mg_draw(f, &(MgDrawDesc){
        .pipe = r->hud, .mat = r->mat, .mesh = r->quad, .state = r->state,
        .params = mg_const(f, &hp, sizeof hp), .instances = 1,
        .flags = MG_DRAW_ORDERED,     // the overlay lands last, and always last
    });
}
