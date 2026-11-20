# mobile-vulkan

Research and design for rendering on mobile, in the `mu_new` repository's own style. Two tracks:
**porting `vk.h`** (`MOBILE_VK_RESEARCH.md`, `vk_mobile.h`, `pass_cache.c`), and **a new API designed
from the mobile hardware up, deliberately different from `vk.h`** (`MOBILE_TALKS.md`,
`MOBILE_API_DESIGN.md`, `mg.h`, `mg.c`).

| File | What it is |
|---|---|
| **`MOBILE_VK_RESEARCH.md`** | The main document. Hardware model, a bandwidth ledger of the current frame, what Mesa's tiler drivers do with the API, ten doctrine rules, the concrete deltas to `vk.h`, the backend restructure, the Android platform layer, the measurement protocol, 25 failure modes, and a six-stage port plan. |
| **`vk_mobile.h`** | The mobile delta to `vk.h`, as code: feature table, capability clamping, subpasses and input attachments, NONE load/store, the 128-byte push ABI, and the value mappings as tables. Compiles against the tree. |
| **`pass_cache.c`** | The pass compiler: `PassDesc` → hash → cached `VkRenderPass` + `VkFramebuffer`. Working, with a self-test and mock loader entry points. |
| **`MOBILE_PORT_CHECKLIST.md`** | Per-frame and per-pass gates, review questions, and the trap list. |
| **`MOBILE_TALKS.md`** | The mobile rendering talks, with the numbers: Kapoulkine/Roblox (840 draws, GLES 38 ms → Vulkan 13 ms, 2 descriptor sets), Aaltonen/HypeHype (10 000 draws at 60 fps on a $99 phone, metadata-only command stream), Qualcomm SIGGRAPH 2026 (indexed 121 FPS / 2.02 W **vs** GPU-indirect 84 FPS / 3.84 W), Arm (MDI `drawCount` = 1 pre-G710). Ends in five facts → five API rules. |
| **`MOBILE_API_DESIGN.md`** | **The new API.** `mg` is designed from the hardware up and is deliberately *not* `vk.h`: no bindless (a material is 4 textures), no MDI (indirect is one draw per call), no immediate-mode recording (a draw is a 24-byte append into a sorted flat array), passes and states declared at startup and pre-warmed, vertex fetch left in fixed-function hardware (§5.5). Includes the `vk.h` vs `mg.h` table, the record and sort key, the cost model, what it refuses and why, and a 6-stage build order. Starts from `MOBILE_TALKS.md`, not from `vk.h`. |
| **`mg.h`** | The new API as code. **Does not include Vulkan** — no `Vk*` type reaches the application. 8-byte handles with generations, fixed caps, closed pass/material/state sets, the 8-byte push ABI, `mg_const()` slot ring, `MgCaps`. Compiles standalone (and its `_Static_assert`s pin the push to 8 B and the draw record to 24 B). |
| **`mg.c`** | **The single implementation.** Two parts in one translation unit: the collector (flat draw array, stable 4 × 8-bit radix sort, coalescing stats, and a `-DMG_TEST` self-test that needs no Vulkan), and the Vulkan backend (device, one mapped heap, pass table, material sets, 8-byte push, transient attachments, one submit/present per frame). The app includes `mg.h` and links `mg.c`; nothing else knows a `Vk*` type. |
| **`demo/`** | **A working Android demo, a small game, and an APK.** `mobile_main.c` (platform layer, window lifecycle, input, backdrop scene), `game.c`/`game.h` (a playable breakout and the frame-time HUD — bricks, paddle and ball are one draw each and **one shared unit quad**; the HUD is one more draw: a 96-frame graph plus live text in an embedded 5x8 bitmap font, no texture and no upload), `../mg.c` (the whole API — collector and backend), `shaders/`, `AndroidManifest.xml`, `CMakeLists.txt`, `build_apk.sh` — aapt2 → zip → zipalign → apksigner, no Gradle — and `vvl_install.sh`, which installs the Khronos validation layer on a device without root. `demo/mg-demo.apk` is built, signed, arm64+x86_64, **and runs at 60 fps on a Mali-G57** with `val 0e 0w`. `demo/README.md` documents the eleven bugs the device found, in order: `-lm` and packaging, then the black screen (descriptor offsets, state-variant indexing, depth view aspect), then the API-misuse bugs the validation layer reported (`vkResetCommandBuffer` on a pool without the reset flag; present semaphores per frame slot instead of per swapchain image; a render pass whose external dependency ordered nothing against the previous frame, which synchronization validation caught), plus what best-practices mode reported (a 4x MSAA attachment stored to memory every frame, non-transient never-stored attachments, a stray clear value, small unsuballocated resources). |

## Build

The port artifacts compile against the repository's own vendored headers, from this directory:

```sh
gcc -std=c11 -Wall -Wextra -O2 -c pass_cache.c \
    -I ../desktop -I ../desktop/external/vulkan/include

gcc -std=c11 -Wall -Wextra -O2 -DMU_PASS_CACHE_TEST -o /tmp/pc_test pass_cache.c \
    -I ../desktop -I ../desktop/external/vulkan/include && /tmp/pc_test
```

The self-test's output is the mobile-shaped scene pass the design calls for — three 4× MSAA colour
targets and a packed depth attachment that are cleared and **never stored**, with only their
single-sample resolves written back:

```
built a pass: 7 attachments, 1 subpass(es), 2 dependencies
  att 0: fmt 97  samples 4 load 1 store 1     <- R16G16B16A16_SFLOAT, CLEAR, DONT_CARE
  att 1: fmt 97  samples 1 load 2 store 0     <- the resolve: DONT_CARE, STORE
  att 2: fmt 37  samples 4 load 1 store 1
  att 3: fmt 37  samples 1 load 2 store 0
  att 4: fmt 37  samples 4 load 1 store 1
  att 5: fmt 37  samples 1 load 2 store 0
  att 6: fmt 129 samples 4 load 1 store 1     <- D24_UNORM_S8_UINT, CLEAR, DONT_CARE
```

The collector is standalone — no Vulkan, no repository headers:

```sh
gcc -std=c11 -O2 -Wall -Wextra -Werror -DMG_TEST -o /tmp/mg_test mg.c && /tmp/mg_test
```

```
10000 draws, 8 pipes, 64 materials, 4 state buckets

             pipe binds material binds   push changes   records
unsorted           8739           9861          10000     10000
sorted                8            512          10000     10000

radix sort (4 x 8 bit, stable, 24 B records)
  first call (cold pages):    217.6 us
  best of 20:                 151.8 us  (15.2 ns/draw)

command stream: 10000 draws x 12 B = 117.2 KB (12.0 B/draw, 50.0% of the records themselves)

key monotone: ok   stable within a bucket: ok   records intact: ok
```

Verified clean under `-Wall -Wextra -Werror`, and under `-fsanitize=address,undefined`.

## The three edits it asks of `vk.h`

1. `MAX_PUSH_CONSTANTS` → 128. Mobile `maxPushConstantsSize` is 128; 256 fails on the device.
2. `ImageType` gains `IMAGE_INPUT`, and `PassDesc` gains a subpass list. This is the one real API
   addition: it is worth ~3.7 GB/s and ~0.4 W at 2400×1080/60 for the toon frame.
3. `Load` gains `NONE`, `Store` gains `NONE`, and `ImageDesc.transient` is wired to hardware
   (transient usage + lazily allocated memory + forced `DONT_CARE`).

Everything else survives: `PassDesc`, `DrawState`, the arenas, the timeline retirement, the
pipeline cache file, the bindless ABI (clamped), and vertex pulling (with a split vertex format).

## The new API, in one paragraph

`vk.h` is a good desktop API: it assumes bindless, immediate-mode recording and, for draw-call scale,
GPU-driven submission. On the hardware in the field each of those is a loss. Bindless is not in the
Android Baseline 2021/2022 profile and costs a dependent descriptor load (40.5 vs 12.1 text-read
cycles, Qualcomm's own compiler numbers). `maxDrawIndirectCount` is 1 on every Mali before G710. And
the fully GPU-driven path is **1.44× slower at 1.9× the power** than CPU culling with indexed draws
in the one vendor study that measured both, with a thermal cliff at four minutes. So `mg` inverts the
desktop trend: a material is four statically bound textures, a pass is declared once at startup, a
draw is a 24-byte append into a flat array that is sorted before it is recorded, per-draw constants
travel as an 8-byte push of an *offset into the arena*, and 10 000 records collapse into 8 pipeline
binds. The frame is a data structure, not a command stream — which is also what makes 10 000 draws at
60 fps on a $99 phone possible in the first place.

## The `vk.h` port, in one paragraph

A tiler executes a render pass, not a command sequence: it bins the geometry, then runs the tiles
with the attachments in on-chip memory, and it only pays DRAM for what a `loadOp`/`storeOp` says it
must. So the abstraction's job on mobile is not to wrap fewer calls — it is to make the frame have
fewer pass boundaries and to tell the driver the truth about every attachment's lifetime. `vk.h`
already has the right unit (`PassDesc`); what it lacks is subpasses with input attachments, a
transient attachment path, a 128-byte push ABI, a 1.1 render-pass fallback next to dynamic
rendering, and a pass cache so the same pass comes back as the same object and the driver's
autotuner can converge.
# mu_new
