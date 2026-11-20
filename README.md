# mu — one repo, two renderers: laptop or mobile

This repository ships two Vulkan renderers side by side. Pick the one you need:

| Target | Directory | What it is | Build |
|--------|-----------|------------|-------|
| **laptop / desktop** | `desktop/` | A small Vulkan 1.3 layer (`vk.h`/`vk.c`) plus a toon renderer (`main.c`) with an X11/Wayland window via GLFW. | `make` → `./desktop/toon` |
| **mobile** | `mobile/` | `mg`, a mobile-first GPU API (`mg.h`/`mg.c`: collector + stable radix sort + Vulkan backend), the `vk.h` mobile port (`vk_mobile.h`/`pass_cache.c`), and an Android demo (`demo/`). | `make TARGET=mobile` → `mobile/mg_test` |

## Build what you need

### Laptop (desktop)

```sh
make               # ./desktop/toon, X11
make WAYLAND=1     # ./desktop/toon, Wayland only
make run           # open a window
make shot          # render to a file, no window or display
make check         # headless end-to-end test of vk.c
```

See `desktop/README.md` and `desktop/AGENTS.md` (the C style rules).

### Mobile

```sh
make TARGET=mobile        # mg_test: collector + sorter, no Vulkan, no device
make -C mobile pc_test    # pass-cache self-test (needs the vendored Vulkan headers)
```

The Android demo is in `mobile/demo/` and is built with `mobile/demo/build_apk.sh`
(or Gradle against `mobile/demo/CMakeLists.txt`):

```sh
make mobile-apk   # needs ANDROID_HOME and the NDK
```

See `mobile/README.md`, `mobile/MOBILE_API_DESIGN.md` and `mobile/demo/README.md`.

## Layout

```
desktop/   laptop renderer: vk.h, vk.c, main.c, platform.c, shaders/, tests/, external/
mobile/    mobile renderer: mg.h, mg.c, vk_mobile.h, pass_cache.c, demo/, *.md
mesa/      (not part of this repo — a separate upstream project, left out)
```

## Contributing

The C style rules in `desktop/AGENTS.md` apply to both targets unchanged:
longer functions, few `if` blocks, fatal-on-failure setup, and `MU_ASSERT` for
invariants rather than run-time checks.
