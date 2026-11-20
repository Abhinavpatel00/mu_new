# Builds ./toon (the demo in main.c) and ./smoke (tests/smoke.c, a headless
# end-to-end check of vk.c).
#
#   make              ./toon for X11
#   make WAYLAND=1    ./toon for Wayland only: no X11 code is compiled, and
#                     libdecor is never loaded
#   make run          ./toon in a window
#   make shot         ./toon --shot toon.png, no window needed
#   make check        builds ./smoke and runs it twice on whatever Vulkan device
#                     is there: once from nothing, once from the pipeline cache file
#
# WAYLAND=1 goes with every make command, or is exported once. Changing it
# rebuilds what depends on it, and so does editing this file or any header.
#
# Vulkan is loaded at run time through volk: there is no -lvulkan. GLFW is built
# from external/glfw and loads the window system's libraries itself at run
# time, so the only link-time dependencies are libm, libdl and libpthread.
# Only platform.c uses GLFW.
#
# The X11 build needs the X11, Xrandr, Xinerama, Xcursor, Xi and Xext headers.
# The Wayland build needs wayland-scanner and the wayland-client and xkbcommon
# headers instead, and no X11 header.
#
# Shaders are Slang. vk.c compiles them with `slangc` (or $SLANGC) the first
# time and whenever a .slang file changes; the .spv files next to them are
# what a machine without slangc runs.

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -std=c11 -Wall -Wextra -Werror -fno-strict-aliasing
INCLUDE := -Iexternal/vulkan/include -I.
LIBS    := -lm -ldl -lpthread
# every object lists the headers it read in a .d file, so editing one rebuilds it
DEPEND  := -MMD -MP

GLFW_SRC := context init input monitor platform vulkan window egl_context osmesa_context \
            null_init null_monitor null_window null_joystick \
            posix_module posix_time posix_thread posix_poll linux_joystick

# ---- the window system: two compile flags decide it --------------------------
#   GLFW_FLAGS      -D_GLFW_WAYLAND or -D_GLFW_X11: the one backend of GLFW that
#                   is compiled. (HAVE_MEMFD_CREATE is what GLFW's own CMake
#                   build defines on Linux.)
#   PLATFORM_FLAGS  -DPLATFORM_NO_LIBDECOR: platform.c tells GLFW not to load
#                   libdecor for the window frame. The compositor draws the
#                   frame where it offers to, GLFW a plain one where it does not.
ifeq ($(WAYLAND),1)
WINDOW_SYSTEM  := wayland
GLFW_SRC       += wl_init wl_monitor wl_window
GLFW_FLAGS     := -D_GLFW_WAYLAND -DHAVE_MEMFD_CREATE -Ibuild/wayland
PLATFORM_FLAGS := -DPLATFORM_NO_LIBDECOR
else
WINDOW_SYSTEM  := x11
GLFW_SRC       += x11_init x11_monitor x11_window xkb_unicode glx_context
GLFW_FLAGS     := -D_GLFW_X11
PLATFORM_FLAGS :=
endif
GLFW_OBJ := $(GLFW_SRC:%=build/glfw/%.o)

all: toon

toon: build/main.o build/platform.o build/vk.o build/mu.o $(GLFW_OBJ)
	$(CC) $^ -o $@ $(LIBS)

smoke: build/smoke.o build/vk.o build/mu.o
	$(CC) $^ -o $@ $(LIBS)

build/main.o: main.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/platform.o: platform.c Makefile build/window-system
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(PLATFORM_FLAGS) $(DEPEND) -c $< -o $@

build/vk.o: vk.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/smoke.o: tests/smoke.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/mu.o: external/mu_lib/mu.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(DEPEND) -c $< -o $@

# third-party code: built as its authors build it, without our warning set
build/glfw/%.o: external/glfw/src/%.c Makefile build/window-system | build/glfw
	$(CC) $(CFLAGS) -std=c99 -w $(GLFW_FLAGS) -D_DEFAULT_SOURCE -Iexternal/glfw/include $(DEPEND) -c $< -o $@

# GLFW's Wayland code includes headers that wayland-scanner writes from the
# protocol descriptions GLFW ships
PROTOCOLS := $(patsubst external/glfw/deps/wayland/%.xml,build/wayland/%-client-protocol,$(wildcard external/glfw/deps/wayland/*.xml))
ifeq ($(WAYLAND),1)
$(GLFW_OBJ): | $(PROTOCOLS:%=%.h) $(PROTOCOLS:%=%-code.h)
endif

build/wayland/%-client-protocol.h: external/glfw/deps/wayland/%.xml | build/wayland
	wayland-scanner client-header $< $@

build/wayland/%-client-protocol-code.h: external/glfw/deps/wayland/%.xml | build/wayland
	wayland-scanner private-code $< $@

# The objects that list this file are for one window system. It changes only
# when WAYLAND does, and they are compiled again then.
build/window-system: FORCE | build
	@echo $(WINDOW_SYSTEM) | cmp -s - $@ || echo $(WINDOW_SYSTEM) > $@

build build/glfw build/wayland:
	mkdir -p $@

run: toon
	./toon

shot: toon
	./toon --shot toon.png

# Twice, on a cache directory of its own: the first run compiles every pipeline
# and leaves the pipeline cache file, the second run starts from that file.
check: smoke
	rm -rf build/cache
	XDG_CACHE_HOME="$(CURDIR)/build/cache" ./smoke
	test -s build/cache/smoke/pipelines-*.bin
	XDG_CACHE_HOME="$(CURDIR)/build/cache" ./smoke

clean:
	rm -rf build toon smoke

FORCE:

.PHONY: all run shot check clean

-include $(wildcard build/*.d build/glfw/*.d)
