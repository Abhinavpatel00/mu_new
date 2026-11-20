# The 2026 mobile-Vulkan corpus: what it adds to `mg`

A second research pass, after `MOBILE_TALKS.md`. That document established *why* `mg` looks the way
it does (CPU-submitted draws, 4-slot materials, passes as compile units, fixed-function fetch). This
one is about **correctness and bandwidth details I had wrong or missing**, taken from sources published
since, and it changes the API in five concrete places.

Every claim below is a quote or a direct consequence of one. Sources first.

---

## 1. The sources, and why these ones

| Source | Who / when | Why it matters here |
|---|---|---|
| **"How to write a Vulkan application in 2026"** — [slides][kv26], [tutorial][kvt26] | Iago Calvo Lista (Arm), Ralph Potter (Samsung, **chair of the Vulkan WG**), José-Emilio Muñoz-López (Arm). SIGGRAPH 2026, July | The Khronos-recommended baseline as of *this year*, and an explicit statement about what mobile drivers do and do not have |
| **"Getting started on mobile & best practices for Arm GPUs"** — [Vulkanised 2023][v23] | Pete Harris (Arm) | AFBC's exact API requirements, tile-memory rules, render-pass overlap, DPU pre-rotation |
| **"Understanding Render Passes"** — [Arm doc 102479][rp] | Arm | Packed depth-stencil, clear/blit traps, MSAA bandwidth arithmetic |
| **"Modern Mobile GPU Architecture"** — [MMG 2026][rp26] | Ralph Potter (Samsung) | What mobile GPUs actually render, and the thermal baseline |
| **"GPU-driven Rendering on Mobile"** — [MMG 2026][qc26] | R. H. Reis (Qualcomm) | already in `MOBILE_TALKS.md`: indexed > indirect, measured |
| Roblox GDC 2020 / Reboot 2019, HypeHype 2023–24 | Kapoulkine, Aaltonen | already in `MOBILE_TALKS.md` |

[kv26]: https://docs.vulkan.org/tutorial/latest/_attachments/How_to_write_a_Vulkan_application_in_2026.pdf
[kvt26]: https://docs.vulkan.org/tutorial/latest/courses/siggraph2026_vk_tutorial/00_Overview.html
[v23]: https://vulkan.org/user/pages/09.events/vulkanised-2023/vulkanised_2023_getting_started_on_mobile_and_best_practices_for_arm_gpus.pdf
[rp]: https://documentation-service.arm.com/static/6560a5942c8b3557fee70a16
[rp26]: https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/2026_2D00_mmg_2D00_ralph_2D00_mobile_2D00_arch.pdf
[qc26]: https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/2026_2D00_mmg_2D00_rodrigo_2D00_gpu_2D00_driven.pdf.pdf

---

## 2. The finding that settles a design argument

The Khronos 2026 course recommends this baseline:

> **Vulkan 1.4** · `VK_KHR_surface`/swapchain · **`VK_EXT_descriptor_heap`** · **`VK_EXT_shader_object`** ·
> `VK_EXT_extended_dynamic_state3` · `VK_EXT_vertex_input_dynamic_state` · **`VK_KHR_unified_image_layouts`**

and then, on the same slide deck, about hardware support:

> "**Current production mobile drivers lack support for descriptor heaps and shader objects. Support
> for KHR versions of the extensions planned**"

That is the chair of the Vulkan working group, in July 2026, saying the modern Khronos baseline is a
*desktop* baseline. It validates three decisions `mg` already made — no descriptor indexing, no
descriptor-dependent indirection in the shader, and a Vulkan 1.1 core path — and it gives the
migration plan a date: when the KHR versions land on mobile, `mg` swaps its material write and its
pipeline creation for cheaper ones **without the app changing**, because neither is visible in
`mg.h`. The API's job is to be the shape that survives that swap.

`VK_KHR_unified_image_layouts` is worth naming for a different reason: it "reduces the need to manage
GPU image layouts and synchronization". `mg` already refuses to expose layouts. That extension is
evidence the refusal was correct, and it will let the backend delete code rather than add it.

---

## 3. Twelve constraints, and what each one does to `mg`

### 3.1 AFBC is free bandwidth, and one usage flag destroys it

Arm's exact requirements for a framebuffer-compression-eligible image (`VK_IMAGE_TILING_OPTIMAL`,
~2:1 lossless, read on the `texture()` path, written on the framebuffer path):

> "Must **NOT** use `VK_IMAGE_USAGE_STORAGE_BIT`" · "Must NOT use `TRANSIENT_ATTACHMENT_BIT`" ·
> "Must NOT use `FRAGMENT_SHADING_RATE_ATTACHMENT_BIT`" · "Must NOT use `CREATE_ALIAS_BIT`" ·
> "Must NOT use `CREATE_MUTABLE_FORMAT_BIT` (Midgard, Bifrost only)"

**Consequence for `mg`:** a render target is never also a storage image. This is a rule an API can
enforce instead of documenting, because it costs the app nothing to lose: fatal at
`mg_texture()` if `MG_TEX_RENDER | MG_TEX_STORAGE` is set. (An app that genuinely needs compute to
write an attachment writes to a separate storage image and copies — or uses a second pass.)

The same slide deck introduces **AFRC** (`VK_EXT_image_compression_control`): lossy *fixed-rate*
compression, RGBA8 at 16/12/8 bpp for 2:1/3:1/4:1, with **identical API constraints** to AFBC. It is
opt-in per image, which makes it a `MgTexFlags` bit and a `MgCaps` field — the app chooses a ratio,
the way it chooses a format.

### 3.2 `STORE_OP_DONT_CARE` and `STORE_OP_NONE` are different, and I had them merged

Arm's tile-memory slide, verbatim:

> "storeOp = tile memory finalize · Use `STORE_OP_DONT_CARE` **if transient** (also transient
> allocation) · Use **`STORE_OP_NONE` if read-only** (or read-only attachment)"

`mg` has one `MG_STORE_NONE` that maps to `DONT_CARE`. Those are two different promises: *this
attachment's contents are garbage and may be discarded* versus *nothing wrote to this attachment, so
there is nothing to write back, but do not treat the tile as invalidated*. On a tiler the second is
the cheap one for a **depth buffer you only test against** and for an attachment carrying content from
an earlier pass. Merging them costs the driver the information it needs.

**Consequence:** add `MG_STORE_READONLY` → `VK_ATTACHMENT_STORE_OP_NONE` where
`VK_EXT_load_store_op_none` (or Vulkan 1.3) is present, falling back to `DONT_CARE` with a log line.
`MG_STORE_NONE` keeps meaning "transient, discard".

### 3.3 Never store a multisampled attachment. Ever.

> "**NEVER** `STORE_OP_STORE` multi-sample attachments back to memory"
> "…reduces the system bandwidth impact to **just a ninth** of that used by the naive implementation"

`mg` already hard-codes `DONT_CARE` for the MSAA colour attachment and stores only the resolve. What
was missing is that `mg` *allowed* the caller to ask otherwise. The descriptor should make the wrong
thing unexpressible: `MgPassDesc.color[i].store` is ignored for a multisampled attachment, and debug
builds assert it.

### 3.4 Packed depth-stencil is one resource, not two

> "If you use a packed depth-stencil attachment, always attach **both** attachments, clear **both** on
> load, and invalidate **both** on store."
> "If you use a packed depth-stencil attachment and need to continue using one of the attachments in a
> later render pass, invalidate the other at the end of the render pass."

`mg.c` already sets `stencilLoadOp = load_of(depth_load)` and `stencilStoreOp = store_of(depth_store)`,
which is right — but only by accident of writing them together. It becomes a stated rule: **`D24S8` is
attached as a unit; there is no per-aspect API and there never will be**, because a per-aspect API is
how apps end up with a load of one aspect and a store of the other and lose the packed saving.

### 3.5 Overlap render passes, or serialise the world

Arm, on tile-based pipelining:

> "**Good**: Pipeline barriers that allow overlap — e.g. `srcStage = ALL_GRAPHICS`, `dstStage = FRAGMENT`"
> "**Bad**: Pipeline barriers that serialize the world — e.g. `srcStage = BOTTOM_OF_PIPE`,
> `dstStage = TOP_OF_PIPE`"
> "The cake timer query is a lie … render passes are disjoint" (vertex and fragment work of *different*
> passes overlap; a serialised timeline reports 1.0 + 1.0 where the real cost is 0.33 + 0.67)

Kapoulkine said the same thing from the engine side: *"vertex work for the subsequent pass can be
scheduled before the previous pass ends — this is *really* important for tilers"*.

**Consequence:** `mg`'s pass-boundary dependency must never name `TOP_OF_PIPE`/`BOTTOM_OF_PIPE`/
`ALL_COMMANDS`, and never a queue-level fence between passes. The correct shape, which `mg.c` now
has:

```c
/* pass N → N+1: only the fragment stage is ordered; vertex work of N+1 may start early */
{ .srcStageMask = COLOR_ATTACHMENT_OUTPUT, .srcAccessMask = COLOR_ATTACHMENT_WRITE,
  .dstStageMask = FRAGMENT_SHADER, .dstAccessMask = SHADER_READ }
```

This is also the argument against a frame graph that inserts a barrier per resource: on a tiler that
is exactly the "serialize the world" pattern.

### 3.6 Overdraw is the dominant mobile cost, not geometry

Ralph Potter (Samsung), *Modern Mobile GPU Architecture*, on what mobile GPUs actually render:

> "**The Dominant Workload** — Layered 2D UI — stacked, often semi-transparent panels, text, and
> widgets, not complex 3D scenes"
> "**Geometry processing matters less** — few triangles, mostly simple quads and layers to transform"
> "**Fragment shading and hidden-surface removal matter more** — many overlapping, blended layers make
> **overdraw the dominant cost**"
> "**Bursty and event-driven** — rendering fires in response to touch, scroll, and animation, not a
> continuous simulation loop"

This is the most important slide in the corpus for an API, because it says the *median* mobile frame
is not the one `mg` was designed around (opaque triangles, batchable). It is a painter's-algorithm
stack of translucent quads where **order is meaning**.

`mg`'s frame is sorted by `(pipe, material, state)`. For opaque geometry that is pure win. For a
translucent UI stack, **the sort is a correctness hazard**: two overlapping panels with different
state handles but the same blend mode get reordered, and the frame is wrong.

**Consequence — and this is the change I most needed:** a draw can declare itself **ordered**:

```c
typedef enum MgDrawFlags {
    MG_DRAW_ORDERED = 1u << 0,   /* keep submission order; never reorder against neighbours */
} MgDrawFlags;
```

Ordered draws sort into a reserved final bucket keyed on their append index, so they coalesce with
each other, never move relative to one another, and always come last — after everything that is
order-independent. One line in the key function, and the API stops being able to break the workload
that actually dominates the platform:

```c
uint32_t mg_coll_key(const MgDraw *d)
{
    if (d->flags & MG_DRAW_ORDERED) return 0xFF000000u | (d->order & 0x00FFFFFFu);
    return (d->pipe << 20) | (d->mat << 8) | (d->state & 0x0F);
}
```

And the corollary that `mg` must **not** grow a depth prepass: hidden-surface removal is already what
the tiler does, and on a tiler a prepass means store + reload of the depth buffer (§3.2, §3.3). If a
frame is overdraw-bound, the answer is fewer overlapping layers and a *subpass* for anything fused,
not a second pass.

### 3.7 Thermal: optimise against the sustainable clock, not peak

> "…first steps is typically to **lock the CPU and GPU frequencies** on a handset to levels that can
> be sustained indefinitely, and to optimize against that rather than against peak."

This is the protocol `mg`'s measurement rules already implied (and matches Qualcomm's thermal-cliff
numbers: the GPU-driven path throttles at 4.1–4.3 minutes). It means **a 30-minute run is the
benchmark**, and it means `mg_stats` needs a time series rather than a peak — the demo logs every 60
frames for that reason.

### 3.8 Pre-rotation, or the display processor pays for you

> "**Advice:** Always ensure swapchain is in a **DPU-friendly orientation**
> · Apply screen pre-rotate as a geometry transform in the application
> · Set swapchain `preTransform` to match the surface `currentTransform`
> · Recreate swapchain when the surface `currentTransform` changes"
> "Android Vulkan API capability reporting is **not useful** … SurfaceFlinger's GPU fallback means
> everything is possible … **but not everything is fast!**"

`mg` sets `preTransform = caps.currentTransform`, which is correct but not optimal: on a 90°-rotated
device the app renders in display orientation and the DPU (or a SurfaceFlinger GPU pass) rotates the
output every frame — a full-screen read+write for nothing. The DPU-friendly path is
`preTransform = IDENTITY` plus rotating the *geometry*, which the vertex shader does for free.

**Consequence:** `mg_surface()` negotiates `IDENTITY` when `supportedTransforms` allows it, and
exposes the rotation the app must apply: `uint32_t mg_display_rotation(const MgDevice *d)` in degrees.
One matrix in the frame constants. This is a mobile-only feature that desktop APIs never need and
that no wrapper layer provides, which makes it exactly the kind of thing `mg` should own.

### 3.9 UI belongs in the 3D pass

> "Avoid creating unnecessary render passes · 3D Render Pass + 1:1 Read + UI Render Pass →
> **Append to existing pass whenever possible**"

`mg`'s pass model allows this (the app just draws its UI records into the same pass with an ordered
flag from §3.6) — but nothing in the design told anyone to. It does now.

### 3.10 Clear, blit, and per-fragment clears are all round trips

> "Do not clear an attachment inside a render pass using `vkCmdClearAttachments()`" ·
> "Do not write a constant color using a shader program to manually clear" ·
> "Do not use `vkCmdBlitImage()` as a way of upscaling … an unnecessary round trip to memory"

`mg` has no such API, which is the right answer: a clear is a `loadOp`, an upscale is a draw with a
fullscreen triangle (§ the composite pass in the demo).

### 3.11 The post-transform limit is a real `DEVICE_LOST`

> "Mali typically supports up to **180 MB per render pass** … `DEVICE_LOST` if intermediate allocation
> fails … Keep the application's vertex count below approximately **2 million**."

`mg` cannot prevent this (it is a per-pass geometry budget), so it belongs in
`MOBILE_PORT_CHECKLIST.md` and in the pass-design guidance: a *single* pass holding 2M+ vertices is
not a thing on mobile, regardless of how the API is shaped.

### 3.12 Descriptor heaps and shader objects: not yet, and not on faith

From §2. `mg.c` writes materials with `vkUpdateDescriptorSets`, which is the only portable option
today. When `VK_KHR_descriptor_heap` lands, the same four descriptors become a 64-byte `memcpy` into a
mapped heap and the "material" bind becomes an offset — **`mg.h` does not change**, which is the test
of whether the abstraction was drawn in the right place.

---

## 4. What changed in the code, as a result

| # | Change | From | Risk |
|---|---|---|---|
| 1 | `MG_DRAW_ORDERED` + key change in `mg.c` | §3.6 | none — opt-in, one branch in the key |
| 2 | `MG_STORE_READONLY` → `STORE_OP_NONE` with capability check | §3.2 | low — falls back to `DONT_CARE` |
| 3 | `mg_texture()` rejects `MG_TEX_RENDER \| MG_TEX_STORAGE` | §3.1 | none — it was always a bandwidth bug |
| 4 | `mg_display_rotation()` + `preTransform = IDENTITY` when supported | §3.8 | medium — needs a device to confirm the DPU path |
| 5 | Build: `-Wl,-z,max-page-size=16384`, 16 KB-aligned `.so` | Android 15+ requirement, adjacent to §2 | none |

What did **not** change, and why:

- **The 24-byte record and the sort.** §3.6 adds an escape, it does not remove the sort.
- **4-texture materials.** §2 says descriptor heaps are not on mobile; the 4-slot material is what is
  fundable on the fleet, and it is the shape a heap would later make cheaper.
- **Fixed-function vertex fetch.** §3.6 says geometry barely matters on the median mobile frame,
  which is an *additional* argument against pulling: it optimises the part that is already cheap.
- **No depth prepass, no MDI, no bindless, no frame graph with per-resource barriers.** §3.3, §3.5,
  §3.6 and `MOBILE_TALKS.md` §4 respectively.

---

## 5. The one-slide summary

> A mobile frame is usually a stack of translucent quads on a thermally limited tiler. The API's job
> is to keep that stack's order intact, keep every attachment in tile memory, never make the DPU
> rotate the output, and spend nothing per draw — and to leave the door open for the 2026 Khronos
> baseline on the day mobile drivers actually have it.
