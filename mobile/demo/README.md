# mg-demo — a mobile Vulkan demo for `mg`, and an APK

```
mg-demo.apk        304 KB, signed, zipaligned, arm64-v8a + x86_64, minSdk 24
lib/libmobile.so   the whole app: mobile_main.c + game.c + ../mg.c, ~160 KB per ABI
                   (uncompressed and page-aligned in the zip -- see bug #3)
```

**What you see:** a small game — bricks, a ball and a paddle — over a dim backdrop of 1024
spinning textured quads (4× MSAA, resolved in-pass, with a vignette), and a **frame-time HUD**
across the top: the last 96 frame intervals as a graph, plus `16.7 ms  60 fps`, `cpu 7.2 ms` and
the score, in real bitmap text.

**Drag** anywhere to move the paddle, **tap** to launch the ball. Break every brick and the next
level is faster. Once a second, logcat gets a line like:

```
mg-demo:  59.9 fps | cpu 2.4 ms | draws 1063 | pipes 4 | mats 3 | pushes 1063 | sort 33 us | arena 35184 B | val 0e 0w
```

`pipes 4 / mats 3` against `draws 1063` is the whole point of the API: a thousand background
quads plus 39 game and HUD draws cost four pipeline binds and three material binds, because the
frame was sorted before it was recorded.

---

## What is actually being demonstrated

| Demo behaviour | Design rule it exercises |
|---|---|
| Draws are appended **transparent first, opaque last** and come out correct | the frame is sorted before it is recorded (`mg_draw.c`) |
| `pipes 1 / mats 1 / pushes N` in the log | state changes are coalesced; a draw costs 8 bytes of push |
| Two passes declared once at startup, never described again | a pass is a compile unit (`MOBILE_API_DESIGN.md` §4) |
| Scene pass: 4× MSAA colour and D24S8 that are **cleared and never stored**, resolved in-pass | tile memory is the only cheap memory; never `vkCmdResolveImage` |
| Composite samples the resolve from the previous pass | the pass carries the synchronisation; the app never emits a barrier |
| One vertex arena + one index arena, `vertexOffset` per draw, positions and UVs in **separate streams** | §5.5: keep fixed-function vertex fetch, never pull |
| Fullscreen triangle generated from `gl_VertexIndex`, zero attributes | the one place pulling is not needed at all |
| One submit, one fence, two frames in flight | Roblox's rule: one completion fence per frame |
| No Java anywhere; `android:hasCode="false"` | the platform layer is one C file |
| The game: 35 bricks, a paddle and a ball, each a **unit quad** placed by a 48-byte param slot | a 2D layer costs one draw per sprite and no vertex data |
| The HUD: graph + real text, one draw, 608 bytes of param slot, no texture, no atlas | per-frame data goes in the param ring, not into an upload |
| Sprites and HUD are `MG_DRAW_ORDERED` in the composite pass | painter's order is meaning: the sort may coalesce them, never reorder them |

---

## Files

| File | Lines | What it is |
|---|---|---|
| `mobile_main.c` | ~600 | the Android app: `android_main`, window lifecycle, input, frame pacing, the backdrop scene, the game hookup, logcat stats |
| `game.c` | ~360 | the game (bricks, ball, paddle, levels) and the frame-time HUD: it emits mg draws and owns no Vulkan |
| `game.h` | | the game's interface: `Game`, `GameRes` (five handles the app already created) |
| `../mg.h` | | the API (does not include Vulkan) |
| `../mg.c` | ~2200 | the single implementation: the collector and the stable radix sort (part 1, no Vulkan), then the Vulkan backend for the subset of `mg.h` the demo uses (part 2) |
| `shaders/*.vert/frag` | | scene + composite (3D), sprite + hud (2D), GLSL 450 |
| `shaders/gen_spirv.sh` | | GLSL → SPIR-V → `spirv.h` with the NDK's own `glslc` |
| `CMakeLists.txt` | | the same build for Gradle / Android Studio |
| `AndroidManifest.xml` | | `android.app.NativeActivity`, `lib_name=mobile` |
| `build_apk.sh` | | aapt2 → zip → zipalign → apksigner, no Gradle |

---

## Build the APK

One command, given a JDK and the Android SDK:

```sh
export ANDROID_HOME=$HOME/android-sdk
export PATH=$HOME/jdk-17/bin:$PATH          # only apksigner and keytool need Java
sh build_apk.sh                             # -> mg-demo.apk
```

`build_apk.sh` runs the same six steps the Gradle plugin runs, in order:

```sh
# 1. shaders: GLSL -> SPIR-V -> shaders/spirv.h   (NDK's shader-tools/glslc)
$ANDROID_HOME/ndk/*/shader-tools/linux-x86_64/glslc --target-env=vulkan1.1 -O -o x.spv scene.vert

# 2. the native library, per ABI
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
    -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared \
    -I. -I.. -I$NDK/sources/android/native_app_glue \
    -o lib/arm64-v8a/libmobile.so \
    mobile_main.c ../mg.c $NDK/sources/android/native_app_glue/android_native_app_glue.c \
    -landroid -llog -lvulkan -u ANativeActivity_onCreate

# 3. manifest -> binary XML + resources
$BUILD_TOOLS/aapt2 link -o base.apk -I $ANDROID_HOME/platforms/android-34/android.jar \
    --manifest AndroidManifest.xml --min-sdk-version 24 --target-sdk-version 34

# 4. put the .so files in the APK
zip -q -r base.apk lib

# 5. align to 4 bytes (required before signing, so the loader can mmap the .so)
$BUILD_TOOLS/zipalign -f -p 4 base.apk aligned.apk

# 6. sign (v1+v2+v3)
keytool -genkeypair -keystore debug.keystore -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US"
$BUILD_TOOLS/apksigner sign --ks debug.keystore --ks-pass pass:android \
    --key-pass pass:android --ks-key-alias androiddebugkey --out mg-demo.apk aligned.apk
```

### Getting the SDK, from nothing

```sh
export ANDROID_HOME=$HOME/android-sdk
mkdir -p $ANDROID_HOME/cmdline-tools
curl -O https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
unzip commandlinetools-linux-11076708_latest.zip -d $ANDROID_HOME/cmdline-tools
mv $ANDROID_HOME/cmdline-tools/cmdline-tools $ANDROID_HOME/cmdline-tools/latest
yes | $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --licenses
$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager \
    "platform-tools" "platforms;android-34" "build-tools;34.0.0" "ndk;26.3.11579264"
```

(That is exactly what produced this APK — SDK 34, build-tools 34.0.0, NDK r26d, on a machine
with no Gradle and no Android Studio.)

### The Gradle path, if you want Android Studio

Same sources, `CMakeLists.txt` already written:

```groovy
// app/build.gradle
android {
    namespace 'com.example.mgdemo'
    compileSdk 34
    defaultConfig {
        applicationId 'com.example.mgdemo'
        minSdk 24
        targetSdk 34
        ndk { abiFilters 'arm64-v8a' }
        externalNativeBuild { cmake { arguments '-DANDROID_STL=none' } }
    }
    externalNativeBuild { cmake { path '../CMakeLists.txt'; version '3.22.1' } }
}
```

then `./gradlew assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`.
Gradle is only a nicer `build_apk.sh`: it does the same aapt2/d8/zipalign/apksigner steps plus
dependency management and the wrapper, at the cost of ~400 MB and a first-run download.

---

## Install and run

```sh
adb install -r mg-demo.apk
adb shell am start -n com.example.mgdemo/android.app.NativeActivity
adb logcat -s mg mg-demo        # device line first, then once a second
```

First lines in logcat tell you what the device is and what the API branches on:

```
mg: device 'Adreno (TM) 740', vulkan 1.1.128
mg: caps: push=128 B, sampled=200, colour=8, indirectMax=1, astc=1, invocations=1024
mg: heap: 8.0 MB mapped, 3072.0 KB used
mg: swapchain: 1080x2400, 3 images, format 37
mg: pass 'scene': 3 attachments, 1080x2400, 4x MSAA, resolve=1, depth=1
mg: pass 'post': 1 attachments, 1080x2400, 1x MSAA, resolve=0, depth=0
mg: validation: VK_LAYER_KHRONOS_validation, spec 1.4.363, impl 1
mg: validation: errors and warnings now reach logcat as '[validation ERROR] ...'
mg: warm: 3 pipeline variants, 0 created after this point
```

`warm: 0 created after this point` is the rule "nothing may be created at draw time" holding.

---

## Bug #1 — the first thing a device ever told us

First run on hardware died at library load, before `vkCreateInstance`:

```
java.lang.UnsatisfiedLinkError: dlopen failed:
    cannot locate symbol "cosf" referenced by libmobile.so
```

**Root cause: the link line was missing `-lm`.** Not a Vulkan problem, not an NDK
problem, not a `minSdk` problem. `cosf`, `sinf`, `sqrtf` and `fmaxf` live in
`libm.so` on Android — they are *not* in `libc.so`, and unlike glibc there is no
implicit libm. The proof, from the shipped binary:

```
$ llvm-nm -D --undefined-only libmobile.so        # before
                 U cosf                            <-- unversioned: no provider recorded
                 U sinf
$ llvm-readelf -d libmobile.so | grep NEEDED       # before
  ... libandroid.so, liblog.so, libvulkan.so, libdl.so, libc.so   <-- no libm.so
```

`-shared` without `--no-undefined` happily emits an undefined symbol and *no*
`DT_NEEDED` entry for the library that would provide it, so the failure surfaces
on the device instead of at build time. After adding `-lm`:

```
                 U cosf@LIBC                       <-- versioned: the loader knows where it lives
   DT_NEEDED: libm.so libandroid.so liblog.so libvulkan.so libdl.so libc.so
```

Two fixes, both permanent:

1. **`-lm`** on the link line (`m` in `CMakeLists.txt`).
2. **`-Wl,--no-undefined`**, so this class of mistake is a *build* error. It is now
   in both build paths, and it is why this cannot silently recur.

And a third, because the linker only sees what it is given: `build_apk.sh` step 7
audits the finished `.so` — every undefined symbol must be provided by a library
in its `DT_NEEDED` list, checked against the NDK's own API-24 stubs. Today:
**158 (arm64) / 156 (x86_64) imports, all resolved**, on both ABIs.

(The one-line diagnosis: `llvm-readelf -d libmobile.so | grep NEEDED`. If `libm.so`
is missing and `llvm-nm -D --undefined-only` shows `cosf`, that is the whole bug.)

## Bug #2 and #3 — why it died silently on launch

The first run after the `-lm` fix loaded the library and then closed instantly with no visible
output. Two separate defects were found by inspection; either could do it.

### #2 — a null image view in three of four material slots

`mg_create()` makes a 1×1 white texture for empty material slots. It copied the wrong array entry:

```c
MgTex t = mg_texture(d, &td, &white, 4);   /* stored at texs[t.index - 1] */
d->dummy_white = d->texs[t.index];         /* BUG: one slot too far, all zeros */
```

So `dummy_white.view == VK_NULL_HANDLE`, and every material wrote three of its four
`COMBINED_IMAGE_SAMPLER` descriptors with a null image. Writing a null descriptor is only legal with
the `nullDescriptor` feature (Vulkan 1.1 optional, not requested), so the first draw was undefined
behaviour — which on a mobile driver means a GPU fault and an instant close. Fixed to
`texs[t.index - 1]`.

### #3 — the `.so` was compressed inside the APK

```
$ unzip -v mg-demo.apk | grep libmobile
  71560  Defl:N   29722  59%   lib/arm64-v8a/libmobile.so      <-- before
 114296  Stored  114296   0%   lib/arm64-v8a/libmobile.so      <-- after
```

With `android:extractNativeLibs="false"` (the default for `targetSdk >= 23` as packaged here), the
loader **mmaps the `.so` straight out of the APK**, which requires the zip entry to be uncompressed
and page-aligned. `zip -r` deflated it, and `zipalign -p` could not align an entry that was
compressed in the first place. Now: `zip -0` (store), `zipalign -f -p 4`, `extractNativeLibs="false"`
declared explicitly, and step 5 verifies it.

### #4 — one semaphore pair shared by two frames in flight

`sem_acquire`/`sem_present` were single objects while `MG_FRAMES_IN_FLIGHT == 2`. Both are now
per-slot arrays. Same class of bug as #2: works until it doesn't, then hangs or faults.

## Bug #5, #6, #7 — the black screen

The first run that survived startup (Mali-G57 MC2, Android 15) was **a fully black window that was
otherwise healthy**: `step 10/12: first frame`, then `60.0 fps | cpu 2.7 ms | draws 1025 | pipes 2 |
mats 2`. Every command buffer was recorded, every frame was submitted and presented, and nothing
reached the display. Three independent defects, each enough on its own.

The method, because "it runs but it is black" says nothing about which stage is lying: fill the
present pass with a **clear** instead of `DONT_CARE` and skip its draw (a green screen appears —
so surface, swapchain, render pass and present all work), then **copy the scene pass's resolve
target back to the host** and look at it (it was 100% clear colour — so the 1024 quads drew
nothing). Those two runs put the fault between `vkCmdDrawIndexed` and the rasteriser, not in the
platform layer.

### #5 — descriptor offsets were heap offsets, not buffer offsets

```c
/* before: d->ubo already starts at ubo_off inside heap_mem */
VkDescriptorBufferInfo ubo = { d->ubo, d->ubo_off + i * UBO_BYTES, UBO_BYTES };
```

One heap, many buffers: `heap_alloc()` binds each buffer at an offset inside `heap_mem`, so a
`VkDescriptorBufferInfo` offset is relative to **that buffer**, not to the heap. `ubo_off` was
~3.5 MB into a 512-byte buffer, so the descriptor was out of range on both bindings: the shader
read `viewproj` as all zeros, every vertex went to `w = 0`, and the whole scene collapsed to a
point. The composite then sampled a scene image that held only its clear colour, which `pow(x,
1/2.2)` turned into the black that was on screen. Fixed to `i * UBO_BYTES` / `i * PARAMS_BYTES`.

`vkCmdBindIndexBuffer`, `vkCmdBindVertexBuffers` and `vkCmdPushConstants` all take buffer-relative
offsets too — this is the one place the difference is silent, because an out-of-range descriptor is
not a validation error on a release driver, it is just a read of zeros.

### #6 — the pipe/state variant table was indexed with 0-based and 1-based keys

`mg_state()` returns a **handle** (`++n_state`, so 1, 2, 3...) and a draw record stores that handle,
but `mg_warm()` filled the variant table with `si = 0 .. n_state-1`:

```c
for (uint32_t si = 0; si < d->n_state; si++)      /* before */
    d->pipes[pi].pipe[si] = build_pipe(d, pi, si);   /* reads states[si]     */
...
vkCmdBindPipeline(..., d->pipes[r->pipe - 1].pipe[r->state]);   /* handle!  */
```

Two consequences. Every state ran with its **neighbour's** variant (opaque used the alpha pipeline,
alpha used the composite one). And the last state — the composite's, handle 3 — had `have[3] ==
false`, so `record_pass()` silently skipped the `vkCmdBindPipeline` and issued `vkCmdDrawIndexed`
with **no pipeline bound**: no fragments, no error, no validation message without layers. That is
why replacing the composite shader with a solid magenta still produced nothing on screen; the
fragment stage never ran. Warming `si = 1 .. n_state` and reading `states[si - 1]` fixes both
halves of the off-by-one.

### #7 — the depth image view named the colour aspect

`pass_image()` created every attachment view with `VK_IMAGE_ASPECT_COLOR_BIT`, including the
`D24S8` depth attachment. That is a spec violation (`VUID-VkImageViewCreateInfo-format`); Mali
accepted the view and then could not use it as a depth attachment. Views now derive the aspect
from the format (`DEPTH`, or `DEPTH|STENCIL` for combined formats).

### After the fix

The same device, same APK: **60 fps at 1024 draws** (a build that still had the tap toggle reached
`draws 3073 | pipes 2 | mats 2`, ~47 fps, 4x MSAA). In place of the diagnostic readback's "100% clear colour", the same screenshot
analysis now finds **103 622 distinct colours at 1024 draws** (the vignetted background, the texture,
and the translucent quads over it) — a rendered frame, not a filled one.

## Bug #8, #9 — what the validation layer found on its first run

With `VK_LAYER_KHRONOS_validation` finally loaded (see *Validation layers* below), a frame that
looked perfect reported two errors that no amount of pixel-inspection would have found:

### #8 — resetting command buffers from a pool without the reset flag

```
vkResetCommandBuffer(): VkCommandBuffer ... was created from VkCommandPool ... which was created
with VK_COMMAND_POOL_CREATE_TRANSIENT_BIT (missing VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT).
[ VUID-vkResetCommandBuffer-commandBuffer-00046 ]
```

`mg_frame()` resets the slot's command buffer every frame; the pool only asked for `TRANSIENT`.
Mali tolerates it, which is why the demo had been running at 60 fps — it is still undefined
behaviour, and it is exactly the kind of thing that suddenly fails on the next driver. The pool now
sets both flags.

### #9 — one present semaphore per frame slot instead of per swapchain image

```
vkQueueSubmit(): pSubmits[0].pSignalSemaphores[0] (VkSemaphore ...) is being signaled by VkQueue ...,
but it may still be in use by VkSwapchainKHR ...
[ VUID-vkQueueSubmit-pSignalSemaphores-00067 ]
```

The presentation engine holds the semaphore a frame signalled until **that image** is acquired
again, and with seven images coming back in an arbitrary order, two frames in flight are not enough
independent semaphores. This is a hang/corruption class bug, not a cosmetic one. `sem_present[]` is
now sized and indexed by the acquired image, the pattern Khronos recommends for swapchain semaphore
reuse.

After both: `val 0e 0w` on every frame at 1024 and 3073 draws.

### #10 — validation must never be the reason the app does not start

Asking for a layer that cannot be loaded turns `vkCreateInstance` into `VK_ERROR_LAYER_NOT_PRESENT`
— a black screen and no output, the exact failure mode this README exists to prevent. Enumerating
the layer is **not** proof that it loads (a stale `setprop debug.vulkan.layers`, a half-copied `.so`,
a missing dependency all enumerate fine and then fail). The instance is now created with one retry:
if it fails and we asked for a layer or `VK_EXT_debug_utils`, it retries with neither, logs why, and
carries on. Verified by pointing the loader at a layer binary it could not reach: the app starts, and
logcat says so in one line.

## Validation layers on the device

`mg.c` enables `VK_LAYER_KHRONOS_validation` whenever `MgDesc.validation` is non-zero
(`mobile_main.c` asks for `MG_VALIDATE_CORE` by default — flip it to `MG_VALIDATE_ALL` to reproduce
the runs below), installs a `VK_EXT_debug_utils` messenger, and turns
on the layer's own **sync** and **best-practices** modes through `VK_EXT_layer_settings` at
`vkCreateInstance` time — no device property, no `vk_layer_settings.txt`, no environment variable:

```c
VkLayerSettingEXT lset[] = {
    { "VK_LAYER_KHRONOS_validation", "validate_sync",           VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on },
    { "VK_LAYER_KHRONOS_validation", "validate_best_practices", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on },
};
```

Logcat then arrives under the demo's own tag, in order with the calls that caused it, and is counted
in the stats line:

```
mg      : validation: VK_LAYER_KHRONOS_validation, spec 1.4.363, impl 1
mg      : validation: modes core=1 sync=1 best_practices=1
mg      : [validation ERROR] vkResetCommandBuffer(): VkCommandBuffer ... (missing VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)
mg-demo :  42.2 fps | cpu 23.70 ms | draws 1025 | ... | val 0e 0w
```

`val 0e 0w` is the running error/warning count for the run; anything above zero is API misuse, not a
performance note. If the layer is not installed the app says so in one line and keeps running:

```
mg: validation: requested but VK_LAYER_KHRONOS_validation is not visible to the loader -- see demo/README.md
```

### Install it (one command, no root)

Android does not ship the layer. Take the official Android binary (27 MB per ABI), then:

```sh
curl -LO https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-1.4.363.1/android-binaries-1.4.363.1.zip
unzip -q android-binaries-1.4.363.1.zip
sh vvl_install.sh android-binaries-1.4.363.1/arm64-v8a/libVkLayer_khronos_validation.so
```

`vvl_install.sh` wraps exactly what this needs (from
`developer.android.com/ndk/guides/graphics/validation-layer`, "Use external validation layers"):

```sh
adb push libVkLayer_khronos_validation.so /data/local/tmp
adb shell run-as com.example.mgdemo cp /data/local/tmp/libVkLayer_khronos_validation.so .
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app       com.example.mgdemo
adb shell settings put global gpu_debug_layers    VK_LAYER_KHRONOS_validation
adb shell settings put global gpu_debug_layer_app com.example.mgdemo
```

The layer lives in the app's own data directory (no root, no `run-as` afterwards) and the four
settings point the loader at it; they persist across reboots. Check and undo with
`sh vvl_install.sh --status` and `sh vvl_install.sh --off`.

Things that will otherwise cost an hour:

- **The app must be debuggable**, or (targetSdk ≥ 30) carry
  `<meta-data android:name="com.android.graphics.injectLayers.enable" android:value="true"/>` — both
  are in this manifest. The loader prints `added global layer 'VK_LAYER_KHRONOS_validation' from
  library '/data/user/0/com.example.mgdemo/libVkLayer_khronos_validation.so'` when it works.
- **Reinstalling via `adb uninstall` wipes the app's data dir**, and with it the copied `.so`:
  re-run the script after a clean install.
- **Do not use the `/data/local/debug/vulkan` + `setprop debug.vulkan.layers` route on a retail
  build.** `mkdir /data/local/debug` is permission-denied, and a forced layer that cannot be loaded
  makes `vkCreateInstance` fail with `VK_ERROR_LAYER_NOT_PRESENT` for *every* Vulkan app on the
  device. (This is how bug #10 was found.)
- 64-bit ABI only: the demo APK ships arm64-v8a + x86_64, so install the matching `libVkLayer_*`.

### What it costs

On the Mali-G57, per frame at 1024 draws (tap → 3073 in brackets):

| `MgDesc.validation` | cpu | fps |
|---|---|---|
| `0` | 2.5 ms (11.9 ms) | 60 (60) |
| `MG_VALIDATE_CORE` | 8.2 ms (20.4 ms) | 60 (47) |
| `MG_VALIDATE_ALL` (core + sync + best practices) | 23.7 ms (38.4 ms) | 42 (26) |

With the game and the HUD in the frame (39 more draws, `draws 1062`), `MG_VALIDATE_CORE` costs
11.0 ms CPU and still holds **60.0 fps**, which is why that is the default in `mobile_main.c`.

Synchronisation validation is the expensive one and it is the one that finds the bugs core
validation cannot see. Use `MG_VALIDATE_CORE`, or `0` for measurement runs.

### Bug #11 — a render pass that carried only half its synchronisation

The moment sync validation came on, a frame that *looked* perfect reported this ten times:

```
vkQueueSubmit(): ... WRITE_AFTER_WRITE hazard detected. vkCmdBeginRenderPass ... performs image
layout transition ... on attachment 2 (oldLayout VK_IMAGE_LAYOUT_UNDEFINED, newLayout
VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL), which was previously written at the end of
subpass 0 (vkCmdEndRenderPass) by the attachment storeOp
```

The scene pass's external dependency had `srcAccessMask = 0`, i.e. it ordered nothing against the
previous frame. Both the MSAA colour and the depth image are written by frame N and written again by
frame N+1, and the resolve target is *sampled* by frame N's composite — three cross-frame hazards
that only a `srcAccessMask` of `COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE | SHADER_READ`
over `COLOR_ATTACHMENT_OUTPUT | (EARLY|LATE)_FRAGMENT_TESTS | FRAGMENT_SHADER` actually orders. This
is exactly the guarantee `mg` claims to provide ("the pass carries the synchronisation"), so the pass
now carries it. Mali tolerated the old code; the hazard was real.

### What best practices reported, and what changed

| Finding | Fix |
|---|---|
| *"multisampled image with 4 samples ... uses loadOp/storeOp which requires accessing data from memory. Multisampled images should always be loadOp = CLEAR or DONT_CARE, storeOp = DONT_CARE"* | The 4x colour attachment was created with `STORE` even though it is resolved in-pass: the demo was writing the 4x surface to memory every frame. It is now `DONT_CARE` whenever `resolve` is on, and drops its pointless `TRANSFER_SRC` usage. |
| *"Attachment 2 ... loadOp/storeOps never have to be backed by physical memory, but the image ... does not have VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT"* | Attachments that are never stored (the resolved MSAA colour, the depth with `MG_STORE_NONE`) are now created with `TRANSIENT_ATTACHMENT` and bound to **lazily allocated** memory, probed with `vkGetPhysicalDeviceImageFormatProperties` and falling back to device-local when unsupported. This is §7's "one heap per memory type" meeting tile memory. |
| *"render pass does not have loadOp == CLEAR but VkRenderPassBeginInfo.clearValueCount > 0"* | The composite pass passes no clear values now; `clearValueCount` reaches only the last attachment that is actually cleared. |
| *"smaller buffers/images should be sub-allocated from larger memory blocks (threshold 1 MB)"* ×4 | Texture uploads stage through the existing mapped heap instead of a fresh `VkDeviceMemory` + map/unmap per texture (one was 4 bytes), and small device-local images (the 128×128 texture, the 1×1 white) bump-allocate from a shared 2 MB block. |

After all of it: `val 0e 0w` — zero errors and zero warnings — with core, sync and best-practices
all on, at 1024 and 3073 draws.

### The alternative: bundle it in the APK

Google's preferred route is to put `libVkLayer_khronos_validation.so` in the APK under
`lib/<abi>/`, which needs no settings and works on any Android version. It is not used here because
it adds 27 MB per ABI to a 241 KB demo APK; `build_apk.sh` does not package it.

## Making the next crash loud instead of silent

A crash before the first frame is invisible: no window, no toast, nothing. The demo now reports
itself, so the next run will say exactly where it stopped.

**Every phase stamps a step number** to logcat:

```
mg-demo: libmobile.so loaded and initialised      <- dlopen + constructors ran
mg-demo: step 1/12: android_main entered
mg-demo: step 2/12: mg_create (instance, device, heap, descriptors)
mg-demo: device 'Adreno (TM) 740', vulkan 1.1.128
mg-demo: step 3/12: mg_create done
mg-demo: step 4/12: APP_CMD_INIT_WINDOW
mg-demo: step 5/12: creating surface + swapchain
mg-demo: pass 'scene': 3 attachments, 1080x2400, 4x MSAA, resolve=1, depth=1
mg-demo: step 6/12: building passes, pipes, materials, geometry
mg-demo: step 9/12: objects built and warmed
mg-demo: step 10/12: first frame
mg-demo:  59.9 fps | cpu 0.61 ms | draws 1024 | pipes 1 | mats 1 ...
```

**Every fatal signal prints the step and a backtrace**, then re-raises so the crash is still a
crash:

```
mg-demo: ==== FATAL SIGNAL 11 at step 10 (first frame) ====
mg-demo:   #00 0x7d1a44c3e8 libmobile.so+0x23e8
mg-demo:   #01 0x7d1a44c410 libmobile.so+0x2410
mg-demo:   ...
mg-demo: resymbolicate: adb logcat | ndk-stack -sym libmobile.so
```

Roman letters into `file:line` with the unstripped library, kept next to the APK:

```sh
adb logcat -d > log.txt
ndk-stack -sym libmobile-arm64-v8a.so -dump log.txt
```

And every `VKOK` failure now names both the call and the result:

```
mg: FATAL: vkCreateSwapchainKHR(...) -> -9 (VK_ERROR_OUT_OF_DATE_KHR)
```

### What to send me if it still dies

```sh
adb logcat -c                                     # clear
# tap the app, wait 3 seconds
adb logcat -d -s mg mg-demo DEBUG libc > log.txt  # then send log.txt
```

The **last** `step N/12` line plus any `FATAL`/`FATAL SIGNAL` line is the whole diagnosis. If
`libmobile.so loaded` is missing, the problem is packaging, not code. If it stops at step 2 the
device refuses something in device creation; at step 5, the surface/swapchain; at step 10, the first
frame (render state).

## The game, and the frame-time HUD

The game is deliberately ordinary — 35 bricks, a ball, a paddle, levels, a score — because it is
here to show what a real 2D frame looks like in `mg`, not to be a good game:

| | cost |
|---|---|
| a sprite (brick, paddle, ball) | one draw, a 48-byte param slot, **no vertex data**: all four corners are the same unit quad |
| the whole game layer | 37 ordered draws, 1 pipe, 1 material, 1 state |
| the HUD | 1 draw, 608 bytes of param slot, no texture, no atlas, no upload |
| the frame | `draws 1063 | pipes 4 | mats 3 | pushes 1063` |

The HUD is the interesting half. It shows a 96-sample graph of real frame intervals (green under
20 ms, amber under 34, red past that, with the 16.7 ms line drawn), plus two lines of live text
written straight into the param ring as ASCII:

```
16.7 ms  60 fps
cpu 7.2 ms  score 1230
```

The font is MicroProfile's 5x8 bitmap font, embedded as bits — a 1024x9 sheet packed into 288
`uint`s, one glyph per 5 columns, the ninth row the drop shadow. The lookup is microprofile's own
compute shader moved into a fragment shader: the same `bitfieldExtract` of the same tables, so no
texture, no atlas and no upload is involved. Both text lines, the graph and the panel are one
quad: 4 vertices, 6 indices.

Two shader bugs worth remembering, both found by reading pixels out of screenshots because the
validation layer cannot see either one:

1. **The text was blank because the param block was indexed from `p[0]`, not from
   `p[pc.const_off >> 4]`.** The graph worked — it multiplied by the same base — so the panel and
the sprites looked perfect while both text lines drew nothing. `text_px()` now takes the base
   explicitly, and both readers go through one `param_f(base, i)` accessor, which unpacks a
   `vec4` with a select instead of a dynamic component index.
2. **The font lookup read past the sheet.** The cell is 10 rows tall (8 glyph rows + shadow +
margin) but the sheet is 9 rows, so `gy == 9` indexed `g_font_bits[288..]` — undefined, and on
   this driver it produced garbage strokes. MicroProfile's compute shader cannot hit this because
   its workgroup is exactly 5x9; the shader now rejects `gy >= 9` explicitly.

Everything is laid out in units of `panel_height / 4`, so the overlay scales with the display
instead of needing a second layout for tablets.

---

## What is verified, and what is not

Verified here:

- compiles for **arm64-v8a and x86_64** with `-std=c11 -Wall -Wextra -Werror` against NDK r26d,
  no warnings, and links with `-Wl,--no-undefined`;
- **every import resolves**: 158/156 undefined symbols across both ABIs, all provided by a library
  in `DT_NEEDED` (build step 7), so `dlopen` cannot fail on a missing symbol again;
- `clang --analyze` on `mg.c` + `mobile_main.c`: no findings;
- the eight shaders pass `spirv-val --target-env vulkan1.1`;
- the APK is a valid signed archive: `apksigner verify` ok, `zipalign -c -p 4` ok, `.so` entries
  `Stored` and page-aligned, `extractNativeLibs="false"`, `aapt2 dump badging` reports a launchable
  `android.app.NativeActivity`, and `ANativeActivity_onCreate` is exported (`T`);
- the collector's sort/coalescing numbers are from a real run (`cc -DMG_TEST ../mg.c`).

**Run on device**: Mali-G57 MC2, Android 15, 720x1548, Vulkan 1.3.219, 7 images, `R8G8B8A8_UNORM`,
`preRotate=0`, 4x MSAA resolve. The game renders and plays (tap launches, drag moves the paddle,
bricks break, the draw count drops 1063 -> 1061 as they do), the HUD's two text lines render in
the embedded font, and `val 0e 0w` holds with core + sync + best-practices validation on.
Verified by screenshot analysis (panel, brick rows, paddle position after a swipe) and by logcat.

Still not verified, in order of likelihood:

1. **Other vendors** — every run so far is one Mali device. Adreno's tile path, `preRotate=1`
   devices, and the `VK_EXT_load_store_op_none` fallback are untested.
2. **Validation coverage** — core, synchronisation and best-practices validation all run clean
   (`val 0e 0w`) on the paths the demo exercises. GPU-assisted validation and `debugPrintfEXT` are
   not enabled.
3. **Surface churn** — backgrounding the app recreates the surface and the passes; a resume was
   verified visually, but `APP_CMD_TERM_WINDOW` mid-frame and multi-window resize are not
   exercised deliberately.
4. **`vertexOffset` sign** — the demo walks forward from 0, so `int32_t` truncation cannot trigger
   at 3072 quads.

Run with validation if anything looks wrong:

```c
.validation = MG_VALIDATE_ALL,   // mobile_main.c: add sync + best practices to the default CORE
```
and read `adb logcat -s mg`.

---

## Deliberate deviations from `MOBILE_API_DESIGN.md`

| Design says | Demo does | Why |
|---|---|---|
| Attachments use transient / lazily allocated memory | same, when the device allows it | `TRANSIENT_ATTACHMENT` is a usage flag, so it is claimed whenever nothing reads the image back; **lazily allocated** memory needs `lazyAllocation` from `vkGetPhysicalDeviceImageFormatProperties`, and falls back to plain device-local when the format does not report it. Both are probed once at pass creation, not per frame |
| Immutable samplers baked into the material layout | sampler written into the combined image sampler descriptor | the demo has one sampler; baking would need one layout per sampler combination |
| `mg_pass_begin(f, pass)` describes nothing per frame | same | but note `mg_frame_const()` and `mg_pass_output()` were **added to `mg.h` while writing this**: the demo proved the API was missing a way to write per-frame constants and a way to read a previous pass's output |
| Coarse GPU-driven batches behind `MG_GPU_DRIVEN` | not implemented | §10 of the design doc says it is an escape, and Qualcomm's numbers say it loses |

---

## Next, in order

1. ~~Run it on a device; fix whatever logcat says.~~ Done: bugs #5-#7, and the demo now renders.
2. `AChoreographer` instead of the clock-based pacer (Android 24+; the port plan's stage 4).
3. `MG_MEM_FRAME` defragmentation (transient attachments are done).
4. Push the draw count until the CPU is visibly the limit, and log where the time goes — that is
   the measurement that decides whether stage 5 (coarse indirect) ever happens.

---

## What this build demonstrates beyond the first APK

- **Ordered draws.** The translucent quads are submitted back to front with
  `MG_DRAW_ORDERED`; the opaque ones are submitted backwards and state-sorted. Both contracts
  are visible in one pass, and `mg_coll_key()` decides which is which.
- **Store-op semantics.** `mg_states` uses `MG_STORE_READONLY` where the attachment is only
  read, which becomes `STORE_OP_NONE` when `VK_EXT_load_store_op_none` (or Vulkan 1.3) is
  present, and `DONT_CARE` when it is not. The log says which you got.
- **Pre-rotation.** The swapchain is created with `preTransform = IDENTITY` where the surface
  allows it and the rotation moves into `viewproj`, so the display processor never rotates the
  frame. Expect `preRotate=1, rotation=0 deg` on a portrait phone; a 90/270 on a device whose
  native panel orientation differs from the window.
- **16 KB pages.** The `.so` is linked with `-z max-page-size=16384`; `llvm-readelf -l` shows
  every `LOAD` at `0x4000`.

New first line in logcat to check:

```
adb logcat -d -s mg | head -3
I mg  caps: store_op_none=1, preRotate=1, rotation=0 deg
```
