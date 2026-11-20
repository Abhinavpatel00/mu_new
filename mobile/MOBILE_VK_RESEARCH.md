# Mobile Vulkan — what changes in `vk.h` / `vk.c`, and why

A design and evidence document for taking the `mu_new` Vulkan layer (`vk.h`, `vk.c`, 3,576 line
backend) to Android. Written against the code in this repository at `a79cc93`, in its own style
rules (`AGENTS.md`, `external/mu_lib/AGENTS.md`), and against Mesa's own source as the model of what
a driver does with the API you hand it.

Everything here is either (a) a spec/`VkPhysicalDeviceLimits` fact, (b) a vendor-documented hardware
behaviour, or (c) a line of Mesa source. Where a number is per-device it says *query it* and gives
the query.

---

## 0. The short version

Twelve decisions, each defended later in this document.

| # | Decision | Why |
|---|---|---|
| 1 | **The render pass is the object the whole design turns on.** `PassDesc` stays the unit of work and becomes the *compile unit* for a cached `VkRenderPass` + `VkFramebuffer`. | A tiler only sees a pass as a pass. Mesa's turnip autotunes GMEM-vs-sysmem, tile size and render mode **keyed by render pass**, and only converges if the same pass recurs (`tu_autotune.h:21`, `tu_autotune.cc:822`). |
| 2 | **Add subpasses with input attachments.** `use_input()` + `subpassLoad`, `MAX_SUBPASSES = 4`, `VK_DEPENDENCY_BY_REGION_BIT` always. | Reading the scene colour from tile memory instead of DRAM is worth 3.7 GB/s and ~0.4 W at 2400×1080/60 on the toon frame (§3). This is the single largest win available and the current API cannot express it. |
| 3 | **Push constants drop from 256 to 128 bytes.** Everything else moves to a per-frame UBO arena indexed by `firstInstance` / draw offset. | `maxPushConstantsSize` is 128 on essentially every Android GPU. Today's `MAX_PUSH_CONSTANTS 256` is a hard `vkCreatePipelineLayout` failure on phones, not a slow path. |
| 4 | **Keep dynamic rendering, add the 1.1 path.** Same `PassDesc` must also compile to `vkCreateRenderPass2` + `vkCmdBeginRenderPass`. | Android Baseline 2021/2022 profiles do **not** include `VK_KHR_dynamic_rendering`; the installed fleet is Vulkan 1.1. New chipsets are Vulkan 1.3 (Android 15 CDD). Both paths must exist, and only one of them is the default. |
| 5 | **No barriers inside a frame.** Attachment dependencies come from the pass; only buffer/`ARENA_DEVICE` dependencies may produce a barrier, and those are merged into the pass's external dependency. | A `vkCmdPipelineBarrier2` between two passes that touch the same attachment = store-tiles-then-reload-tiles. Today `begin_pass()` emits one barrier **per pass**. On a tiler that is the design. |
| 6 | **Compute may not sit in the middle of a frame's render passes** unless it is the only consumer, and then it goes at the frame boundary. | A compute dispatch splits the pass: GMEM contents flush and reload (Arm: minimising render passes; Qualcomm: resolves are not free). `VK_QCOM_tile_shading` and `VK_KHR_dynamic_rendering_local_read` exist precisely to work around this; neither is guaranteed. |
| 7 | **Transient attachments and lazy memory are first-class, not an optimisation flag.** `ImageDesc.transient` must select `TRANSIENT_ATTACHMENT` usage + `LAZILY_ALLOCATED` memory, and force `storeOp = DONT_CARE`. | Depth, MSAA colour and the MSAA resolves never need a byte of DRAM. `tu_image.cc:1199` only offers the lazy memory type to images carrying that usage bit — the API is the only place the decision can be made. |
| 8 | **UMA memory model: one mapped heap per memory type, few large `VkDeviceMemory`, suballocated by the existing arenas.** Delete the staging copy path for vertex/texture uploads. | All device memory on Android is host-visible; mobile `maxMemoryAllocationCount` is small (4096 typical) and Mali/Adreno both prefer few large allocations. `buffer_host()` vs staging is a distinction with no hardware behind it. |
| 9 | **Bindless has three tiers and the shader ABI must survive all three.** | `VK_EXT_descriptor_indexing` is not in Android Baseline 2021/2022; `maxPerStageDescriptorSampledImages` is 200 by profile (`VP_KHR_roadmap_2022`). A 65,536-entry array is a request, not a capability. |
| 10 | **Vertex pulling stays, but the vertex format changes and the alternative exists.** | Pulling is fine on a tiler; a 32-byte-stride fetch to read 12 bytes is not. The binning pass re-reads positions separately — Arm's advice is to split them. |
| 11 | **Pipelines: assume `blend`, `write_mask` and `samples` are static.** EDS3 is *not* in Roadmap 2022, only in Roadmap 2024. | On the fleet, whole-state-dynamic (RADV-style, what `pipeline-explosion.md` assumed) is unavailable, so variant count is real. `MU_NO_EDS3=1` already simulates this on desktop — use it as the mobile proxy from day one. |
| 12 | **Frame pacing and present are part of the abstraction, not the platform layer.** | Pre-rotation, `preTransform`, 3-image swapchains, `AChoreographer`, `VK_GOOGLE_display_timing`, ADPF. Presenting wrong costs 1–3 ms of GPU time in the compositor. |

---

## 1. Audit: what survives, what breaks

Line numbers are from this tree. `Survives` = the abstraction is right and only the backend needs a
second path. `Breaks` = the API itself is wrong for the platform.

| API surface (`vk.h`) | Mobile verdict | Evidence / reason |
|---|---|---|
| `PassDesc` as a fixed-size value object: attachments, load/store, clear, resolve, `ImageUse[]`, `BufferUse[]` | **Survives, and is the right shape.** It is already almost exactly a `VkRenderPassCreateInfo`. | `tu_pass.cc:935 attachment_set_ops()` collapses `loadOp`/`storeOp` into `att->load`, `att->store`, `att->clear_mask`; `tu_pass.cc:906 tu_render_pass_bandwidth_config()` turns those into gmem-vs-sysmem bytes. The information a tiler needs **is** the `PassDesc`. |
| Declared dependencies (`use_sampled`, `use_storage`, `use_read`) → one `vkCmdPipelineBarrier2` per pass | **Breaks.** Correct on desktop, wrong on a tiler. | Between passes touching the same attachment, a barrier forces a GMEM store and a reload. Must become `VkSubpassDependency` (external) + `VK_DEPENDENCY_BY_REGION_BIT`, batched per frame. |
| `MAX_PUSH_CONSTANTS 256` + `PUSH(name, BODY)` + `_Static_assert(sizeof == 256)` | **Breaks.** `vkCreatePipelineLayout` fails on phones. | `maxPushConstantsSize` = 128 on Adreno and Mali. The spec minimum is 128. |
| Bindless: `MAX_BINDLESS_TEXTURES 65536`, `MAX_BINDLESS_SAMPLERS 256`, `MAX_BINDLESS_STORAGE 16384`, `image_sampled_id()` | **Breaks at the top end, survives in shape.** Needs a tier clamp from `maxPerStageDescriptorUpdateAfterBindSampledImages` and a descriptor-set fallback. | `VP_ANDROID_baseline_2022` has no descriptor indexing; `VP_KHR_roadmap_2022` guarantees it (Vulkan 1.3 devices). `maxPerStageDescriptorSampledImages` floor is 200. |
| `DrawState` 8 bytes + `draw_sort_key()` + `run_open` shadowing | **Survives, and becomes more valuable.** On mobile more of it is static, so the sort key saves context-rolls. | EDS3 absent ⇒ `blend`, `write_mask`, `samples` are baked. Sorting by (pipe, state) is then mandatory, not an optimisation. |
| `draw_indexed(pass, push, n, instances)` with vertex pulling from binding 3 | **Survives with a format change.** | Arm: separate position from the other attributes so the binning pass reads less. A 32-byte vertex is 2.7× the traffic of a 12-byte position for the pass that only needs positions. |
| `attach_depth_read()` (test + sample, never write) | **Survives** — it is the correct mobile idiom, and on a tiler the same-pass depth *sample* needs a fallback: `DEPTH_STENCIL_READ_ONLY` layout + `VK_EXT_attachment_feedback_loop_layout` where available, else a second pass. | Arm best-practice: "use `DEPTH_STENCIL_READ_ONLY` after the geometry pass". |
| `ImageDesc.transient` | **Survives and must be wired to hardware.** | Must imply `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT`, `LAZILY_ALLOCATED` preferred flags, and `storeOp` forced to `DONT_CARE`. |
| `Arena` = `ARENA_DEVICE` / `ARENA_TRANSIENT` / `ARENA_STAGING` | **Survives.** On Android `ARENA_STAGING` should alias `ARENA_DEVICE` on UMA. | Everything is host-visible; a staging ring on UMA is a memcpy for nothing. |
| Timeline semaphore retirement (`Gpu.submitted/completed`) | **Survives** on 1.2+; needs a fence fallback for 1.1. | `VK_KHR_timeline_semaphore` is core 1.2, extension on 1.1 — supported broadly on mobile but not guaranteed on the oldest tier. |
| `frame_image()`, `frame_begin/end`, `gpu_resize` | **Survives, gains pre-rotation and an explicit "surface gone" state.** | Android window recreation, `currentTransform`, `VK_ERROR_OUT_OF_DATE_KHR` on rotate. |
| `PipelineFile` cache (`$XDG_CACHE_HOME/<name>/pipelines-<vendor>-<device>.bin`) | **Survives and becomes load-bearing.** Path must become `Context.getCodeCacheDir()`. | Pipeline compile is a top-3 stutter source on Android. `VK_EXT_pipeline_creation_cache_control` (1.3 core) lets you refuse a compile at draw time and warm it off-thread. |
| `MU_ESCAPE` (`pass_raw_cmd`) | **Survives.** Needed for `VK_QCOM_render_pass_transform` and tile-shading experiments. | — |
| Size: 1 file, 13 sections, ~3.5k lines | **Survives.** Android does not want a framework here. | — |

---

## 2. The machine

### 2.1 Two passes over the geometry

Every mobile GPU in the field (Adreno, Mali, PowerVR, Xclipse/RDNA-mobile in its tiled mode) is a
tile-based renderer. A render pass is executed as:

```
draw commands ──► tiler: transform positions, cull, bin into tiles
                          │  visibility/parameter stream to DRAM
                          ▼
                 for each tile: initialise tile memory from loadOps
                                draw the primitives binned into this tile
                                resolve/compress inside tile memory
                                write back per storeOps
```

Three consequences, all of them architectural:

1. **Geometry is touched twice.** Vertex work costs what it costs twice over, and the intermediate
   is DRAM traffic. Vertex-bound content on a phone is binning-bandwidth-bound, not ALU-bound.
2. **Tile memory (GMEM / "tile buffer" / parameter buffer) is the only cheap memory.** A colour
   write that stays in a tile is free; one that leaves and comes back is 2× DRAM.
3. **The pass is the scheduling unit.** The tiler cannot start shading tile 0 of pass N+1 before the
   last tile of pass N if pass N+1 reads what pass N wrote — unless the two are the *same* pass
   (subpasses with a per-region dependency), in which case the hardware keeps the tile in place and
   just runs more draws on it.

### 2.2 What "keeping it on chip" is worth

Arm's measurement: `loadOp = LOAD` → `CLEAR` cuts external read bytes by **36 %**; `storeOp = STORE`
→ `DONT_CARE` cuts external write bytes by **62 %**; merged subpasses for a G-buffer cut reads by
**45 %** and writes by **56 %** on a Mali-G76. Their power figure is the one to hold in your head:
**~100 mW per GB/s** of external DDR bandwidth. On a 2.5 W device budget, the framebuffer is a
large minority shareholder.

Adreno says the same thing in its own words: binning mode uses GMEM as the local colour *and* depth
buffer, "the costly resolve operations can be minimized", and "Vulkan's Renderpass feature is highly
advantageous to tiling architectures". It adds one nuance nobody else says out loud: **subpass
merging only applies when the surface is rendered in binning mode** (Adreno FlexRender can choose
direct rendering instead, and then your subpasses buy nothing).

### 2.3 The three cliffs

| Cliff | Limit | What happens | What you do |
|---|---|---|---|
| **Tile-memory overflow** | Per-pixel tile storage is fixed per GPU generation: 128 bit/px on older Mali, 256 bit/px on G72-era, 1024 bit/px on G710. Adreno does not publish; it shrinks the bin instead. | The driver uses smaller tiles (more tile lists, more binning traffic) or spills. G-buffer-heavy passes silently lose the subpass win. | Budget the attachment set: `MAX_COLOR_ATTACHMENTS` in practice **4**, colours packed (`R11G11B10`, `RGB10A2`, `R8`), nothing wide that a subpass does not truly need. |
| **Parameter-buffer / intermediate overflow** | Mali Midgard/Bifrost pre-G77: fixed 180 MB region for varyings; `VK_ERROR_DEVICE_LOST` when exceeded. Indexed draws allocate across `[min_index, max_index]`. | Random device loss on real users' devices, not in your test scene. | Index coherence inside a mesh; split a pass whose vertex count approaches ~2 M × 64 B; never draw index `[0,1,100000]` for one triangle. |
| **Sysmem fallback** | If attachments do not fit, or a pass uses tessellation, or the render area is empty, the driver renders straight to memory. | Pass costs like a desktop pass. Turnip logs the reason (`force_render_mode_reason`). | Know which of your passes fall back. Tessellation/geometry shaders + tiling is a bad mix on mobile; prefer compute-generated geometry. |

### 2.4 Hidden surface removal is a pass-level property

HSR (PowerVR/Apple), LRZ (Adreno) and IDVS + forward-pixel-kill (Mali Valhall) all work at
**pass** granularity: the depth buffer is processed once per tile before any fragment work. Anything
that makes depth per-fragment-dynamic kills it for the *rest of the pass*:

- `discard` / `discard`-by-alpha-test in the depth-writing subpass,
- writing `gl_FragDepth`,
- alpha-to-coverage,
- blending (Adreno LRZ off for the draw and until the next depth clear),
- stencil operations,
- changing the depth-comparison direction mid-pass,
- masked depth writes,
- reading the depth attachment back.

`vk.h`'s reverse-Z presets (`CMP_GREATER`, clear to 0) are correct and help all three vendors. But
the toon renderer's **cel bands from `dFdx`/`dFdy` with alpha-tested silhouettes, plus three
depth-sampling passes**, is the worst case for HSR on mobile. This is a content problem the API can
only expose, not fix: it needs `PassNote` (already there) plus a per-pass "did this pass disable
HSR" flag derived from the shader's declared output (see §7.7).

### 2.5 Compute has no tile

A compute dispatch is not inside a render pass. If it runs between two render passes that share
attachments, the tile memory has to be flushed and refilled — a full-resolution store and load.
Ways to stay on chip, in order of availability:

| Mechanism | Support | Use |
|---|---|---|
| Input attachments + subpass (fragment only) | Vulkan 1.0, universal | Default. Per-pixel dependencies only. |
| `VK_EXT_rasterization_order_attachment_access` / `VK_ARM_rasterization_order_attachment_access` | Mali, Adreno; guarded | Read your own attachment in the same subpass without UB (framebuffer fetch). |
| `VK_EXT_attachment_feedback_loop_layout` | Adreno, Mali (recent) | Generalise the above to sampling the attachment. |
| `VK_KHR_dynamic_rendering_local_read` (Vulkan 1.4 / Roadmap 2024) | Newest only | The same win without a `VkRenderPass`. **Design for it, do not depend on it.** |
| `VK_QCOM_tile_shading` | Adreno | Per-tile compute inside a pass. Specialist. |

So: **a pass boundary is a hardware event, and the API's main job on mobile is to make the frame
have fewer of them.** Not fewer *logically* (the toon frame needs its 7 stages) but fewer
*pass boundaries* — which is exactly what subpasses and input attachments buy.

---

## 3. The ledger: the toon frame, costed

Formula, so these numbers can be recomputed at any resolution:

```
bytes/frame = W·H · Σ(per-pixel bytes read or written outside tile memory)
GB/s        = bytes/frame · fps / 1e9
power       ≈ GB/s · 0.1 W/(GB/s)          (Arm's figure)
```

At 2400×1080 (2.592 Mpx), 60 fps, RGBA8 = 4 B:

| Operation | B/px | MB/frame | GB/s @60 |
|---|---|---|---|
| one full-res target, write + later read | 8 | 20.7 | 1.24 |
| one full-res target, store only | 4 | 10.4 | 0.62 |
| 3 full-res targets (`R11G11B10`+`RGBA8`+`RGBA8`) stored | 12 | 31.1 | 1.87 |
| the same three, loaded again by the next pass | 12 | 31.1 | 1.87 |
| 2048² `D32F` shadow map, store + sample | 8 (of shadow texels) | 33.6 | 2.01 |

Now the current frame (`README.md`, "The frame") costed as written — pass-attached, dynamic
rendering, one barrier per pass, no subpass inputs:

| Pass | Attachments / traffic | MB/frame |
|---|---|---|
| `paper` (compute, once) | tiny | ~0 |
| `petals` (compute, per frame) | read+write arena | ~0 |
| `shadow` | 2048² `D16`/`D32` depth store | 8.4–16.8 |
| `scene` (4× MSAA, 3 targets, in-pass resolve) | resolve stores 3 × 4 B = 12 B/px; MSAA colour + depth stay on tile **if** transient — today they are not marked transient, so add 3 × 4 B × 4 samples + 4 B × 4 in the worst case | 31 (best) → 145 (worst) |
| `bloom` | 4 levels down + 3 up, ~1.33 × full-res × 8 B | ~27 |
| `composite` | loads 12 B/px (HDR + normal/depth + id) + bloom + paper, stores 4 B/px to the swapchain | ~55 |
| **total** | | **~150–270 MB/frame ≈ 9–16 GB/s** |

That is 1–1.6 W of pure framebuffer bandwidth on a device with a 2.5–4 W sustained GPU budget. The
wins, in order of size:

| Change | Saved | Why it is available |
|---|---|---|
| Composite reads the scene through **input attachments in the same pass** | 24 B/px = 62 MB/frame = **3.7 GB/s, ~0.4 W** | Removes both the 12 B store and the 12 B reload. Needs subpasses + `subpassLoad`. |
| Scene colour/depth marked **transient + lazily allocated** | up to 100 MB/frame in the bad case | Tile memory only; no allocation, no store, no load. |
| `storeOp = DONT_CARE` on every MSAA attachment with a resolve attachment in-pass | one whole MSAA store | Arm: "NEVER `STORE_OP_STORE` multisample attachments back to memory." |
| Bloom stays in tile memory as subpasses, or moves to `R11G11B10` + half-res | ~10–20 MB | Bloom is 4 reads + 4 writes of a full-res-scale chain. |
| Shadow: `D16_UNORM` not `D32F` | 8.4 MB | Halves the shadow store and every fetch. |
| Do not store depth after `scene` (`STORE_DISCARD`) | 4 B/px = 10 MB | Already expressible as `attach_clear(img,...).store = STORE_DISCARD` — check every call site actually sets it. |

The point is not that the toon renderer is wrong. It is that **the API currently cannot express the
first four rows**, and those four rows are 80 % of the available saving.

---

## 4. Device reality: what the API may assume

### 4.1 The two tiers

| Tier | Devices | What you may assume | What you must still query |
|---|---|---|---|
| **Tier A — Vulkan 1.3** (Android 15 CDD requires 1.3 for new chipsets; Adreno 6xx/7xx, Mali-G57+, PowerVR A/B) | shipping 2022+ | dynamic rendering, `synchronization2`, timeline semaphore, descriptor indexing (Roadmap 2022), `bufferDeviceAddress`, 16-bit storage, `drawIndirectCount`, `pipelineCreationCacheControl`, `shaderDemotetoHelperInvocation`, `subgroupSizeControl`, scalar block layout | EDS3, MSRTSS, AFBC/fixed-rate compression, descriptor buffers, `local_read`, tile properties |
| **Tier B — Vulkan 1.1/1.2** (the installed fleet: `VP_ANDROID_baseline_2021/2022`) | 2017–2021, still a large install base | 1.1 core, `create_renderpass2`, `descriptor_update_template`, `AHARDWAREBUFFER`, `GOOGLE_display_timing` | everything in Tier A |

Note the trap the profiles hide: **the Android Baseline profiles do not include
`VK_KHR_dynamic_rendering`, and do not include `VK_EXT_descriptor_indexing`.** A mobile abstraction
that is written against "Vulkan 1.3 is the baseline" will render nothing on the fleet. A mobile
abstraction written against 1.1 will leave 30–40 % of bandwidth on the table on new devices. So the
backend carries **two render paths behind one `PassDesc`**, and the feature table decides which.

### 4.2 Limits that shape the API (not just the backend)

These are the ones that force API changes rather than backend code:

| Limit | Value to design against | Consequence for `vk.h` |
|---|---|---|
| `maxPushConstantsSize` | **128** | `MAX_PUSH_CONSTANTS` → 128 + a UBO path (§7.1) |
| `maxColorAttachments` | 7 (profile floor), 4 practical on a tiler | `MAX_COLOR_ATTACHMENTS 8` fine, but keep real passes ≤ 4 |
| `maxComputeWorkGroupInvocations` | **256** (profile floor), 128 on older Mali | `PassNote.workgroup` and the API's compute presets: 64 or 128, not 1024 |
| `maxComputeSharedMemorySize` | 32 KB typical | shared-memory budget in the caps struct |
| `subgroupSize` | **16 on Mali, 64 (128 legacy) on Adreno, 32 on PowerVR** | subgroup-agnostic compute; no `subgroupSizeControl` assumption; reductions sized per-group, not per-wave |
| `maxPerStageDescriptorSampledImages` | 200 (floor); update-after-bind is the large one | bindless tier (§7.5) |
| `maxMemoryAllocationCount` | **4096 typical** | one `VkDeviceMemory` per memory type, suballocated (§8.5) |
| `nonCoherentAtomSize` | 32–256 depending; `HOST_COHERENT` often **absent** | every mapped write needs an explicit flush, so the arena write path must flush once per frame per range, not per allocation |
| `bufferImageGranularity` | up to 4096 | linear and optimal resources in one pool need padding; separate pools per `VkBufferImageCopy`-incompatible class |
| `timestampPeriod` / `timestampComputeAndGraphics` | often `timestampComputeAndGraphics = false` | `pass_scope()` must degrade to CPU timing when compute and graphics timestamps differ; put the flag in the caps struct |

---

## 5. What Mesa does, and the six rules it implies

Mesa is the best available vehicle for "how will a tiler react to my API call", because turnip
(freedreno) and panvk (panfrost) are the tiler drivers and because Mesa's common Vulkan runtime
(`src/vulkan/runtime`) is a worked example of *an abstraction over Vulkan written by people who
write drivers*.

### 5.1 The render pass is where the tiler's whole cost model lives

`tu_pass.cc:906 tu_render_pass_bandwidth_config()`:

```c
if (att->load)  pass->gmem_bandwidth_per_pixel  += att->cpp;   /* tu_load_gmem_attachment  */
if (att->store) pass->gmem_bandwidth_per_pixel  += att->cpp;   /* tu_store_gmem_attachment */
if (att->clear_mask)      pass->sysmem_bandwidth_per_pixel += att->cpp;
if (att->will_be_resolved) pass->sysmem_bandwidth_per_pixel += att->cpp + att->cpp / att->samples;
```

and `tu_cmd_buffer.cc:1419 use_sysmem_rendering()` picks direct-to-memory rendering with an explicit
reason string for each cause: *can't fit attachments into gmem*, *render area is empty*, *uses
tessellation shaders*, *XFB is incompatible with non-hw binning GMEM rendering*.

**Rule 1 — for every `PassDesc` the API must hand the driver the complete, final truth about
load/store/resolve, and the same `PassDesc` must recur every frame.** A backend that synthesises
`VkRenderPass` objects per frame (or varies the attachment list with what is visible) destroys both
the bandwidth model and the autotuner.

### 5.2 The driver autotunes per render pass, over time

`tu_autotune.h:21`: *"Autotune allows for us to tune rendering parameters (such as GMEM vs SYSMEM,
tile size divisor, etc.) based on dynamic analysis of the rendering workload via on-GPU profiling.
This lets us make much better decisions than static analysis…"*. The unit it keys on, `rp_key`
(`tu_autotune.cc:822`), is a hash of framebuffer dimensions, layers, and per-attachment
`{img_id, view_offset, load, store, load_stencil, store_stencil}`.

**Rule 2 — pass identity must be stable and cheap.** Concretely: your `PassDesc` → `VkRenderPass`
mapping should be a hash lookup that returns the *same* handle for the *same* attachments across
frames, and the framebuffer cache must return the same `VkFramebuffer` too. This is what makes the
autotuner converge instead of exploring forever.

### 5.3 Load/store collapse into four booleans

`tu_pass.cc:935 attachment_set_ops()` reduces `loadOp`/`storeOp`/stencil variants to
`att->load`, `att->store`, `att->clear_mask` (plus the `D24S8` packed-depth/stencil special case and
the `STORE_OP_NONE` "passthrough" case). Then `tu_pass.cc:586 tu_render_pass_cond_config()` computes,
per attachment:

```c
att->cond_load_allowed  = (att->load  || att->load_stencil)  && !att->clear_mask && !att->will_be_resolved;
att->cond_store_allowed = (att->store || att->store_stencil) && !att->clear_mask;
```

with the comment *"When there is no geometry in a tile … we can skip load/store"*.

**Rule 3 — `DONT_CARE` is not "I don't care"; it is `cond_load/store_allowed = true`, which lets the
hardware skip the tile entirely.** Every attachment in the API must be able to say it. `Load` has
`DONT_CARE`; `Store` needs `STORE_NONE`/`DONT_CARE` to be used deliberately, per attachment, and the
`D24S8` rule must be honoured at the API level: *packed depth/stencil must clear both on load and
discard both on store or it costs full bandwidth for nothing.*

### 5.4 Transient images are an API-level decision only

`tu_image.cc:1199`: *"Only expose the lazy memory type for images with `TRANSIENT_ATTACHMENT`"* —
i.e. if the application does not set the usage bit, the driver will not offer the memory type. And
MSRTSS in turnip allocates its implicit multisampled images *tied to the `VkFramebuffer`*
(`tu_device.cc:4346`).

**Rule 4 — `ImageDesc.transient` must be threaded all the way down** (usage bit, memory type,
forced `DONT_CARE`, no layout transitions needed, aliasing allowed through `arena_alias()`).

### 5.5 Dynamic state is a dirty-set, not a call

`vk_graphics_state.h:989` carries a `BITSET_DECLARE(dirty, …)` and
`vk_dynamic_graphics_state_any_dirty()`; drivers emit only the set that changed. That is exactly
`vk.c`'s `Pass.applied` / `run_open` shadowing, generalised.

**Rule 5 — keep the shadow, and extend it to the pass level**: bindless set bound once per command
buffer (already true), pipeline/state/viewport/scissor diffed (already true), and a *pass* is
"already current" if the same `VkRenderPass`+`VkFramebuffer` pair is open. On mobile, redundant state
calls are worse than on desktop because each one can cost a context roll in the tile stream.

### 5.6 Mesa gives you the frame pacer for free

`src/vulkan/runtime/vk_frame_pacer.h:17` — a `vk_frame_pacer` measuring frame time from "before the
first submission until after the last" and queueing delay, driven by calibrated timestamps, in
service of `VK_NV_low_latency2` / `VK_AMD_anti_lag`. It is the reference implementation of the
pacing model Android expects you to implement yourself (via `AChoreographer` + `VK_GOOGLE_display_timing`).

**Rule 6 — pacing is part of the renderer, and it needs the same three numbers Mesa measures:
frame time, queueing delay, and the deadline.**

Two more Mesa behaviours worth stealing outright:

- `src/vulkan/runtime/vk_render_pass.c` is a complete, driver-independent implementation of both
  `vkCmdBeginRenderPass2` **and** dynamic rendering, with layout tracking
  (`vk_command_buffer_get_attachment_layout`, `:1635`) and a `load_attachment()` fast path (`:1810`)
  that folds clears into `vkCmdBeginRendering` where possible. It is the shape to copy for the
  backend's two render paths: one internal "begin pass" that both entry points funnel into.
- `vk_framebuffer` + `vk_render_pass` are cached objects keyed by their create-info, with the
  attachment set stored flat (`vk_framebuffer.h:42 attachment_count`). Copy that: a hash table of
  `PassKey → {VkRenderPass, VkFramebuffer, refs}` inside `Gpu`.

---

## 6. API doctrine for mobile

Ten rules. Each one is falsifiable by a measurement in §10.

1. **The application describes intent; the backend owns scheduling.** No layouts, no barriers, no
   subpass indices, no framebuffers in `vk.h`. `PassDesc` is already this. Keep it that way.
2. **A pass boundary is expensive and therefore must be a deliberate act.** If a renderer can merge
   two stages without changing a pixel, the API must allow it: that means subpasses with
   `BY_REGION` dependencies and input attachments, not "two passes and hope".
3. **Every attachment declares its full life: load, store, sample count, resolve target, and whether
   it is transient.** A field the API cannot express becomes a store the driver cannot elide.
4. **Data that is only read once is not data; it is a step.** Bloom levels, half-res targets, SSAO
   buffers: the API should make it natural to keep them on tile, and unnatural to round-trip.
5. **The default must be the cheap path.** `attach_write()` = `DONT_CARE` (already), `image_depth()`
   = transient by default when no `use_sampled` is registered, `store = STORE_DISCARD` unless a
   later pass declares a read. Deriving `storeOp` from the frame's dependency graph is the backend's
   job, not the app's.
6. **Fixed capacity, no allocation in a frame.** Already the house style. On mobile it is also a
   correctness issue: `maxMemoryAllocationCount` is 4096.
7. **Nothing in the API may require a feature that is not in the declared tier.** No EDS3, no
   `descriptor_buffer`, no `local_read` in the mandatory path; each is a `GpuInfo` bit with a
   documented fallback.
8. **Setup failure is fatal where it happens; run-time failure (surface, swapchain) is a state, not
   a return code.** `gpu_resize` already models the second half. Android needs a third:
   `gpu_surface_lost()`.
9. **One submit per frame, one command buffer per frame, one thread records.** Secondary command
   buffers on a tiler are a way to break the pass into pieces the hardware cannot merge; Arm's own
   guidance is to minimise them.
10. **Measure in bytes and in tiles, not in frames per second.** The counters that mean something on
    mobile are external read/write bytes, physical tiles (`PTILES`), fragment jobs, HSR efficiency,
    and bin count. FPS hides all four.

---

## 7. Concrete deltas to `vk.h`

The header stays one file, one include, fixed capacities, `static inline` helpers, no allocation.
Below are the changes, with the code in `vk_mobile.h` (compilable, see §7.9).

### 7.1 Push constants: 256 → 128, plus a UBO path

```c
// today
#define MAX_PUSH_CONSTANTS 256
#define PUSH(name, BODY) ... _Static_assert(sizeof(name) == MAX_PUSH_CONSTANTS, ...)

// mobile
#ifndef MU_PUSH_BYTES
#define MU_PUSH_BYTES 128          /* maxPushConstantsSize floor; Adreno and Mali report 128 */
#endif
```

A 128-byte budget is enough for what push constants are *for* — offsets, an id, a 4×4 only if you
have nothing else. The toon renderer's `ScenePush` (a `mat4` view-projection + ids) is 64 + 8 = 72
bytes; it fits. Anything larger moves to the `ARENA_DEVICE` buffer that the shader already reads at
binding 3, addressed by a `uint32_t` offset in the push block — which is already how geometry works:

```c
PUSH(DrawPush,
    uint32_t vertex_offset;   /* ARENA_DEVICE */
    uint32_t index_offset;
    uint32_t constants;       /* ARENA_DEVICE: the per-draw block */
    uint32_t texture_id;
    uint32_t sampler_id;
    uint32_t pad[3];
);   /* 32 bytes, room to spare */
```

Two rules: the per-draw block must stay **wave-uniform** (read it into a scalar/UBO-like binding so
the compiler keeps it in uniform registers, not per-lane), and per-frame data (camera, exposure,
time) belongs in **one** per-frame block that every draw indexes at offset 0, so the CPU writes it
once.

### 7.2 Subpasses and input attachments

This is the one real API addition. It costs four things: a `SubpassDesc`, an `IMAGE_INPUT` use
type, a `subpass_count` on `PassDesc`, and a `subpassLoad`-capable binding in the shader ABI.

```c
#define MAX_SUBPASSES 4

typedef struct SubpassDesc {
    Attach    colors[MAX_COLOR_ATTACHMENTS];
    uint32_t  color_count;
    Attach    depth;
    bool      has_depth;

    ImageUse  images[MAX_IMAGE_USES];   /* IMAGE_INPUT entries here read the tile */
    uint32_t  image_count;

    BufferUse buffers[MAX_BUFFER_USES];
    uint32_t  buffer_count;

    Pipe      pipe;         /* a subpass may begin with its own pipe */
    DrawState state;
    uint32_t  by_region;    /* default 1: a per-pixel dependency. 0 only for a deliberate full barrier */
} SubpassDesc;
```

and in the pass helpers:

```c
// The scene's HDR colour as an input attachment: subpassLoad(4, i) in the fragment shader.
static inline ImageUse use_input(Image img) {
    return (ImageUse){ .image = SUB(img), .access = ACC_READ, .type = IMAGE_INPUT };
}
```

Backend rules, non-negotiable:

- Every intra-pass dependency is `VK_DEPENDENCY_BY_REGION_BIT`. Without it the hardware waits for
  *all* pixels of the previous subpass, and merging is off (`VK_SUBPASS_EXTERNAL` deps stay external).
- An input attachment's layout is `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` at the end of the pass
  and `GENERAL`-free inside it: for subpass inputs the layout is
  `VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ`/`COLOR_ATTACHMENT_OPTIMAL` with the read via
  `INPUT_ATTACHMENT_READ`, which turnip and panvk implement on tile memory.
- `vkCmdNextSubpass` is the entire cost of the mapping and it is nearly free **only if** the
  attachment sets are compatible and the deps are by-region. If a pass would not merge, the driver
  will spill and the app has gained nothing but has lost nothing either.
- Depth read in a later subpass: `DEPTH_STENCIL_READ_ONLY` layout (Arm's explicit recommendation).
- **Subpasses only exist in the `VkRenderPass` path.** With dynamic rendering on a device without
  `local_read`, the backend must *fall back* to separate passes — which is precisely the case
  `use_input` must be able to detect and report through `GpuInfo`.

### 7.3 Attachment flags: transient, lazy, resolve, store-none

```c
typedef enum Load  { LOAD, CLEAR, DONT_CARE, LOAD_NONE } Load;   /* NONE: never touched */
typedef enum Store { STORE, STORE_DISCARD, STORE_NONE } Store;   /* NONE: pass did not write it */

typedef struct ImageDesc {
    ...
    bool transient;   /* usage |= TRANSIENT_ATTACHMENT, memory preferred LAZILY_ALLOCATED,
                         store forced to DONT_CARE, layout is pass-local */
} ImageDesc;
```

Derivation rules the backend owns (so the app cannot get them wrong):

| App says | Backend does |
|---|---|
| `transient = true` | `usage |= TRANSIENT_ATTACHMENT_BIT`, `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` preferred (fall back to `DEVICE_LOCAL` when no memory type has it — turnip does exactly this at `tu_image.cc:1199`), `DONT_CARE` on load if never sampled, `DONT_CARE` on store, memory from `ARENA_TRANSIENT` with `arena_alias()` reuse |
| `samples > 1` and a `resolve` in the same pass | resolve attachment's `storeOp = STORE`, MSAA `storeOp = DONT_CARE`, `resolve_mode` from `Attach.resolve_mode` (`AVERAGE`, `SAMPLE_ZERO`, `MIN`, `MAX`) |
| any `D24S8` attachment | depth and stencil get the same load/store op. Packed formats pay full bandwidth for one aspect (`tu_pass.cc:935`) |
| store never read later this frame | `STORE_DISCARD` — computed by the frame's declaration graph, not asked of the app |

### 7.4 Capabilities as data

`GpuInfo` grows the fields that change *application* decisions, not just backend ones:

```c
typedef enum Feature {
    F_DYNAMIC_RENDERING, F_SYNCHRONIZATION2, F_TIMELINE_SEMAPHORE, F_DESCRIPTOR_INDEXING,
    F_BUFFER_DEVICE_ADDRESS, F_DRAW_INDIRECT_COUNT, F_EDS1, F_EDS2, F_EDS3,
    F_SAMPLED_DEPTH_READ, F_ATTACHMENT_FEEDBACK_LOOP, F_RASTER_ORDER_ATTACHMENT_ACCESS,
    F_SUBPASS_INPUT, F_LAZY_MEMORY, F_TRANSIENT_ATTACHMENT, F_MSRTSS,
    F_FIXED_RATE_COMPRESSION, F_DESCRIPTOR_BUFFER, F_LOCAL_READ, F_TILE_PROPERTIES,
    F_SHADER_FLOAT16, F_STORAGE_16BIT, F_SUBGROUP_SIZE_CONTROL, F_PIPELINE_CACHE_CONTROL,
    F_TIMESTAMP_COMPUTE_AND_GRAPHICS, F_YCBCR_CONVERSION, F_EXTERNAL_HARDWARE_BUFFER,
    F_PREROTATION, F_DISPLAY_TIMING, F_PERFORMANCE_QUERY,
    F_COUNT
} Feature;

typedef struct GpuCaps {
    uint32_t features;          /* bits over Feature */
    uint32_t push_bytes;        /* 128 today, 256 on desktop */
    uint32_t max_color_attachments;
    uint32_t max_subpasses;     /* 1 where subpass inputs are unavailable */
    uint32_t sampled_images_max;    /* the update-after-bind bound actually granted */
    uint32_t storage_images_max;
    uint32_t samplers_max;
    uint32_t workgroup_invocations;
    uint32_t workgroup_shared_bytes;
    uint32_t subgroup_size;
    uint32_t memory_allocation_max;
    void    *non_coherent_atom; /* alignment for flush/coherency granularity */
    bool     host_coherent;     /* if false, every mapped write is flushed once per frame */
} GpuCaps;

bool gpu_have(const Gpu *g, Feature f);
```

`bool gpu_have()` is one shift and one test; app code branches on it once at setup and stores the
result in a table, never per draw (house style: *existence over flags*).

### 7.5 Bindless in three tiers

| Tier | Condition | ABI | Notes |
|---|---|---|---|
| **A** full bindless | `F_DESCRIPTOR_INDEXING`, update-after-bind limits ≥ requested | one descriptor set, `binding 0/1/2` arrays as today, sizes from the *granted* limits | Bind once per command buffer. This is today's design and it survives. |
| **B** clamped bindless | `F_DESCRIPTOR_INDEXING` with smaller update-after-bind limits | arrays sized to `min(requested, granted)`, `image_sampled_id` returns 0 (the white dummy) past the cap and the backend logs one line | Practically: clamp `MAX_BINDLESS_TEXTURES` to 4096–16384. |
| **C** descriptor sets per pass | no descriptor indexing | `PassDesc` grows `size_t set_key`; the backend caches `VkDescriptorSet` per (pipe, texture-set) and binds at pass start | Costs one `vkCmdBindDescriptorSets` per pass — cheap, and still one per pass, not per draw. |

The shader ABI must not change between tiers: keep the arrays, and let C bind a small set with the
same bindings. Slang can emit both from one source with a `MU_BINDLESS` define (the `ShaderDesc.defines`
field already exists for exactly this).

Also: on tier A, **`VK_EXT_descriptor_buffer` on Adreno and Mali removes `vkUpdateDescriptorSets`
entirely** — worth a `F_DESCRIPTOR_BUFFER` experiment, not a dependency, because the descriptor
buffer path needs its own allocation strategy and `descriptorBufferImageLayoutIgnored` behaviour
varies.

### 7.6 Vertex pulling: verdict

Keep it. The reasons that made it right on desktop are stronger on mobile:

- Binning reads positions only. With pulling, the tiler's vertex stage reads the full vertex — so a
  32-byte vertex for `{pos, normal, uv, tangent}` is 2.7× the binning traffic of a 12-byte
  `{pos}` array. **Fix: split the stream.** `ARENA_DEVICE` layout: all positions packed
  (`R16G16B16A16_SNORM` or `R32G32B32`), then all other attributes in a parallel array. The
  binning pass touches array 0; the fragment pass touches array 1.
- It keeps `topology` out of the pipeline key (already true) and lets an indirect draw carry its own
  offsets.
- The dependent load (index → vertex) is one more latency hop; on Mali the warp is 16 wide, so
  latency hiding is thin. Mitigate by issuing the index load first and the position load into a
  register early, or by pre-transforming the index into a byte offset in the compute cull.

Escape hatch: `BUF_VERTEX`/`BUF_INDEX` already exist in `BufferUsage`. A `GpuCaps` bit
(`F_VERTEX_PULL` off) can select a real `vkCmdBindVertexBuffers` + `vkCmdBindIndexBuffer` path for
A/B measurement. Do not ship the second path without a number.

### 7.7 What the API should refuse to let the app say

Removing expressiveness is a feature here. The mobile backend should not accept:

- `Access`/layout on an image that is only ever an attachment (infer from `Attach`).
- A compute pass in the middle of a frame's render passes on the `VkRenderPass` path (assert, then
  merge it into the next pass's external dependency).
- `vkCmdClearAttachments`-style clears: `Load::CLEAR` only, at pass start, full attachment.
- A `PassDesc` whose attachments exceed the tile budget (`> 4` colour or total bytes/px above a
  device-derived budget) without a `pass_note()` explaining it — a warning in debug, never a silent
  sysmem fallback.

---

## 8. Backend: `vk.c` as a mobile driver

Same 13 sections, same order, same house rules (fatal setup, no `if` ladders, fixed tables).
What changes is per-section.

| Section | Mobile change |
|---|---|
| 1 state | `PassKey`/`FramebufferKey` hash tables; `Pass` gains `subpass` index and the cached `VkRenderPass`; `Gpu` gains `caps`, `render_path` (dynamic render / render pass), `mapped_heap[]`. |
| 2 errors and names | `VK_EXT_device_fault` where present; `vkGetPhysicalDeviceFeatures2` chain becomes a static table of feature structs, not a chain of `if`. |
| 3 timeline and retirement | Tier A: as today. Tier B: one `VkFence` per frame slot + the same `Retired[]` list keyed by frame index, and `completed` = the oldest unsignalled fence. |
| 4 command buffers | One pool per frame slot, `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT`. Never `RESET_COMMAND_BUFFER` on Adreno mid-frame. |
| 5 barriers | The big rewrite. `begin_pass` becomes: (a) compile-or-find the `VkRenderPass`; (b) if a pass is open and compatible, nothing; (c) emit *external* dependency barriers only for buffers/arena; (d) attachments are never transitioned by hand — the pass does it. Counters `FrameStats.barriers` should go to zero for attachment-only frames. |
| 6 gpu create/destroy | Tier detection, `GpuCaps` fill, memory heap discovery, `VK_KHR_performance_query` opt-in, pipeline cache path from the platform layer. |
| 7 buffers, arenas | UMA heap allocator (§8.5); `ARENA_STAGING` aliases the mapped heap. |
| 8 images, samplers | `TRANSIENT_ATTACHMENT` + lazy memory; `VK_IMAGE_CREATE_...` flags from `ImageDesc`; image views cached per (image, mip, layer, aspect) as today (`MAX_IMAGE_VIEWS 16` is right). |
| 9 shaders and pipelines | `PipeKey` order: `{vs, fs, formats, samples, blend, write_mask, view_mask, subpass}` (only fields in `Gpu.static_state` for EDS3-less devices). `VK_PIPELINE_COMPILE_REQUIRED_EXT` + background compile + `VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`. Pipeline cache at `Context.getCodeCacheDir()`. |
| 10 swapchain, frame | Pre-rotation via `preTransform`; `minImageCount = 3`; `frame_begin` = `vkAcquireNextImageKHR` + `AChoreographer` deadline; surface-lost state. |
| 11 passes and commands | Two render paths behind one function pair (§8.2). |
| 12 upload and readback | UMA direct write instead of staging; one flush per range per frame; readback via `HOST_CACHED` memory or a small staging ring. |
| 13 profiling | `VK_KHR_performance_query` optional; `timestampComputeAndGraphics = false` degradation; the mobile counters of §10 into `FrameStats`. |

### 8.1 The pass compiler

```
PassDesc ──hash──► PassKey ──lookup──► { VkRenderPass, refs }
                     │                        │
                     │                        ├─ image views ──hash──► FramebufferKey ──► VkFramebuffer
                     └─ miss: build VkAttachmentDescription[], VkSubpassDescription[],
                              VkSubpassDependency[] (external edges, BY_REGION),
                              vkCreateRenderPass2 → store
```

- The table is fixed capacity (`MAX_PASSES 64` today is too small once subpass variants exist;
  256 entries × ~48-byte key is 12 KB, fine).
- Keys are compared byte-wise over a packed struct (`PipelineFile`'s "no padding, `_Static_assert`
  the offsets" trick).
- Zero allocations after the first occurrence of each pass; a per-frame `PassDesc` that matches
  costs one hash and one memcmp.
- On Tier A this table caches the *derived* pass information (validated attachment sets, resolve
  legality, `PipelineRenderingCreateInfo`), so the dynamic-rendering path also avoids recomputation.

`pass_cache.c` in this directory is a working, compilable sketch of exactly this, including the
key layout and the framebuffer cache.

### 8.2 The two render paths

```c
// one entry point, two paths; the app never knows which
static void pass_begin(Gpu *g, const PassDesc *d) {
    // find or build the cached pass + framebuffer  (§8.1)
    // Tier A: vkCmdBeginRendering with PipelineRenderingCreateInfo from the same key
    // Tier B: vkCmdBeginRenderPass (or 2) with the cached VkFramebuffer
    // either way: no attachment barriers, no layout transitions issued by us
}
```

Rules that make the two paths behave the same:

- On Tier A, `VkRenderingAttachmentInfo.imageLayout` is the layout the pass *ends* in; the backend
  still tracks the last layout per mip (as today) and transitions only at frame boundaries.
- On Tier B, the `VkRenderPass` owns the transitions; `initialLayout` is `UNDEFINED` for a pass that
  clears, `COLOR_ATTACHMENT_OPTIMAL` for one that loads.
- Both paths must emit the same resolve semantics from `Attach.resolve`.
- **Subpasses exist only on Tier B**, so a `PassDesc` with `subpass_count > 1` on a Tier A device is
  compiled into N dynamic-rendering passes with the same attachments and `load = LOAD` between them,
  *and* the backend counts it in `FrameStats` so the cost is visible in the stats output rather than
  being a mystery.

### 8.3 Synchronisation, rewritten in the small

Today: `begin_pass()` → `vkCmdPipelineBarrier2` with up to `MAX_IMAGE_BARRIERS 256` image barriers.
Mobile:

1. Attachment dependencies never produce barriers. They produce `VkSubpassDependency` entries at
   pass creation (`src/dstSubpass = VK_SUBPASS_EXTERNAL` for the frame edges) and `BY_REGION`.
2. Buffer/arena dependencies produce one barrier, batched, emitted **before the first pass that
   needs it**, not per pass.
3. `Sync` (per mip, as today) keeps working for the cases that need it — a storage image written in
   a compute pass and sampled by the next pass — but the transition is a *global* memory barrier
   outside any render pass, which is cheap; the expensive thing is a barrier *between two passes on
   the same attachments*, and the design's job is to make that not exist.
4. `ARENA_TRANSIENT` memory is reset per frame, so its barrier is per frame, not per allocation.

### 8.4 Pipelines and the stutter budget

Numbers to design against: a mid-range phone takes **tens of milliseconds to hundreds of
milliseconds** to compile one nontrivial pipeline, and the driver may compile on first bind. The
mitigations, in order:

1. **Pipeline cache blob from disk**, per (vendor, device, driver version, `pipelineCacheUUID`) —
   `PipelineFile` already does exactly this, including a "no padding" header and a size/hash guard.
   Move the path to the app's code cache directory.
2. **`VK_EXT_pipeline_creation_cache_control` / `pipelineCreationCacheControl`** (1.3 core):
   create with `FAIL_ON_PIPELINE_COMPILE_REQUIRED`, and if it fails, place a *fallback* pipeline
   (a simpler shader that renders something) and schedule the real compile on a worker thread.
3. **`VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`** so worker threads can create
   pipelines concurrently into one cache; `Pipe` handles are already `{index, gen}`, so interning
   per key with a lock-free publish is cheap.
4. **Pre-warm at load**: enumerate the passes, create every `VkRenderPass`, and create every
   pipeline for the (pipe × format-set × static-state) combinations that exist, behind a loading
   screen. The toon renderer's variant set is 19 pipelines today; on mobile the same scene at EDS3
   off, 2 sample counts, 3 attach formats will be ~30–60. That is a minute of phone compiler for
   ~0.5 MB of cache — acceptable once, fatal per frame.

### 8.5 Memory: one heap per memory type

```
vkGetPhysicalDeviceMemoryProperties
  → for each memory type with HOST_VISIBLE: one VkDeviceMemory of ~64–256 MB, vkMapMemory once
  → ARENA_DEVICE / ARENA_TRANSIENT / ARENA_STAGING bump-allocate from the mapped heap
  → LAZILY_ALLOCATED types: no allocation, bind with VK_NULL_HANDLE (transient images)
```

- Check `maxMemoryAllocationCount` (4096 typical) and assert it covers `heap_count` + swapchain.
- `bufferImageGranularity` up to 4096: keep a linear-only pool and an optimal-only pool, or pad
  every allocation to the granularity. The current single-pool design will fault validation on
  devices where the granularity is not 1, so pool-per-class is the fix.
- `HOST_COHERENT` is often false on Mali. The rule: **flush once per frame per contiguous range**,
  at `frame_end`, using `nonCoherentAtomSize`-aligned ranges — never per allocation. Keep bookkeeping
  data (`ArenaStats`) in CPU memory, never in the mapped range (the mapping is write-combining;
  reading it back is ~0.27 GB/s vs 28 GB/s).
- `VK_EXT_memory_budget` where present: keep the total mapped heap well under the budget, because
  the OS will kill you before it returns `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- Trim on Android's `onTrimMemory` callback: free the pipeline cache blob, transient arenas, and any
  cached readback.

### 8.6 Present

- `minImageCount = 3` (Android's default is 3; more means more latency, fewer means blocking).
- `preTransform = currentTransform` and render in the native orientation. That is the whole
  pre-rotation story: swapchain matches display native size, MVP is adjusted, and **fragment
  derivatives** must be rotated too if you scale/rotate UVs — the reason the toon renderer's
  `dFdx`-driven cel bands need an explicit check on rotation.
- `VK_QCOM_render_pass_transform` only if `F_PREROTATION`; it costs an extension struct on every
  `vkCmdBeginRenderPass` and has an early-driver `sType` bug (Adreno `< 512.469.0`), so it is opt-in.
- One `vkQueuePresentKHR` per frame with `VK_PRESENT_MODE_FIFO_KHR`. `MAILBOX` is not available on
  Android surfaces in practice; `VK_KHR_present_mode_fifo_latest_ready`/`surface_maintenance1` is
  where the jank win is, on the newest OS only.

---

## 9. Android platform layer

`platform.h`/`platform.c` (GLFW on Linux) becomes `platform_android.c`: `android_native_app_glue`
or `GameActivity`, one `ANativeWindow` at a time, and a Vulkan surface created/destroyed with it.

| Event | What the layer must do |
|---|---|
| `APP_CMD_INIT_WINDOW` | `vkCreateAndroidSurfaceKHR`, `gpu_set_surface`, create the swapchain |
| `APP_CMD_TERM_WINDOW` | `gpu_wait_idle`, destroy the swapchain **and** the surface before the window goes away |
| `APP_CMD_WINDOW_RESIZED` / rotation | `gpu_resize`, recreate the swapchain, `preTransform` from the new `currentTransform` |
| `onTrimMemory` | drop caches (pipeline blob stays; transient arenas and readbacks go) |
| `AChoreographer` frame callback | `frame_begin()`; present at the callback, not in a `while(1)` |
| ADPF (`APerformanceHint`) | report the actual frame duration each frame; take the thermal headroom as an input to quality (resolution scale, sample count) |
| `VK_GOOGLE_display_timing` | `vkGetRefreshCycleDurationGOOGLE` + `vkGetPastPresentationTimingGOOGLE` into `FrameStats` — that is your jank counter |

The loop, in the API's terms:

```
AChoreographer.postFrameCallback
  → frame_begin(g)            // acquire, deadline from the callback's frameTimeNanos
  → scene draws (passes)
  → frame_end(g)              // submit, present with the timing struct
  → onPresentTiming(...)      // feeds FrameStats.present_ms, missed frames
```

Non-obvious requirements:

- **The render thread must not be the UI thread**, and Vulkan surface creation must happen on the
  thread that owns the window.
- **Validation layers on Android are 10–50× slower.** `--validate 1` must be a debug-only default.
- **`VK_ERROR_SURFACE_LOST_KHR` is normal**, not exceptional: `gpu_surface_lost()` + recreate.
- Format: query `vkGetPhysicalDeviceSurfaceFormatsKHR`, prefer `VK_FORMAT_R8G8B8A8_UNORM` (the
  current `FMT_RGBA8_UNORM` + manual display encode in the last pass is exactly right — Android
  surfaces are UNORM + `SRGB_NONLINEAR` colour space, so a shader-side encode is correct and an
  `_SRGB` swapchain is the wrong choice).

---

## 10. Measurement protocol

### 10.1 What to count

| Counter | Where | Target |
|---|---|---|
| External read/write bytes per frame | Arm Streamline (Mali), Snapdragon Profiler / AGI (Adreno) | ≤ 2× the minimum the algorithm needs; the ledger in §3 is the model |
| Physical tiles (`PTILES`) | Streamline | equal to `ceil(W/16)·ceil(H/16)` for a single-pass frame; a jump means a pass broke |
| Fragment jobs / `FRAG_JOBS` | Streamline / Adreno "fragment shading" | merge ratio visible when subpasses are used |
| HSR / LRZ efficiency (`Z_PRE_PASS_PIXELS`, Adreno `LRZ` counters) | profilers | non-zero; zero means something in the pass disabled it |
| `VK_KHR_performance_query` counters | in-app, if available | opt-in only, never in release |
| Pass GPU time | `FrameStats.passes[]` (already exists) | the pass that got slower is the pass that fell back to sysmem |
| Pipeline creation time | `vkGetPipelineCacheData` hit rate + a CPU timestamp around creation | cache hit rate > 95 % after first run |
| Frame deadline misses | `VK_GOOGLE_display_timing` | counted, not averaged |

### 10.2 The measurement matrix

Every optimisation in this document is applied alone, on this matrix, at 2400×1080:

| Axis | Values |
|---|---|
| device class | one Adreno 6xx, one Adreno 7xx, one Mali-G7x, one Mali-G5x (Tier B path) |
| render path | dynamic rendering (Tier A) vs `VkRenderPass` (forced), and `MU_NO_EDS3=1` on desktop as the pipeline-variant proxy |
| pass shape | as today (7 passes) vs merged (scene+composite via subpass inputs) |
| attachment flags | transient on/off, `STORE_DISCARD` where legal on/off |
| MSAA | off / 4× with in-pass resolve / 4× via MSRTSS where available |

### 10.3 Acceptance gates (per stage, §12)

1. `FrameStats.barriers` for an attachment-only frame = 0.
2. External write bytes per frame ≤ the §3 ledger for the same output image, ±10 %.
3. No `maxMemoryAllocationCount` growth after frame 3 (i.e. no allocation in the steady state).
4. Every `PassDesc` in the frame resolves to a cached pass and framebuffer from frame 2 onwards.
5. Pipeline creation count = 0 after the pre-warm, and the cache hit rate holds across process
   restarts.
6. Same pixels as the desktop backend (`--shot` reference images, bit-exact where the formats allow,
   perceptual otherwise).

---

## 11. Failure catalogue

Ordered by how often it kills a port. Each is a bug in the abstraction, not in the app.

1. **`vkCreatePipelineLayout` fails** because the push range is 256 bytes and the device offers 128.
2. **`vkCreateImage` fails / the transient path silently costs full bandwidth** because
   `TRANSIENT_ATTACHMENT` was never set (turnip will not even offer the lazy memory type).
3. **`VK_ERROR_OUT_OF_DEVICE_MEMORY` at ~4096 allocations** — one `VkDeviceMemory` per buffer.
4. **Random `VK_ERROR_DEVICE_LOST`** on Mali Midgard/Bifrost from the 180 MB parameter-buffer limit,
   triggered by an incoherent index range in a single draw.
5. **Bandwidth 3× the budget** because a barrier between two passes touching the same attachment
   turned a tile-resident chain into two DRAM round trips.
6. **A compute pass in the middle of the frame** with the same effect, plus a lost binning overlap.
7. **Nothing renders on a Tier B device** because the backend only has dynamic rendering.
8. **Bindless arrays exceed `maxPerStageDescriptorUpdateAfterBindSampledImages`** →
   `vkCreateDescriptorSetLayout` fails, or worse, succeeds with a clamped limit and the shader
   indexes past it.
9. **Torn / stale frames on rotation** because `preTransform` was not set and the compositor is
   doing the rotate.
10. **A black frame after backgrounding** because the surface was destroyed but the swapchain was
    not.
11. **Stutter on first draw of each new pipeline** — no disk cache, no background compile.
12. **Coherency bugs on Mali** — mapped writes never flushed, or flushed per allocation (~0.1 ms
    each).
13. **Reading a storage buffer through the mapped pointer** — the mapping is write-combining;
    loads are ~100× slower than cached.
14. **`bufferImageGranularity` aliasing** — linear and optimal resources in one arena without
    padding; validation error on one device, corruption on another.
15. **Subpass with a global (non-by-region) dependency** — the tile merge silently stops happening
    and the frame gets slower for no visible reason.
16. **`vkCmdClearAttachments` / shader clears** — a per-fragment clear where a `loadOp` was free.
17. **`loadOp = LOAD` where the pass fully overwrites** — a full-screen read per attachment per frame.
18. **`storeOp = STORE` on a multisampled attachment** — Arm: never. One full MSAA store.
19. **`D24S8` with only one aspect declared** — full bandwidth for half the data.
20. **8 `MAX_COLOR_ATTACHMENTS` used for real** — tile memory per pixel exceeded, bins shrink,
    tile-list traffic rises.
21. **`discard`/`gl_FragDepth`/alpha-to-coverage in the depth-writing subpass** — HSR/LRZ off for
    the rest of the pass; every later pixel costs a depth test.
22. **Timestamps read as if `timestampComputeAndGraphics` were true** — nonsense `gpu_ms` for
    compute passes on mobile.
23. **Validation layers shipped on** — 10–50× slowdown, and on some Adreno drivers a crash.
24. **`vkDeviceWaitIdle` to change MSAA / resize** — a multi-frame stall; the swapchain and the
    transient images are the only things that must be recreated.
25. **Pipeline cache blob written without the device/driver guard** — the `PipelineFile` header
    exists for this; on Android a driver OTA changes `pipelineCacheUUID` and a stale blob can crash
    the compiler.

---

## 12. Port plan

Six stages. Each has an exit gate from §10.3 and produces a shippable state.

| Stage | Work | Gate |
|---|---|---|
| **0 — proxy on desktop** | Nothing changes in `vk.h`. Add `MU_NO_EDS3=1` to the CI run, add a `FrameStats` line for bytes/frame (computed from the declarations), and add a fake 128-byte push build (`-DMU_PUSH_BYTES=128`) that fails to compile the demo until its push blocks are fixed. | The demo runs on desktop through the mobile pipeline key and the mobile push ABI. |
| **1 — platform** | `platform_android.c`, surface lifecycle, swapchain, pre-rotation, the frame loop on `AChoreographer`, pipeline cache in the code-cache dir. All Tier A, dynamic rendering, no subpasses, no transients. | Runs at 60 fps on one Adreno and one Mali device with validation off; cache hit rate > 0. |
| **2 — the pass compiler** | §8.1 + the Tier B path. Attachment barriers → subpass deps. `transient` wired to hardware. `STORE_DISCARD` derived, not asked. | Gate 1, 2, 4. Barriers per frame = 0. |
| **3 — subpasses / input attachments** | `SubpassDesc`, `use_input`, `IMAGE_INPUT` binding, Slang ABI, the composite merged into the scene pass. | 3.7 GB/s off the frame at 2400×1080 (§3), pixels match. |
| **4 — memory and bindless tiers** | UMA heap allocator, pool-per-class, flush-once-per-frame, bindless clamp + tier C. | Gates 3, 5. Runs on the Tier B device with 200-entry descriptor limits. |
| **5 — measure and tune** | MSRTSS, `D16` shadows, fixed-rate compression where offered, `VK_KHR_performance_query` counters, ADPF-driven resolution scale. | Gate 6 + sustained 30 min without thermal throttle below the target. |

The order matters: stage 0 costs a day and prevents stages 2–4 from being written twice.

---

## 13. References

**This repository**
- `README.md` — the frame, the conventions, what `vk.c` decides.
- `vk.h` §13–§20 — `PassDesc`, the pass helpers, the push ABI.
- `vk.c` header comment — the shader ABI, geometry, conventions, pipelines, sync, timeline, present.
- `graphics_finding/docs/pipeline-explosion.md` — the RADV-dynamic-state assumption this document
  revises for mobile (§4.1, decision 11).
- `graphics_finding/skills/vulkan-opt/OPTIMIZATION-CATALOG.md` — the pass/bandwidth budget rows are
  the desktop half of §3.
- `external/mu_lib/AGENTS.md` — the style this document writes in.

**Mesa** (paths as of the tree cloned for this document)
- `src/freedreno/vulkan/tu_pass.cc:906` `tu_render_pass_bandwidth_config` — gmem vs sysmem bytes.
- `src/freedreno/vulkan/tu_pass.cc:935` `attachment_set_ops` — load/store → four booleans, `D24S8`.
- `src/freedreno/vulkan/tu_pass.cc:586` `tu_render_pass_cond_config` — conditional load/store.
- `src/freedreno/vulkan/tu_cmd_buffer.cc:1419` `use_sysmem_rendering` — the fallback reasons.
- `src/freedreno/vulkan/tu_autotune.h:21`, `tu_autotune.cc:822` `rp_key` — per-pass autotuning.
- `src/freedreno/vulkan/tu_device.cc:4346` — MSRTSS images tied to the framebuffer.
- `src/freedreno/vulkan/tu_image.cc:1199` — lazy memory only for `TRANSIENT_ATTACHMENT`.
- `src/vulkan/runtime/vk_render_pass.c:1810,1891,2644` — one implementation, both render paths.
- `src/vulkan/runtime/vk_graphics_state.h:989` — dynamic state as a dirty bitset.
- `src/vulkan/runtime/vk_frame_pacer.h:17` — frame time and queueing delay.
- `src/panfrost/vulkan/panvk_vX_nir_lower_input_attachment_loads.c` — input attachments → tile
  memory loads on a tiler.

**Vendor and Khronos**
- Arm, *GPU Best Practices* (rev 3.4) and *Understanding Render Passes* (102479) — load/store
  savings (36 %/62 %), subpass merging (45 %/56 %), MSAA in tile memory, the 180 MB limit, packed
  depth/stencil, `~100 mW per GB/s`.
- Arm, *Efficient render passes with Vulkan*, *Picking the most efficient load/store operations*.
- Qualcomm, *Using Vulkan with Adreno* (80-NB295-7) — binning, GMEM, LRZ kill-list, render pass
  fusion, FlexRender/direct mode.
- Khronos, *Tile Based Rendering Best Practices*; *Vulkan Roadmap Milestones* (Roadmap 2022 /
  2024); Vulkan Profiles `VP_KHR_roadmap_2022`, `VP_ANDROID_baseline_2021/2022`.
- `VK_EXT_multisampled_render_to_single_sampled` proposal; `VK_QCOM_render_pass_transform`;
  `VK_KHR_dynamic_rendering_local_read`; `VK_EXT_load_store_op_none`.
- Android: *Handle device orientation with Vulkan pre-rotation*; Android 15 CDD (Vulkan 1.3 for new
  chipsets); ADPF; `VK_GOOGLE_display_timing`.
