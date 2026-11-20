/*
 * mobile_main.c — an Android demo for the mg API: a living island.
 *
 * What it does, and why each thing is in it:
 *
 *   - a NativeActivity with no Java at all: the whole app is this file, mg.c
 *     and the shaders
 *   - surface lifecycle handled the way Android actually delivers it
 *     (INIT/TERM_WINDOW arrive at arbitrary times, and TERM_WINDOW means the
 *     window is already gone)
 *   - three passes declared once at startup: a reflection pass whose output is
 *     sampled by the pond a moment later in the same frame, a 4x MSAA scene
 *     pass resolved in-pass and never stored to memory, and a post pass that
 *     composites and then draws the HUD
 *   - one 208-byte frame constant block: one camera, one sun, one sky, shared
 *     by every shader in every pass
 *   - an 8-byte push per draw and a per-draw param slot in the ring: a tree is
 *     48 bytes, a creature 48 bytes, and neither has a matrix
 *   - touch: drag to orbit, tap a creature to pick it up, tap the ground to
 *     put it down
 *
 * Frame body in one sentence: record three pass lists, sort them, record one
 * command buffer, one submit, one present.
 */

#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <signal.h>
#include <time.h>
#include <unwind.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "eco.h"
#include "hud.h"
#include "mg.h"
#include "shaders/spirv.h"

#define TAG       "mg-demo"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__))

// ---------------------------------------------------------------
// A silent crash is a crash nobody can fix. Every phase of startup
// stamps a step number, and any fatal signal prints the step it was
// on plus a backtrace. "adb logcat -s mg-demo" then says where.
// ---------------------------------------------------------------

static volatile sig_atomic_t g_step;
static const char           *g_step_name = "not started";

#define STEP(n, name)                                                                                                  \
    do {                                                                                                               \
        g_step_name = (name);                                                                                          \
        g_step      = (n);                                                                                             \
        LOGI("step %d/%d: %s", (int)(n), 12, (name));                                                                  \
    } while (0)

struct Bt {
    void **p;
    int    n, max;
};

static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *arg) {
    struct Bt *b  = arg;
    uintptr_t  ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (ip && b->n < b->max)
        b->p[b->n++] = (void *)ip;
    return _URC_NO_REASON;
}

static void crash_handler(int sig) {
    void     *frames[40];
    struct Bt b = {frames, 0, 40};
    _Unwind_Backtrace(bt_cb, &b);
    __android_log_print(ANDROID_LOG_ERROR, TAG, "==== FATAL SIGNAL %d at step %d (%s) ====", sig, (int)g_step,
                        g_step_name);
    for (int i = 0; i < b.n; i++) {
        Dl_info info;
        if (dladdr(frames[i], &info) && info.dli_fname)
            __android_log_print(ANDROID_LOG_ERROR, TAG, "  #%02d %p %s+0x%lx", i, frames[i], info.dli_fname,
                                (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase));
        else
            __android_log_print(ANDROID_LOG_ERROR, TAG, "  #%02d %p", i, frames[i]);
    }
    __android_log_print(ANDROID_LOG_ERROR, TAG, "resymbolicate: adb logcat | ndk-stack -sym libmobile.so");
    signal(sig, SIG_DFL);
    raise(sig);
}

// Proves the library was actually dlopen'd and its initialisers ran: if this
// line is missing from logcat, nothing below it ever existed.
__attribute__((constructor)) static void on_library_load(void) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "libmobile.so loaded and initialised");
}

typedef struct App {
    MgDevice           *d;
    struct android_app *app;

    bool have_surface, focused, started;

    // three passes: reflection -> scene -> post (composite + HUD)
    MgPass  pass_reflect, pass_scene, pass_post;
    MgPipe  pipe_post, pipe_hud;
    MgState st_post, st_ui;
    MgMat   mat_post, mat_ui;
    MgTex   tex_reflect, tex_scene;
    MgMesh  mesh_fullscreen, mesh_quad;

    EcoRes res;
    Eco   *eco;
    Hud    hud;
    HudRes hudres;

    // one finger: drag orbits the camera, a tap picks up or puts down a creature
    float finger_x, finger_y;
    float last_x, last_y;
    float drag_x, drag_y;
    bool  finger_down, moved;
    bool  tap_ready;
    float tap_x, tap_y;

    float last_frame_ms, last_cpu_ms;

    double   t0;
    uint32_t stats_frames;
    uint32_t frames;
    double   fps_t0, cpu_ms_sum;
} App;

// ============================================================
// 1. BUILD
// ============================================================

static MgPipe mkpipe(MgDevice *d, MgPass pass, const uint32_t *vs, uint32_t vsn, const uint32_t *fs, uint32_t fsn,
                     uint32_t streams, MgFmt color, MgFmt depth, uint32_t samples) {
    return mg_pipe(d, &(MgPipeDesc){
                          .pass        = pass,
                          .vs          = {vs, vsn},
                          .fs          = {fs, fsn},
                          .streams     = streams,
                          .color       = {color},
                          .color_count = 1,
                          .depth       = depth,
                          .samples     = samples,
                          .topology    = MG_TRIANGLES,
                      });
}

static void app_build(App *s) {
    MgDevice *d = s->d;

    // ---- passes, declared once -------------------------------------------------
    // The reflection pass has no MSAA: it is a small, blurry thing that will be
    // wobbled by the water anyway. Its colour attachment is left in
    // SHADER_READ_ONLY by the pass itself, which is what lets the pond sample it
    // later in the same frame without the application owning a barrier.
    MgPassDesc reflect = {
        .name          = "reflect",
        .color         = {{.fmt = MG_RGBA8, .load = MG_CLEAR, .store = MG_STORE}},
        .color_count   = 1,
        .depth         = MG_D24S8,
        .depth_load    = MG_CLEAR,
        .depth_store   = MG_STORE_NONE,
        .samples       = 1,
        .resolve       = false,
        .subpass_count = 1,
    };
    MgPassDesc scene = {
        .name          = "scene",
        .color         = {{.fmt = MG_RGBA8, .load = MG_CLEAR, .store = MG_STORE}},
        .color_count   = 1,
        .depth         = MG_D24S8,
        .depth_load    = MG_CLEAR,
        .depth_store   = MG_STORE_NONE, // never leaves tile memory
        .samples       = 4,
        .resolve       = true, // in-pass, never vkCmdResolveImage
        .subpass_count = 1,
    };
    MgPassDesc post = {
        .name          = "post",
        .color         = {{.fmt = MG_SWAPCHAIN, .load = MG_DONT_CARE, .store = MG_STORE}},
        .color_count   = 1,
        .depth         = MG_FMT_NONE,
        .subpass_count = 1,
    };
    s->pass_reflect = mg_pass(d, &reflect);
    s->pass_scene   = mg_pass(d, &scene);
    s->pass_post    = mg_pass(d, &post);
    s->tex_reflect  = mg_pass_output(d, s->pass_reflect);
    s->tex_scene    = mg_pass_output(d, s->pass_scene);

    // ---- the closed set of states ----------------------------------------------
    s->res.st_sky = mg_state(d, &(MgStateDesc){.blend      = MG_BLEND_NONE,
                                               .depth      = MG_DEPTH_READ,
                                               .cmp        = MG_CMP_LESS,
                                               .cull       = MG_CULL_NONE,
                                               .write_mask = 0xf});
    s->res.st_solid = mg_state(d, &(MgStateDesc){.blend      = MG_BLEND_NONE,
                                                 .depth      = MG_DEPTH_READ_WRITE,
                                                 .cmp        = MG_CMP_LESS,
                                                 .cull       = MG_CULL_BACK,
                                                 .write_mask = 0xf});
    s->res.st_solid_nocull = mg_state(d, &(MgStateDesc){.blend      = MG_BLEND_NONE,
                                                        .depth      = MG_DEPTH_READ_WRITE,
                                                        .cmp        = MG_CMP_LESS,
                                                        .cull       = MG_CULL_NONE,
                                                        .write_mask = 0xf});
    s->res.st_grass = mg_state(d, &(MgStateDesc){.blend      = MG_BLEND_NONE,
                                                 .depth      = MG_DEPTH_READ_WRITE,
                                                 .cmp        = MG_CMP_LESS,
                                                 .cull       = MG_CULL_NONE, // a blade has no back face
                                                 .write_mask = 0xf});
    s->res.st_cutout = mg_state(d, &(MgStateDesc){.blend      = MG_BLEND_ALPHA,
                                                  .depth      = MG_DEPTH_READ,
                                                  .cmp        = MG_CMP_LESS,
                                                  .cull       = MG_CULL_NONE,
                                                  .write_mask = 0xf});
    s->res.st_water = mg_state(d, &(MgStateDesc){.blend      = MG_BLEND_ALPHA,
                                                  .depth      = MG_DEPTH_READ,
                                                  .cmp        = MG_CMP_LESS,
                                                  .cull       = MG_CULL_NONE,
                                                  .write_mask = 0xf});
    s->st_post = mg_state(d, &(MgStateDesc){.blend      = MG_BLEND_NONE,
                                            .depth      = MG_DEPTH_NONE,
                                            .cmp        = MG_CMP_LESS,
                                            .cull       = MG_CULL_NONE,
                                            .write_mask = 0xf});
    s->st_ui   = mg_state(d, &(MgStateDesc){.blend      = MG_BLEND_ALPHA,
                                            .depth      = MG_DEPTH_NONE,
                                            .cmp        = MG_CMP_LESS,
                                            .cull       = MG_CULL_NONE,
                                            .write_mask = 0xf});

    // ---- materials: the world is procedural, the pond is the reflection -------
    s->mat_ui      = mg_material(d, &(MgMatDesc){.slots = 0});
    s->res.mat     = s->mat_ui;
    s->res.mat_water = mg_material(d, &(MgMatDesc){.tex = {s->tex_reflect}, .slots = 1});
    s->mat_post      = mg_material(d, &(MgMatDesc){.tex = {s->tex_scene}, .slots = 1});

    // ---- geometry: one fullscreen triangle, one unit quad, and nothing else ----
    // The island, the trees, the grass and the pond are built by eco.c, which
    // owns them the way a renderer's asset registry would.
    s->mesh_fullscreen = mg_mesh(d, &(MgMeshDesc){.positions = NULL, .indices_u32 = false, .index_count = 3});
    static const float    quad_pos[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    static const uint16_t quad_idx[6]  = {0, 1, 2, 0, 2, 3};
    s->mesh_quad                       = mg_mesh(d, &(MgMeshDesc){.positions    = quad_pos,
                                                                  .vertex_count = 4,
                                                                  .indices      = quad_idx,
                                                                  .index_count  = 6,
                                                                  .indices_u32  = false});
    s->res.mesh_quad = s->mesh_quad;
    s->res.mesh_sky  = s->mesh_fullscreen;

    // ---- pipes: six for the scene, five for its reflection, two for the frame --
    s->res.sky      = mkpipe(d, s->pass_scene, spirv_sky_vert, sizeof spirv_sky_vert / 4, spirv_sky_frag,
                             sizeof spirv_sky_frag / 4, 0, MG_RGBA8, MG_D24S8, 4);
    s->res.terrain  = mkpipe(d, s->pass_scene, spirv_terrain_vert, sizeof spirv_terrain_vert / 4, spirv_terrain_frag,
                             sizeof spirv_terrain_frag / 4, 2, MG_RGBA8, MG_D24S8, 4);
    s->res.prop     = mkpipe(d, s->pass_scene, spirv_prop_vert, sizeof spirv_prop_vert / 4, spirv_prop_frag,
                             sizeof spirv_prop_frag / 4, 2, MG_RGBA8, MG_D24S8, 4);
    s->res.grass    = mkpipe(d, s->pass_scene, spirv_grass_vert, sizeof spirv_grass_vert / 4, spirv_grass_frag,
                             sizeof spirv_grass_frag / 4, 2, MG_RGBA8, MG_D24S8, 4);
    s->res.creature = mkpipe(d, s->pass_scene, spirv_creature_vert, sizeof spirv_creature_vert / 4,
                             spirv_creature_frag, sizeof spirv_creature_frag / 4, 1, MG_RGBA8, MG_D24S8, 4);
    s->res.water    = mkpipe(d, s->pass_scene, spirv_water_vert, sizeof spirv_water_vert / 4, spirv_water_frag,
                             sizeof spirv_water_frag / 4, 2, MG_RGBA8, MG_D24S8, 4);

    s->res.r_sky      = mkpipe(d, s->pass_reflect, spirv_sky_vert_mirror, sizeof spirv_sky_vert_mirror / 4,
                               spirv_sky_frag_mirror, sizeof spirv_sky_frag_mirror / 4, 0, MG_RGBA8, MG_D24S8, 1);
    s->res.r_terrain  = mkpipe(d, s->pass_reflect, spirv_terrain_vert_mirror, sizeof spirv_terrain_vert_mirror / 4,
                               spirv_terrain_frag_mirror, sizeof spirv_terrain_frag_mirror / 4, 2, MG_RGBA8, MG_D24S8, 1);
    s->res.r_prop     = mkpipe(d, s->pass_reflect, spirv_prop_vert_mirror, sizeof spirv_prop_vert_mirror / 4,
                               spirv_prop_frag_mirror, sizeof spirv_prop_frag_mirror / 4, 2, MG_RGBA8, MG_D24S8, 1);
    s->res.r_grass    = mkpipe(d, s->pass_reflect, spirv_grass_vert_mirror, sizeof spirv_grass_vert_mirror / 4,
                               spirv_grass_frag_mirror, sizeof spirv_grass_frag_mirror / 4, 2, MG_RGBA8, MG_D24S8, 1);
    s->res.r_creature = mkpipe(d, s->pass_reflect, spirv_creature_vert_mirror, sizeof spirv_creature_vert_mirror / 4,
                               spirv_creature_frag_mirror, sizeof spirv_creature_frag_mirror / 4, 1, MG_RGBA8,
                               MG_D24S8, 1);

    s->pipe_post = mkpipe(d, s->pass_post, spirv_composite_vert, sizeof spirv_composite_vert / 4,
                          spirv_composite_frag, sizeof spirv_composite_frag / 4, 0, MG_SWAPCHAIN, MG_FMT_NONE, 1);
    s->pipe_hud  = mkpipe(d, s->pass_post, spirv_hud_vert, sizeof spirv_hud_vert / 4, spirv_hud_frag,
                          sizeof spirv_hud_frag / 4, 1, MG_SWAPCHAIN, MG_FMT_NONE, 1);
    s->hudres.pipe  = s->pipe_hud;
    s->hudres.mat   = s->mat_ui;
    s->hudres.state = s->st_ui;
    s->hudres.quad  = s->mesh_quad;

    mg_warm(d, true);

    // ---- the world -------------------------------------------------------------
    float ww = 1080.0f, hh = 2400.0f;
    if (s->app->window) {
        ww = (float)ANativeWindow_getWidth(s->app->window);
        hh = (float)ANativeWindow_getHeight(s->app->window);
    }
    s->eco = eco_create(d, ww, hh);
    eco_set_rotation(s->eco, mg_display_rotation(d));
    LOGI("eco: island built, %d creatures, %.0fx%.0f", eco_population(s->eco), (double)ww, (double)hh);
}

// ============================================================
// 2. FRAME
// ============================================================

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void app_frame(App *s, double t, double dt_frame) {
    MgFrame *f;
    if (!mg_frame(s->d, &f))
        return;
    if (!s->eco)
        return;

    float ww = 1080.0f, hh = 2400.0f;
    if (s->app->window) {
        ww = (float)ANativeWindow_getWidth(s->app->window);
        hh = (float)ANativeWindow_getHeight(s->app->window);
    }

    // ---- the world tick: touch in, draws out ---------------------------------
    EcoInput in = {
        .x      = s->finger_x,
        .y      = s->finger_y,
        .down   = s->finger_down,
        .tap    = s->tap_ready,
        .drag_dx = s->drag_x,
        .drag_dy = s->drag_y,
    };
    if (s->tap_ready) {
        in.x = s->tap_x;
        in.y = s->tap_y;
    }
    eco_update(s->eco, (float)dt_frame, ww, hh, in);
    s->drag_x = s->drag_y = 0.0f;
    s->tap_ready = false;

    // ---- one frame of constants: camera, sun, sky, water plane ---------------
    eco_set_frame(s->eco, f, (float)t, ww, hh);

    // ---- pass 1: the island mirrored about the pond, same camera -------------
    if (!(eco_skip_mask() & 64u)) {
        mg_pass_begin(f, s->pass_reflect);
        eco_emit_reflection(s->eco, f, &s->res);
        mg_pass_end(f);
    }

    // ---- pass 2: the island, drawn with 4x MSAA and resolved in-pass ---------
    mg_pass_begin(f, s->pass_scene);
    eco_emit_scene(s->eco, f, &s->res);
    mg_pass_end(f);

    // ---- pass 3: composite, then the HUD ------------------------------------
    mg_pass_begin(f, s->pass_post);
    {
        const float p[8] = {0, 0, 1, 0, 1, 1, 1, 1};
        mg_draw(f, &(MgDrawDesc){
                       .pipe      = s->pipe_post,
                       .mat       = s->mat_post,
                       .mesh      = s->mesh_fullscreen,
                       .state     = s->st_post,
                       .params    = mg_const(f, p, sizeof p),
                       .instances = 1,
                   });

        char l1[40], l2[40];
        const float ms = s->last_frame_ms;
        snprintf(l1, sizeof l1, "%4.1f fps %4.1f ms", ms > 0.01f ? 1000.0f / ms : 0.0f, (double)ms);
        eco_status(s->eco, l2, sizeof l2);
        if (!(eco_skip_mask() & 128u))
            hud_draw(&s->hud, f, &s->hudres, ww, hh, ms, s->last_cpu_ms, l1, l2);
    }
    mg_pass_end(f);

    mg_frame_end(s->d, f);
}

// ============================================================
// 3. ANDROID GLUE
// ============================================================

static void handle_cmd(struct android_app *app, int32_t cmd) {
    App *s = app->userData;
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        STEP(4, "APP_CMD_INIT_WINDOW");
        if (app->window) {
            if (s->have_surface)
                mg_surface_lost(s->d);
            STEP(5, "creating surface + swapchain");
            mg_surface(s->d, app->window);
            s->have_surface = true;
            if (!s->started) {
                STEP(6, "building passes, pipes, materials, geometry, island");
                app_build(s);
                STEP(9, "objects built and warmed");
                s->started = true;
            }
            LOGI("window up");
        } else {
            LOGE("INIT_WINDOW with a null window");
        }
        break;
    case APP_CMD_TERM_WINDOW:
        if (s->have_surface) {
            mg_surface_lost(s->d); // the window is already gone here
            s->have_surface = false;
        }
        LOGI("window down");
        break;
    case APP_CMD_GAINED_FOCUS:
        s->focused = true;
        break;
    case APP_CMD_LOST_FOCUS:
        s->focused = false;
        break;
    case APP_CMD_WINDOW_REDRAW_NEEDED:
    case APP_CMD_CONFIG_CHANGED:
        if (s->have_surface)
            mg_resize(s->d, 0, 0);
        break;
    default:
        break;
    }
}

// One finger, and two things it can mean: drag to orbit, or tap to collect.
static int32_t handle_input(struct android_app *app, AInputEvent *ev) {
    App *s = app->userData;
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION)
        return 0;

    const int32_t action = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
    const float   x      = AMotionEvent_getX(ev, 0);
    const float   y      = AMotionEvent_getY(ev, 0);

    switch (action) {
    case AMOTION_EVENT_ACTION_DOWN:
        s->finger_x = s->last_x = x;
        s->finger_y = s->last_y = y;
        s->finger_down = true;
        s->moved       = false;
        return 1;
    case AMOTION_EVENT_ACTION_MOVE:
        s->finger_x = x;
        s->finger_y = y;
        if (fabsf(x - s->last_x) + fabsf(y - s->last_y) > 3.0f)
            s->moved = true;
        s->drag_x += x - s->last_x;
        s->drag_y += y - s->last_y;
        s->last_x = x;
        s->last_y = y;
        return 1;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        s->finger_x = x;
        s->finger_y = y;
        s->finger_down = false;
        if (!s->moved && action == AMOTION_EVENT_ACTION_UP) {
            s->tap_ready = true;
            s->tap_x     = x;
            s->tap_y     = y;
        }
        s->drag_x = s->drag_y = 0.0f;
        return 1;
    default:
        return 0;
    }
}

void android_main(struct android_app *app) {
    static App s;
    memset(&s, 0, sizeof s);
    s.app    = app;
    s.t0     = now_s();
    s.fps_t0 = s.t0;

    app->userData     = &s;
    app->onAppCmd     = handle_cmd;
    app->onInputEvent = handle_input;

    STEP(1, "android_main entered");
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGILL, crash_handler);

    STEP(2, "mg_create (instance, device, heap, descriptors)");
    s.d = mg_create(&(MgDesc){
        .name  = "mg-demo",
        .vsync = true,
        // Khronos validation. CORE is API misuse only and leaves the frame at
        // 60 fps, which is what a game needs; MG_VALIDATE_ALL adds synchronisation
        // hazards and best-practices advice, costs 2-10x more. Needs the layer on
        // the device (sh demo/vvl_install.sh). Set 0 for measurement runs.
        .validation       = 0,   // measurement run: no validation layer
        .frames_in_flight = MG_FRAMES_IN_FLIGHT,
        .arena_bytes      = 8u << 20,
    });
    STEP(3, "mg_create done");

    double       next = now_s();
    const double dt   = 1.0 / 60.0;
    double       cpu0 = 0.0, last_t = now_s(), frame_prev = now_s();

    for (;;) {
        int                         events;
        struct android_poll_source *src;
        const double                now     = now_s();
        int                         timeout = (int)((next - now) * 1000.0);
        if (timeout < 0)
            timeout = 0;
        if (!s.have_surface || !s.focused)
            timeout = -1; // sleep until an event

        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&src) >= 0) {
            if (src)
                src->process(app, src);
            if (app->destroyRequested) {
                mg_destroy(s.d);
                return;
            }
            timeout = 0;
        }
        if (app->destroyRequested)
            break;

        const double t = now_s();
        if (s.have_surface && s.focused && s.started && t >= next) {
            next += dt;
            if (next < t)
                next = t + dt;

            if (s.stats_frames == 0)
                STEP(10, "first frame");
            // the interval the HUD graphs: real wall time between frames, so a
            // missed vsync shows up even when the CPU work was cheap
            s.last_frame_ms = s.stats_frames ? (float)((t - last_t) * 1000.0) : 16.6667f;
            last_t          = t;
            cpu0            = now_s();
            app_frame(&s, t - s.t0, t - frame_prev);
            frame_prev = t;
            s.stats_frames++;
            const double cpu_ms = (now_s() - cpu0) * 1000.0;
            s.last_cpu_ms       = (float)cpu_ms; // one frame of lag, invisible

            s.frames++;
            s.cpu_ms_sum += cpu_ms;
            if (s.frames % 60 == 0) {
                const double   el = now_s() - s.fps_t0;
                const MgStats *st = mg_stats(s.d);
                LOGI("%5.1f fps | cpu %.2f ms | draws %u | pipes %u | mats %u | pushes %u "
                     "| sort %.0f us | arena %llu B | val %ue %uw",
                     60.0 / el, s.cpu_ms_sum / 60.0, st->draws, st->pipe_binds, st->mat_binds, st->push_count,
                     (double)st->sort_us, (unsigned long long)st->arena_used, st->val_errors, st->val_warnings);
                s.fps_t0     = now_s();
                s.frames     = 0;
                s.cpu_ms_sum = 0.0;
            }
        }
    }
    mg_destroy(s.d);
}
