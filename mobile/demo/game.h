/*
 * game.h — a small game, plus the frame-time HUD, drawn through mg.
 *
 * The game is deliberately ordinary (paddle, ball, bricks): what it is here to
 * show is the API's shape under a real frame — a few dozen draws that must keep
 * submission order, one overlay that must land last, and per-frame data written
 * into the param ring instead of into a texture.
 *
 * It owns no Vulkan: `GameRes` is five handles the application already created.
 */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>

#include "mg.h"

#define GAME_MAX_SAMPLES 96     /* frame intervals in the HUD graph */

typedef struct GameRes {
    MgPipe  sprite;             /* unit quad, solid colour, SDF rounded box  */
    MgPipe  hud;                /* the panel: frame-time graph + debug text  */
    MgMat   mat;                /* both pipes read no texture                */
    MgState state;              /* alpha blend, no depth                     */
    MgMesh  quad;               /* the unit quad: 4 vertices, 6 indices      */
} GameRes;

typedef struct Game {
    // layout, recomputed when the window size changes
    float w, h;
    float paddle_w, paddle_h, paddle_y, paddle_x;
    float ball_r;
    float brick_w, brick_h, brick_gap, brick_y0;

    // ball: stuck to the paddle until the first tap
    float bx, by, bvx, bvy;
    bool  live, over, won;

    uint8_t  brick[7 * 5];
    uint32_t bricks_left;
    int      score, lives, level;

    float flash;                /* 1 on a brick hit, decays                  */
    float touch_x;
    bool  touch_down;

    // HUD state
    float   hist[GAME_MAX_SAMPLES];
    uint32_t hist_at, hist_n;
    float   ms_draw, cpu_draw;  /* smoothed values shown on the panel        */
} Game;

void game_reset(Game *g);

// One fixed-ish step. `dt` in seconds, touch in pixels (y down).
void game_update(Game *g, float w, float h, float dt,
                 float finger_x, bool finger_down, bool tapped);

// Emits the game's ordered draws and then the HUD into the current pass.
void game_draw(Game *g, MgFrame *f, const GameRes *r,
               float frame_ms, float cpu_ms);

#endif // GAME_H
