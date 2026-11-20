/*
 * platform.c — platform.h on GLFW. No other file includes GLFW.
 *
 *   INPUT     Keys, mouse buttons and the wheel arrive as GLFW callbacks during
 *             platform_poll() and are queued in the MuInputState as events. A
 *             press and a release that both land between two frames are both
 *             seen, so a tap shorter than a frame still counts once, without
 *             sticky keys. The cursor is a state, not an event: it is read
 *             once per poll.
 *
 *   SLOTS     mu_input reads a key through a control slot the backend picks.
 *             Here a key's slot is its GLFW key token. A token names a place
 *             on the keyboard (the key that is W on a US layout), not the
 *             letter printed on it, and it is the same number on every
 *             system. Mouse buttons take the slots mu_input keeps for them.
 *
 *   HEADLESS  A headless platform is GLFW's null platform: the same calls,
 *             made on a window that is never shown, receives nothing and is
 *             never closed. Nothing below asks whether there is a window.
 *
 *   SIZE      The window's size in pixels is read once per poll, like the
 *             cursor. On X11 a Vulkan surface knows it too. On Wayland only
 *             the window does, which is why platform_size() exists.
 *
 *   WAYLAND   Which window system GLFW is built for is the Makefile's choice
 *             (make WAYLAND=1). With -DPLATFORM_NO_LIBDECOR, GLFW is told not
 *             to load libdecor for the window frame: the compositor draws the
 *             frame where it offers to, and GLFW a plain one where it does not.
 *
 *   FAILURE   Setup failure is fatal where it happens: the message says what
 *             failed and the process exits. Callers do not re-check.
 *
 * Not forwarded yet: gamepads, text entry, and keys GLFW has no token for.
 */

#include "external/mu_lib/mu/mu_macros.h" /* first: owns the POSIX feature test */
#include "external/mu_lib/mu/mu_input.h"
#include "external/mu_lib/mu/mu_time.h"

#include "platform.h"

#define GLFW_INCLUDE_NONE /* no OpenGL headers; platform.h brought the Vulkan ones */
#include "external/glfw/include/GLFW/glfw3.h"

#include <stdio.h>
#include <stdlib.h>

struct Platform {
    GLFWwindow  *window;
    uint64_t     now;           /* one clock reading per poll: every event of that poll carries it */
    int          width, height; /* the window in pixels, as of the last poll */
    MuInputState input;
};

/* MuKey -> GLFW key token. Both name a key by where it sits on a US keyboard. */
static const uint16_t KEY_TOKENS[MU_KEY_COUNT] = {
    [MU_KEY_ESCAPE] = GLFW_KEY_ESCAPE, [MU_KEY_SPACE] = GLFW_KEY_SPACE, [MU_KEY_TAB] = GLFW_KEY_TAB, [MU_KEY_ENTER] = GLFW_KEY_ENTER, [MU_KEY_BACKSPACE] = GLFW_KEY_BACKSPACE,
    [MU_KEY_DELETE] = GLFW_KEY_DELETE, [MU_KEY_INSERT] = GLFW_KEY_INSERT, [MU_KEY_HOME] = GLFW_KEY_HOME, [MU_KEY_END] = GLFW_KEY_END,
    [MU_KEY_PAGE_UP] = GLFW_KEY_PAGE_UP, [MU_KEY_PAGE_DOWN] = GLFW_KEY_PAGE_DOWN,
    [MU_KEY_LEFT] = GLFW_KEY_LEFT, [MU_KEY_RIGHT] = GLFW_KEY_RIGHT, [MU_KEY_UP] = GLFW_KEY_UP, [MU_KEY_DOWN] = GLFW_KEY_DOWN,

    [MU_KEY_LEFT_SHIFT] = GLFW_KEY_LEFT_SHIFT, [MU_KEY_RIGHT_SHIFT] = GLFW_KEY_RIGHT_SHIFT, [MU_KEY_LEFT_CTRL] = GLFW_KEY_LEFT_CONTROL, [MU_KEY_RIGHT_CTRL] = GLFW_KEY_RIGHT_CONTROL,
    [MU_KEY_LEFT_ALT] = GLFW_KEY_LEFT_ALT, [MU_KEY_RIGHT_ALT] = GLFW_KEY_RIGHT_ALT,

    [MU_KEY_MINUS] = GLFW_KEY_MINUS, [MU_KEY_EQUAL] = GLFW_KEY_EQUAL, [MU_KEY_LEFT_BRACKET] = GLFW_KEY_LEFT_BRACKET, [MU_KEY_RIGHT_BRACKET] = GLFW_KEY_RIGHT_BRACKET,
    [MU_KEY_COMMA] = GLFW_KEY_COMMA, [MU_KEY_PERIOD] = GLFW_KEY_PERIOD, [MU_KEY_SLASH] = GLFW_KEY_SLASH, [MU_KEY_SEMICOLON] = GLFW_KEY_SEMICOLON,
    [MU_KEY_APOSTROPHE] = GLFW_KEY_APOSTROPHE, [MU_KEY_BACKSLASH] = GLFW_KEY_BACKSLASH, [MU_KEY_GRAVE] = GLFW_KEY_GRAVE_ACCENT,

    [MU_KEY_F1] = GLFW_KEY_F1, [MU_KEY_F2] = GLFW_KEY_F2, [MU_KEY_F3] = GLFW_KEY_F3, [MU_KEY_F4]  = GLFW_KEY_F4,  [MU_KEY_F5]  = GLFW_KEY_F5,  [MU_KEY_F6]  = GLFW_KEY_F6,
    [MU_KEY_F7] = GLFW_KEY_F7, [MU_KEY_F8] = GLFW_KEY_F8, [MU_KEY_F9] = GLFW_KEY_F9, [MU_KEY_F10] = GLFW_KEY_F10, [MU_KEY_F11] = GLFW_KEY_F11, [MU_KEY_F12] = GLFW_KEY_F12,

    [MU_KEY_A] = GLFW_KEY_A, [MU_KEY_B] = GLFW_KEY_B, [MU_KEY_C] = GLFW_KEY_C, [MU_KEY_D] = GLFW_KEY_D, [MU_KEY_E] = GLFW_KEY_E, [MU_KEY_F] = GLFW_KEY_F, [MU_KEY_G] = GLFW_KEY_G,
    [MU_KEY_H] = GLFW_KEY_H, [MU_KEY_I] = GLFW_KEY_I, [MU_KEY_J] = GLFW_KEY_J, [MU_KEY_K] = GLFW_KEY_K, [MU_KEY_L] = GLFW_KEY_L, [MU_KEY_M] = GLFW_KEY_M, [MU_KEY_N] = GLFW_KEY_N,
    [MU_KEY_O] = GLFW_KEY_O, [MU_KEY_P] = GLFW_KEY_P, [MU_KEY_Q] = GLFW_KEY_Q, [MU_KEY_R] = GLFW_KEY_R, [MU_KEY_S] = GLFW_KEY_S, [MU_KEY_T] = GLFW_KEY_T, [MU_KEY_U] = GLFW_KEY_U,
    [MU_KEY_V] = GLFW_KEY_V, [MU_KEY_W] = GLFW_KEY_W, [MU_KEY_X] = GLFW_KEY_X, [MU_KEY_Y] = GLFW_KEY_Y, [MU_KEY_Z] = GLFW_KEY_Z,

    [MU_KEY_0] = GLFW_KEY_0, [MU_KEY_1] = GLFW_KEY_1, [MU_KEY_2] = GLFW_KEY_2, [MU_KEY_3] = GLFW_KEY_3, [MU_KEY_4] = GLFW_KEY_4,
    [MU_KEY_5] = GLFW_KEY_5, [MU_KEY_6] = GLFW_KEY_6, [MU_KEY_7] = GLFW_KEY_7, [MU_KEY_8] = GLFW_KEY_8, [MU_KEY_9] = GLFW_KEY_9,
};

MU_STATIC_ASSERT(GLFW_KEY_LAST < MU_INPUT_MOUSE_SLOT, "key tokens stay below the mouse slots");
MU_STATIC_ASSERT(GLFW_MOUSE_BUTTON_LAST < MU_INPUT_MOUSE_SLOTS, "every GLFW mouse button has a slot");
MU_STATIC_ASSERT(GLFW_MOUSE_BUTTON_LEFT == MU_MOUSE_LEFT && GLFW_MOUSE_BUTTON_RIGHT == MU_MOUSE_RIGHT && GLFW_MOUSE_BUTTON_MIDDLE == MU_MOUSE_MIDDLE,
                 "GLFW and mu_input number the mouse buttons alike");

/* GLFW keeps the reason of the last call that failed. */
static _Noreturn void platform_fatal(const char *what)
{
    const char *why;

    glfwGetError(&why);
    fprintf(stderr, "platform: fatal: %s: %s\n", what, why);
    exit(1);
}

/* ---- events: GLFW calls these from glfwPollEvents(), in the order things happened ---- */

static void on_key(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    Platform *p = glfwGetWindowUserPointer(window);

    MU_UNUSED(scancode);
    MU_UNUSED(mods);
    /* a held key repeats, and mu_input wants its two edges only; a key GLFW cannot name has no slot */
    if (key != GLFW_KEY_UNKNOWN && action != GLFW_REPEAT)
        mu_input_push_control(&p->input, (uint32_t)key, action == GLFW_PRESS, p->now);
}

static void on_mouse_button(GLFWwindow *window, int button, int action, int mods)
{
    Platform *p = glfwGetWindowUserPointer(window);

    MU_UNUSED(mods);
    mu_input_push_control(&p->input, MU_INPUT_MOUSE_SLOT_OF(button), action == GLFW_PRESS, p->now);
}

static void on_scroll(GLFWwindow *window, double x, double y)
{
    Platform *p = glfwGetWindowUserPointer(window);

    /* GLFW counts a scroll to the left as positive, mu_input one to the right */
    mu_input_wheel_add(&p->input, -(float)x, (float)y);
}

/* ---- platform.h ------------------------------------------------------------------- */

Platform *platform_create(const PlatformDesc *desc)
{
    Platform *p = calloc(1, sizeof *p);
    double    x, y;
    uint32_t  key;

    mu_input_init(&p->input);
    for (key = MU_KEY_UNKNOWN + 1; key < MU_KEY_COUNT; ++key)
    {
        MU_ASSERT(KEY_TOKENS[key]); /* a MuKey that has no row in the table yet */
        mu_input_key_set_slot(&p->input, (MuKey)key, KEY_TOKENS[key]);
    }

    /* headless is a platform of GLFW's own: see the top of the file */
    glfwInitHint(GLFW_PLATFORM, desc->headless ? GLFW_PLATFORM_NULL : GLFW_ANY_PLATFORM);
#ifdef PLATFORM_NO_LIBDECOR
    glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_DISABLE_LIBDECOR);
#endif
    if (!glfwInit())
        platform_fatal("no window system");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); /* Vulkan draws into it: no OpenGL context */
    p->window = glfwCreateWindow((int)desc->width, (int)desc->height, desc->title, NULL, NULL);
    if (!p->window)
        platform_fatal("cannot open a window");

    glfwSetWindowUserPointer(p->window, p);
    glfwSetKeyCallback(p->window, on_key);
    glfwSetMouseButtonCallback(p->window, on_mouse_button);
    glfwSetScrollCallback(p->window, on_scroll);
    /* where the cursor already is does not count as a movement */
    glfwGetCursorPos(p->window, &x, &y);
    mu_input_mouse_warp(&p->input, (float)x, (float)y);
    glfwGetFramebufferSize(p->window, &p->width, &p->height);
    return p;
}

void platform_destroy(Platform *p)
{
    glfwDestroyWindow(p->window);
    glfwTerminate();
    free(p);
}

bool platform_poll(Platform *p)
{
    double x, y;
    int    width = p->width, height = p->height;

    p->now = mu_clock_ns();
    glfwPollEvents();
    glfwGetFramebufferSize(p->window, &p->width, &p->height);
    /* A Wayland compositor can answer a frame with the size it now has for the
     * window (sway does, for a floating window). That answer arrives just after
     * the events of the next frame were read, so a client that goes straight on
     * draws one answer behind, and after one slow frame two sizes chase each
     * other for good. So when the size has changed: wait until the compositor
     * has seen everything sent so far, and take the size it leaves. The empty
     * event is the round trip: it comes back after everything before it. */
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND && (p->width != width || p->height != height))
    {
        glfwPostEmptyEvent();
        glfwWaitEvents();
        glfwGetFramebufferSize(p->window, &p->width, &p->height);
    }
    glfwGetCursorPos(p->window, &x, &y);
    mu_input_mouse_pos(&p->input, (float)x, (float)y);
    mu_input_frame_begin(&p->input, p->now);
    return !glfwWindowShouldClose(p->window);
}

MuInputState *platform_input(Platform *p)
{
    return &p->input;
}

void platform_size(Platform *p, uint32_t *width, uint32_t *height)
{
    *width  = (uint32_t)p->width;
    *height = (uint32_t)p->height;
}

VkSurfaceKHR platform_create_surface(Platform *p, VkInstance instance)
{
    VkSurfaceKHR surface;

    if (glfwCreateWindowSurface(instance, p->window, NULL, &surface) != VK_SUCCESS)
        platform_fatal("cannot create a Vulkan surface for the window");
    return surface;
}
