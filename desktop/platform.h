#ifndef MU_PLATFORM_H
#define MU_PLATFORM_H

// platform.h — one window, and what the user does to it.
//
// The window's keyboard, mouse and frame time arrive as a MuInputState
// (external/mu_lib/mu/mu_input.h), and its Vulkan surface goes to
// gpu_set_surface(). platform.c implements this with GLFW and is the only
// file that includes GLFW.
//
//     platform = platform_create(&(PlatformDesc){ .title = "demo", .width = 1280, .height = 720 });
//     input    = platform_input(platform);
//     gpu_set_surface(g, platform_create_surface(platform, gpu_raw_instance(g)));
//
//     while (platform_poll(platform))
//     {
//         if (mu_input_key_pressed(input, MU_KEY_ESCAPE))
//             break;
//         angle += (float)mu_input_mouse_down(input, MU_MOUSE_LEFT) * input->mouse_dx * 0.01f;
//         platform_size(platform, &width, &height);
//         gpu_resize(g, width, height);   // a Wayland window is as large as its frames
//         ...
//     }
//
//     gpu_destroy(g);              // the surface is the Gpu's, and goes before its window
//     platform_destroy(platform);

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

typedef struct MuInputState MuInputState;
typedef struct Platform     Platform;

typedef struct PlatformDesc {
    const char *title;
    uint32_t    width, height;

    // No display is used or needed: nothing appears, nothing is ever pressed,
    // and the window is never closed. The loop that draws to a window can then
    // draw to a file instead.
    bool headless;
} PlatformDesc;

// One per process. Opening the window is setup: if it fails, the reason is
// printed and the process exits.
Platform *platform_create(const PlatformDesc *desc);
void platform_destroy(Platform *p);

// The frame boundary. Everything the user did since the last call goes into
// the input state, in the order it happened, and the state's frame begins.
// False once the user has asked the window to close.
bool platform_poll(Platform *p);

// What the mu_input queries read: keys by MuKey, mouse buttons by
// MuMouseButton, the cursor in window coordinates, the wheel, and dt.
// It changes only inside platform_poll().
MuInputState *platform_input(Platform *p);

// The window's size in pixels: the size of a frame drawn to it. It follows the
// user's resizing and the display's scale, and it is what gpu_resize() takes.
// It changes only inside platform_poll().
void platform_size(Platform *p, uint32_t *width, uint32_t *height);

// A new surface for the window. gpu_set_surface() takes ownership of it.
// A headless platform has one only where the driver offers
// VK_EXT_headless_surface; rendering to a file needs none.
VkSurfaceKHR platform_create_surface(Platform *p, VkInstance instance);

#endif // MU_PLATFORM_H
