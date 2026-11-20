# `mg` — a mobile-first GPU API, designed against the hardware instead of around it

`vk.h` is a good desktop-shaped API on mobile hardware. It assumes **bindless** (a 65 536-entry
sampler array), **immediate mode** (a `PassDesc` built per pass per frame, tens of `vkCmd*` calls per
draw) and, where a renderer wants draw-call scale, **GPU-driven submission** (indirect draws, device
counters). On the hardware in the field, all three of those assumptions are losses:

| `vk.h` assumes | Mobile reality | Source |
|---|---|---|
| Bindless heaps | `VK_EXT_descriptor_indexing` is **not** in Android Baseline 2021/2022; an indexed descriptor adds a dependent descriptor→texture load that costs **3.3× text-read cycles** | Khronos profiles; Qualcomm, SIGGRAPH 2026 |
| MDI / GPU-driven submission | `maxDrawIndirectCount == 1` on **all pre-Mali-G710** hardware; on Adreno the fully GPU-driven path is **1.44× slower at 1.9× the power** than CPU culling + indexed draws | Arm; Qualcomm, SIGGRAPH 2026 |
| Many cheap API calls per draw | 840 draws in **13 ms CPU** was the *good* number on a 2017 mid-range phone — with 2 descriptor sets and dynamic offsets | Roblox, GDC 2020 |
| — | 10 000 draws at 60 fps on a **$99 phone** came from precreated pipelines/bind groups and a **metadata-only command stream** | HypeHype, SIGGRAPH 2023 |

So the mobile-optimal API is not "`vk.h` minus features". It is the opposite of the desktop trend:
**fewer things bound at once, fewer states, more draws, all of them submitted by the CPU, none of them
indirect.**

This document designs that API from scratch. It is `mg.h`, a new header that **does not include
Vulkan** — only the backend does.

---

## 0. The five rules the hardware writes for us

| # | Fact | Rule |
|---|---|---|
| 1 | The cheapest measured mobile configuration is CPU culling + indexed batched draws (121 FPS / 2.02 W vs 84 FPS / 3.84 W GPU-driven) | **The draw path is CPU-submitted indexed draws.** Indirect exists for batched chunks only. |
| 2 | `maxDrawIndirectCount == 1` on pre-G710 Mali | **MDI is never required.** `mg_draw_indirect` takes exactly one draw per call, and there is one call per *material batch*, not per object. |
| 3 | Bindless is not in the Android baseline, and an indexed descriptor costs a dependent load | **Materials are 4 fixed texture slots**, precreated, bound as a descriptor set. No unbounded arrays anywhere in the shader ABI. |
| 4 | 10k draws are cheap when each costs metadata (HypeHype) | **A draw appends 24 bytes to a flat array.** No allocation, no hash lookup, no `vk*` call at record time. |
| 5 | Per-draw cost is dominated by state changes (Roblox: "still faster to do state filtering in your code") | **Sort the frame's records by (pipe, material, state)** so the 10 000 records become ~8 pipeline binds and ~64 material binds. |

The API that follows: **the frame is a data structure, not a command stream.**

---

## 1. `vk.h` vs `mg.h`, side by side

| Concern | `vk.h` (desktop shape) | `mg.h` (mobile shape) | Why |
|---|---|---|---|
| When a pass is described | `PassDesc` built on the stack every frame, `begin_pass(&desc)` | **Once at startup**: `MgPass p = mg_pass(dev, &desc)`; per frame just `mg_pass_begin(f, p)` | The pass is the driver's autotune unit (turnip keys GMEM/SYSMEM, tile size and render mode on it). Stable ids make the tuner converge. |
| Bindings | bindless: sampled images[65k], samplers[256], storage images[16k] | **2 descriptor sets**: per-pass (uniform ring + arena) and per-material (**4 combined image samplers**, immutable) | No update-after-bind feature needed, no descriptor cache thrash, works on Vulkan 1.1. |
| Per-draw constants | 256-byte push block (fails: 128 on phones) | **8-byte push**: `{const_off, flags}`, data in the arena | 128 bytes is plenty when the push carries an *index*, not the data — and geometry offsets are not the push's job at all. |
| Drawing | `draw_indexed(pass, push, n, instances)` → `vkCmd*` immediately | `mg_draw(f, &rec)` → append 24 B; all `vkCmd*` happen at `mg_frame_end` | Lets the frame be sorted and coalesced; and keeps recording off the driver. |
| Draw order | submission order | submission order **within** (pipe, material, state); sorted otherwise | Opaque geometry is order-independent; alpha keeps order via the state bucket. |
| Indirect | `draw_indirect(..., max_draws, stride)` — MDI first-class | `mg_draw_indirect(f, batch)` — **one draw per call**, one call per material batch | MDI is illegal pre-G710. |
| Pipeline creation | on first use, cached in a file | **explicit `mg_warm()` before frame 1**, plus the file cache | Nothing may be created at draw time. |
| Vertex data | **pulled** from the arena: the vertex shader indexes a storage buffer | **fixed-function fetch** from two bound arenas, per draw `firstIndex` + `vertexOffset`; separate position stream | §5.5: the fetch block converts formats, prefetches and aligns to cache lines for free, and both vendors run the vertex shader twice per frame. |
| Staging | `ARENA_STAGING` ring + copies | **one UMA heap, written directly** | All Android memory is host-visible; a staging copy is a memcpy for nothing. |
| Sync | declared `ImageUse`/`BufferUse` → barriers per pass | **carried by the pass** + one fence per frame | Barriers between passes on the same attachment are store/reload of the whole framebuffer. |
| Compute | `PASS_COMPUTE` anywhere in the frame | only at **frame boundaries** (before/after all render passes) | A dispatch mid-frame flushes tile memory. |
| Header includes | `<vulkan/vulkan_core.h>` | **nothing** | The app never sees a `Vk*` type. Boundaries are `MgDevice`/`MgFrame`/ids. |

The philosophical difference: `vk.h` exposes *what Vulkan lets you say*, mediated by declarations.
`mg.h` exposes *what a mobile GPU can do cheaply*, and refuses the rest.

---

## 2. The record, and why 24 bytes

The application hands in handles; the collector stores indices. That split is where the 24 bytes
come from — four 8-byte handles would be 32 bytes of nothing before a single payload field:

```c
typedef struct MgDrawDesc {      /* what the app passes to mg_draw()          */
    MgPipe pipe; MgMat mat; MgMesh mesh; MgState state;
    uint32_t params;             /* the slot mg_const() returned              */
    uint16_t instances;
} MgDrawDesc;

typedef struct MgDraw {          /* what the frame stores; 24 bytes exactly    */
    uint32_t pipe;               /* 12 bits                                    */
    uint32_t mat;                /* 12 bits                                    */
    uint32_t mesh;               /* 12 bits                                    */
    uint32_t params;             /* offset into the constant ring              */
    uint16_t instances;
    uint16_t state;              /* state index; its low byte is the bucket    */
    uint32_t order;              /* append index: makes the sort stable        */
} MgDraw;
```

The sort key is derived, not stored:

```
key32 = (pipe & 0xFFF) << 20 | (mat & 0xFFF) << 8 | (state & 0xF)
```

`order` is not in the key. A **stable** LSD radix sort over `key32` therefore keeps submission order
inside each (pipe, mat, state) run, which is exactly the guarantee blending needs. Four passes of
8 bits, no comparison function, no branch, no allocation — one 8 KB count table and a scratch array,
and after four passes the data is back in the source array so there is no final copy.

### What the sort buys, measured

`mg.c` in this directory is the whole implementation: the collector, the sort and the
coalescer first (part 1), the Vulkan backend second (part 2), and a test in part 1. 10 000 draws
in an engine's natural traversal order (uniformly mixed 8 pipes × 64 materials × 4 state buckets —
i.e. no grouping at all):

```
             pipe binds   material binds   push changes   records
unsorted           8739           9861          10000     10000
sorted                8            512          10000     10000
```

The sort collapses **8 739 pipeline binds into 8**. Material binds drop to 512, which is not a
disappointment but the arithmetic working: 8 pipes × 64 materials means 512 (pipe, material) pairs,
and the key specifies both, so every pair is visited exactly once. Each bind is then amortised over
~20 draws.

That is rule 5 doing its work. After the sort, the entire *state* cost of a 10 000-draw frame is
8 + 512 API calls, and the per-draw cost is 8 B of push plus one `vkCmdDrawIndexed`. No bindless, no
MDI, no GPU-side submission, no per-draw descriptor set.

### Cost per draw

| | ns/draw |
|---|---|
| Append a 24-byte record (one bounds check, one store, one increment) | ~2–4 |
| Radix sort, 4 passes over 24 B records | **15.2 measured** on a 2.6 GHz Xeon VM; budget 40–60 on a Cortex-A55 |
| Push 8 B + `vkCmdDrawIndexed` at submit | ~30–60 (driver, mobile) |
| **Total** | **~50–100 ns/draw → 10 000 draws ≈ 0.5–1.0 ms of one core** |

Roblox measured 13 ms CPU for 840 draws in 2019 (≈15 µs/draw) — but that was a GLES-era immediate-mode
renderer doing per-draw descriptor work against a driver with no draw-list discipline. 15 µs → 60 ns is
the 250× the last decade of mobile CPU and API design bought, and HypeHype's 10 000 draws at 60 fps on
a $99 phone is the existence proof that the metadata path lands there.

One caveat that matters for a phone: the sort is 4 passes of *memory bandwidth* over 240 KB. On a
mid-range core that is the dominant term, and it is why the record is 24 bytes and not 48. If it ever
becomes the bottleneck, the fix is to sort *indices* (4 B) and permute once, at the cost of a second
pass — not to move the draw path to the GPU.

---

## 3. Materials instead of bindless

A material is a **fixed, precreated descriptor set**:

```c
typedef struct MgMatDesc {
    MgTex tex[4];        /* albedo, normal, pbr, extra. MG_TEX_NONE is legal  */
    uint8_t slot_count;   /* <= 4, always                   */
    uint8_t samplers[4];  /* indices into the sampler table */
    uint8_t flags;        /* alpha test, two-sided, ...     */
} MgMatDesc;
```

Backend: one `VkDescriptorSetLayout` with `binding = 0, descriptorCount = 4,
descriptorType = COMBINED_IMAGE_SAMPLER`, and **immutable samplers** in the layout — so the sampler is
baked into the descriptor at layout creation, never a separate binding, never a separate register in
the shader.

Why this beats a bindless heap on this hardware, concretely:

1. **No feature gate.** No `descriptorBindingPartiallyBound`, no `shaderSampledImageArrayNonUniformIndexing`,
   no update-after-bind. It is Vulkan 1.0. It runs on Android Baseline 2021/2022 devices.
2. **No dependent load in the shader.** Bindless is `textureHeap[data->i]` — a descriptor load, then a
   texture read. Qualcomm's own instruction counts show the indirect path at **40.5 cycles of text
   read vs 12.1** for the indexed path. Four *statically bound* samplers cost the texture unit nothing
   extra.
3. **Descriptor cache friendly.** The driver binds a **32-byte** descriptor set (4 × 64-bit-ish
   entries) from a table it wrote itself, not a 4 MB heap it has to make resident.
4. **Bounded and known.** `mg` can validate a material at creation: 4 slots, known formats, known
   filter. Nothing to clamp at runtime, nothing to fall back on.

The trade is real and must be stated: **a draw cannot exceed 4 distinct textures.** Consequences that
must be designed for, not discovered:

| Need | `mg` answer |
|---|---|
| Terrain with 8 layers | one material per 4-layer group; or a texture **array** as slot 0 and index it in the shader (an array is one descriptor, and `sampler2DArray` indexing is free on all mobile hw) |
| UI with an atlas | one material, one atlas — this is what every mobile UI does anyway |
| Many small objects, many materials | the sort groups them: 64 material binds for 10 000 draws |
| Genuinely open-world, unbounded | an atlas/array + a residency system, i.e. virtual texturing — which is a *physical memory* problem, not an API one |

---

## 4. Passes as a compiled-in table

Passes are created at startup, and `mg` treats the set as closed:

```c
static const MgPassDesc PASSES[] = {
    { .name = "shadow",   .depth = MG_D16,  .w = 2048, .h = 2048, .samples = 1 },
    { .name = "scene",    .color = { MG_R11G11B10, MG_R8G8, MG_RGBA8 }, .depth = MG_D24S8,
                          .samples = 4, .resolve = true, .depth_store = MG_STORE_NONE },
    { .name = "composite",.color = { MG_SWAPCHAIN }, .load = MG_DONT_CARE, .depth = MG_FMT_NONE },
};
```

Everything `vk.h`'s `PassDesc` says about attachments survives here — load/store, resolve, samples,
transient — because that part of `vk.h` was already right (it is the one thing the desktop API said
that the tiler agrees with). What changes is **lifetime and stability**:

- Created once, never re-described. The backend's `MgPass` → `VkRenderPass`/`VkFramebuffer` table is a
  hash lookup with no misses after frame 1, so turnip's `rp_key` autotuner (`tu_autotune.cc:822`)
  sees the same pass every frame and converges instead of exploring.
- `mg_pass_begin(f, pass)` takes an id. There is no per-frame attachment list to get wrong.
- The frame's pass order is the order of `mg_pass_begin` calls, and `mg` records a warning (debug) if
  a compute pass is opened between two render passes.
- Defaults are the cheap ones: `load = MG_DONT_CARE` if the pass fully covers, `store = MG_STORE_NONE`
  for depth and any transient attachment, MSAA resolve **in the pass** (Roblox/Arm: never
  `vkCmdResolveImage`).
- **Subpasses**: `MgPassDesc.subpass[]` up to 4, `mg_read_input()` gives the shader a
  `subpassInput` — the one place `mg` deliberately copies `vk.h`'s research, because it is worth
  3.7 GB/s and the API cannot express it otherwise.

---

## 5. Per-draw constants: 8 bytes of push, data in the arena

```c
/* A 16-byte slot in the per-frame constant ring. Bump-allocated, cache-line packed. */
uint32_t mg_const(MgFrame *f, const void *data, uint32_t bytes);

/* Per draw: where the parameters are, and what the material flags say. */
PUSH_MG(MgPush,
    uint32_t const_off;    /* arena: the 16-byte-slotted params          */
    uint32_t flags;        /* material flags, alpha ref, bucket          */
);
```

Two words. Geometry offsets used to be here and are not any more: `firstIndex` and `vertexOffset`
are draw-command arguments, and the hardware's vertex fetch adds them itself (§5.5). What is left is
the only thing the shader cannot derive — which param slot to read, and which flags apply.

Three reasons 8 bytes beats every alternative on mobile:

- **vs a 256-byte push** (`vk.h` today): it does not fit. `maxPushConstantsSize` is 128.
- **vs dynamic uniform buffer offsets** (Roblox's recommendation): both work; a push is cheaper
  because it is a direct write into the command stream and never touches the descriptor cache. Roblox
  needed dynamic offsets because their constants were *buffers*; ours are a ring the app writes into a
  persistently mapped arena, so the push can carry the offset.
- **vs a bindless root pointer** (Aaltonen's 2026 design): better, but it needs a pointer type in the
  shader language that GLSL/HLSL do not have, and `VK_EXT_descriptor_buffer`-class extensions the
  Android fleet does not ship. At 8 bytes there is room to grow: the root-pointer variant replaces
  these two words with one `uint64_t`.

The constant ring is `1024 slots × 16 B = 16 KB` per frame in flight, aligned to 16 and padded to the
cache line so two draws never share one. At 10 000 draws that is 160 KB/frame of arena — nothing.

---

## 5.5 Vertex fetch — the case *against* vertex pulling

`vk.h`'s own draws "launch N VS invocations pulling from b3" — attribute data is read out of the
device arena with `gl_VertexIndex` arithmetic, and no vertex buffer is ever bound. It is a coherent
design and it is what the first draft of `mg` did too. On mobile it is the wrong default, for four
reasons that come out of the driver source rather than out of taste.

### The fetch unit is hardware you have already paid for

A vertex fetch block sits in front of the shader core. It streams attributes sequentially (so it
prefetches), converts formats (`R16G16B16A16_SNORM`, `A2B10G10R10`, `R11G11B10F`) into floats without
spending an ALU instruction, applies the stride and the base offset, and delivers them as shader
inputs. Pulling replaces all of that with generic buffer loads: address arithmetic per attribute per
vertex, an unpack per component, and a load that the LSU schedules — competing with the attribute
streams and the constant loads for the same issue slots.

The evidence that this is distinct silicon, and that mobile cares: Qualcomm's kernel interface
enumerates a **`VFD` performance-counter group and, separately, a `BV_VFD` group** — vertex fetch
decode in the binning pass and in the render pass, each with its own counters
(`msm_kgsl.h:459` and `:502` in Mesa). Adreno has two of these blocks running per frame.

### Both vendors run the vertex shader more than once per frame

This is the part that decides it. Whatever the vertex shader costs, it is paid twice — and pulling
makes the vertex shader bigger.

- **Adreno**: a binning pass runs vertex processing to build the visibility stream, then the render
  pass runs it again. That is why the hardware duplicates the counters above (`BV_PC`, `BV_VPC`,
  `BV_TP`, `BV_SP`, `BV_VFD`), and why turnip has a whole separate binning-pass command stream
  (`tu6_emit_binning_pass`, `tu_cmd_buffer.cc:2681`) that emits vertex state and no fragment work.
- **Mali**: IDVS splits the vertex shader in two. The tiler runs a *position-only* variant, and the
  varying variant runs per tile. panvk even knows the position results go through memory: *"Index-
  Driven Vertex Shading requires different instances to have different cache lines for position
  results. Each vertex position is 16 bytes and the Mali cache line is 64 bytes, so the instance
  count must be aligned to 4 vertices"* (`jm/panvk_vX_cmd_draw.c:1722`).

So positions are materialised at 16 B/vertex and re-read at 64 B cache-line granularity *before* any
attribute is considered. Adding a pull on top of that means the tiler's position-only shader now does
generic address arithmetic and a cache-line-wide load to obtain the 12 bytes it wanted — and the
varying shader does it again for normal and UV.

### Bandwidth: the objection is real, but splittable

Take a 32-byte vertex (position `R32G32B32` + packed normal + `R16G16` UV), and count what actually
crosses the memory system per vertex, per pass:

| Path | Tiler pass | Varying pass | Total | What you also pay |
|---|---|---|---|---|
| Fixed-function fetch, split streams | **12 B** | **~21 B** | **33 B** | nothing |
| Pulling a 32 B interleaved vertex | 32 B (a 64 B line per 2 vertices, 12 B useful) | 32 B | **64 B** | 2× bandwidth, plus per-vertex address math and unpacking |
| Pulling a split position stream | ~12 B (a line amortised across the lanes of a wave) | ~21 B | **~33 B** | no bandwidth objection at all — but the address math, the unpack and the load latency stay, and they are paid in *both* passes |

So the honest version of the bandwidth argument is narrower than the folklore: **splitting the streams
removes it**, by exactly the same mechanism that makes the fixed-function path cheap. What splitting
does not remove is the other half — the fetch unit converts `R16G16B16A16_SNORM` or `A2B10G10R10` to
floats in hardware, issues one load for a whole attribute, and prefetches a stream it knows is
sequential; a pull does all of that with ALU instructions and dependent loads in a vertex shader that
both vendors run twice. That is the cost that does not go away, and it is why the default stays
fixed-function.

### The CPU-side win pulling claims is available without pulling

The real motivation for pulling is "no vertex buffer binds, no format registry, one big arena". All of
that survives with fixed-function fetch, because Vulkan puts the base offset in the *draw command*:

```c
/* One vertex arena, one index arena, bound once per frame. Nothing per draw. */
vkCmdBindVertexBuffers(cmd, 0, 2, arena_vb, offsets);   /* once */
vkCmdBindIndexBuffer(cmd, index_arena, 0, VK_INDEX_TYPE_UINT16);  /* once */

/* Per draw: local indices, base vertex. Zero binds, zero descriptors. */
vkCmdDrawIndexed(cmd, mesh->index_count, inst, mesh->first_index, mesh->vertex_offset, 0);
```

`vertexOffset` is an `int32_t` added by the fetch unit to every index, so an object stores **local**
16-bit indices and the arena is addressed by the draw command. That is the same "one arena, no binds"
ergonomics as pulling, with the fetch hardware intact — and it lets the push shrink from 16 bytes to
8, because the geometry offsets no longer need to travel through the shader.

Roblox shipped the best published mobile numbers (840 draws, 13 ms CPU, GLES 38 ms) while keeping a
**`GeometryVulkan` struct that is "similar to OpenGL VAO — a struct with all geometry state"**. They
optimised the CPU side and kept vertex buffers. The talk never proposes pulling.

### So when *is* pulling right?

| Case | Verdict |
|---|---|
| Mesh the application already has as an array | **No.** Two arenas + `vertexOffset`. |
| Instanced quads, particles, full-screen triangles | **Neither** — generate from `gl_VertexIndex`. Zero bytes fetched is unbeatable, and this is the one place the technique people call "pulling" is genuinely best. |
| Geometry produced on the GPU (compaction, cloth, marching cubes) | **Yes** — the CPU never knows the offsets, and no fetch descriptor can describe it. |
| Skinned / morph meshes where the *positions* come from a compute pre-pass | **Yes**, for positions; attributes still by fetch. |
| >16 attribute streams, or a format the fetch unit cannot express | **Yes**, by necessity. |

Hence `MgPipeDesc.pull`, defaulting to false, documented as an escape with the reasoning attached.

### How to settle it on a device instead of in a document

Same scene, same draw count, `pull` on and off, and read the counters that already exist:

- **Adreno**: `KGSL_PERFCOUNTER_GROUP_VFD` and `BV_VFD` (binning and render fetch decode), plus
  `TP`/`SP` busy and the `CP`/`RBBM` stall counters. If pulling is winning, `VFD` work falls and `SP`
  work rises by less; if it is losing, `SP` and the memory counters rise on both passes.
- **Mali**: Streamline's *External Read Bytes* and the *Vertex Fetch* / *Tiler* counters. The binning
  pass's external read bytes are the number to watch — that is where the 5.3× lives.
- **Both**: total GPU time for a vertex-bound scene (2048² shadow pass, 500k vertices, no fragment
  work), and the same for a fragment-bound scene to confirm nothing regressed.

Exit criterion, written down so it cannot be argued with later: **pulling must beat fixed-function
fetch by more than 5 % of GPU time on a real device, on a vertex-bound scene, or it does not go in.**

---

## 6. Pipes and state

`MgPipe` is the **thin** half — the part the hardware genuinely bakes:

```c
typedef struct MgPipeDesc {
    MgShader vs, fs;
    MgFmt    color[4]; uint8_t color_count;
    MgFmt    depth;            /* MG_FMT_NONE = no depth attachment */
    uint8_t  samples;
    uint8_t  topology;
    bool     alpha_to_coverage;
    bool     dual_source;      /* reserved: mobile rarely wants it  */
} MgPipeDesc;
```

and the **thick** half travels in the record's `bucket` — blend, depth test/write, cull, write mask —
as a `MgState` index into a **closed table of ≤ 16 states declared at startup**:

```c
mg_state(dev, (MgStateDesc){ .blend = MG_BLEND_ALPHA, .depth = MG_DEPTH_GREATER_WRITE, .cull = MG_CULL_BACK });
```

`bucket` is in the sort key, so state changes are coalesced with pipeline changes, and on Tier A
(everything dynamic) applying a state is a handful of `vkCmdSet*`; on Tier B it selects a pre-baked
pipeline variant — **baked ahead of time, at `mg_warm()`**, so there is no stutter either way.

This is deliberately Aaltonen's "move as much state out of the PSO as possible" *and* Roblox's
"still faster to do state filtering in your code", reconciled by the fact that the state is a
**small closed set** rather than an open-ended descriptor.

---

## 7. Memory

```
mg_malloc(bytes, align, MG_MEM_ANY)   ->  a CPU-writable pointer that is also a GPU address
mg_free(ptr)
```

One heap per memory type, created at startup, mapped once, bump-allocated by `MG_MEM_FRAME` (rewound
at `mg_frame`) and `MG_MEM_STATIC` (never rewound). Two rules the API enforces because the hardware
punishes the alternative:

1. **Never read back through a mapped pointer.** `MG_MEM_FRAME` is write-combining. `mg` returns a
   separate CPU-side `MgArena` bookkeeping struct; the mapping only ever gets stores.
2. **Flush once per frame**, over the union range of what was written, using `nonCoherentAtomSize`
   alignment. Not per allocation (Roblox, Arm: avoid per-mapping `vkFlushMappedMemoryRanges` storms).

Textures are uploaded at frame boundaries only (`mg_tex_upload` records a copy into the frame's
command buffer); nothing is uploaded mid-pass.

---

## 8. Synchronisation

- Attachment dependencies: **carried by the pass**. `mg` never emits an attachment barrier; the
  `VkRenderPass` subpass dependencies (Tier B) or the `VkRenderingAttachmentInfo` layouts (Tier A)
  express them.
- Arena dependencies: one barrier **per frame**, before the first pass, because a compute stage that
  wrote instance data at the end of last frame is done by then.
- Retirement: one fence per frame slot; everything destroyed is appended to that slot's free list.
  No timeline semaphore required (Roblox: "one completion fence per frame"), so Vulkan 1.1 devices
  work.
- `mg` does not expose a barrier API. If an application genuinely needs one it uses
  `MG_ESCAPE` and passes a raw command buffer — the same valve `vk.h` has.

---

## 9. What `mg` refuses, and why each refusal is a feature

| Refused | Because |
|---|---|
| Bindless / unbounded descriptor arrays | Not in the Android baseline; costs a dependent load (3.3× text-read cycles); unbounded residency is a memory problem in disguise. |
| MDI as a first-class path | `maxDrawIndirectCount == 1` pre-G710. |
| GPU-driven culling as the default | 1.44× slower and 1.9× the power on the one vendor that measured it, plus a thermal cliff at 4 minutes. |
| Barriers, layouts, image state | The GPU does not have them; the render pass already implies them. |
| Pipeline creation at draw time | Stutter. `mg_warm()` exists. |
| Compute inside a frame's render passes | It flushes tile memory. |
| More than 4 textures per draw | Tile-era descriptor caches and a 4-slot binding model are the cheap path. |
| A descriptor-set API | A material *is* a descriptor set. Exposing sets is exposing the driver's internals. |

---

## 10. Where GPU-driven does fit (the escape, not the architecture)

The right mobile GPU-driven shape, given the constraints, is **coarse**:

```
frame:
  compute:  cull + compact into N batch lists (N = nr of (pipe, material) pairs, ~8..64)
  barrier (arena)
  for each batch: mg_draw_indirect(f, {pipe, mat, arg_buffer_slice, count = 1})
```

N single-draw indirect calls, each with a device-side count, is exactly what `maxDrawIndirectCount == 1`
allows and what Adreno accelerates. Per-object MDI, `gl_DrawID` indexing and mesh shaders are all
available as experiments behind `MG_GPU_DRIVEN`, and Qulcomm's table says why they should not be the
default: 84 FPS at 3.84 W with a thermal hit, vs 121 FPS at 2.02 W without.

The honest summary of the vendor data: **GPU-driven moves work from the CPU to the GPU, and on a
phone the GPU is the thing you cannot afford to keep busy.** Only move work there when the CPU is
provably the bottleneck.

---

## 11. What this costs us, honestly

| Loss | Consequence | Mitigation |
|---|---|---|
| 4 textures per draw | open-world materials need arrays/atlases/VT | physically correct anyway on mobile; `MG_TEX` may be a `sampler2DArray` |
| No MDI | object count × CPU submit cost | the sort makes it ~50–90 ns/draw; 10k is the proven ceiling (HypeHype) |
| Closed material/pass/state sets | less dynamic than desktop engines | startup declaration is what makes pre-warm and autotune possible |
| CPU culling | main-thread frame cost | one job on a worker at `frame_end - 1`; the collector is pure data, trivially parallel over pass ranges |
| `mg.h` is new | everything is rewritten | the *backend* is smaller than `vk.c`: no descriptor indexing, no MDI, no dynamic-state zoo |

---

## 12. Building it, in order

| Stage | Deliverable | Exit gate |
|---|---|---|
| **0** | `mg.h` + `mg.c` + the test (this directory) | **done**: sort and coalescing numbers reproduce, `mg.h` compiles standalone with no Vulkan |
| **1** | `mg.c`: device, one UMA heap, pass table → `VkRenderPass` cache, material → descriptor set, thin pipes, 8-byte push, two bound arenas + `firstIndex`/`vertexOffset` | triangle + 1000 textured quads; 0 descriptor writes and 0 vertex-buffer binds after frame 1 |
| **2** | frame collector wired to the real backend: sort → record → submit; one fence; one submit | `MgStats`: pipe binds = 8, material binds = number of (pipe, material) pairs, pushes = draws, barriers = 1 |
| **3** | attachment model: transient attachments, in-pass resolve, subpass/input for the composite | the §3 ledger of the research doc, met at 2400×1080 |
| **4** | Android platform: surface lifecycle, pre-rotation, `AChoreographer`, pipeline cache in the code cache dir, `mg_warm` | 60 fps on a mid-range 2021 phone, thermal-stable 30 min |
| **5** | the escapes: coarse indirect (`MG_GPU_DRIVEN`), descriptor buffer experiment, split barriers | measured against the indexed path, never merged without a win |

### The test, as it runs

```
$ cc -std=c11 -O2 -Wall -Wextra -Werror -DMG_TEST -o mg_test mg.c && ./mg_test
10000 draws, 8 pipes, 64 materials, 4 state buckets

             pipe binds material binds   push changes   records
unsorted           8739           9861          10000     10000
sorted                8            512          10000     10000

radix sort (4 x 8 bit, stable, 24 B records)
  first call (cold pages):    217.6 us
  best of 20:                 142.9 us  (14.3 ns/draw)

command stream: 10000 draws x 12 B = 117.2 KB (12.0 B/draw, 50.0% of the records themselves)

key monotone: ok   stable within a bucket: ok   records intact: ok
```

Measured on a 2-vCPU Xeon @ 2.6 GHz (`-O2`, gcc). Clean under `-fsanitize=address,undefined`.
`unsorted` is *submission order* — the order an engine's scene traversal naturally produces, not a
worst case contrived to flatter the sort. The checks that matter are the last line: the key is
monotone, equal keys stay in submission order (so alpha blending survives), and every record arrives
with its mesh, material and constants intact.

---

## 13. What the 2026 corpus changed here

`MOBILE_API_RESEARCH_2.md` reads the SIGGRAPH 2026 Khronos course, Arm's render-pass and
AFBC material, and Samsung's *Modern Mobile GPU Architecture*, and turns them into five
concrete deltas. Three are in the code as of this revision:

- **`MG_DRAW_ORDERED`** — a draw can opt out of the sort. The median mobile frame is a stack
  of translucent layers where order is meaning (Samsung), so a sort key over state is a
  correctness bug for that frame, not a speed-up. Ordered draws coalesce and never move.
- **`MG_STORE_READONLY`** — `STORE_OP_NONE` is not `DONT_CARE` (Arm). "Nothing wrote here" and
  "this tile is garbage" are different promises, and only the first is available for a depth
  buffer you merely test against.
- **`MG_TEX_RENDER | MG_TEX_STORAGE` is now fatal** — either flag alone is fine; together they
  cost AFBC on the whole surface (Arm lists the exact forbidden usage bits).

Two more are structural and are why the design does *not* move:

- Khronos' 2026 baseline is Vulkan 1.4 + descriptor heaps + shader objects, and the Vulkan WG
  chair states in the same course that **"current production mobile drivers lack support for
  descriptor heaps and shader objects"**. `mg` is a mobile API. It stays on Vulkan 1.1 core and
  swaps its material write for a heap memcpy on the day KHR lands — `mg.h` does not change,
  which is the test of whether §5's boundary was drawn correctly.
- `VK_KHR_unified_image_layouts` ("reduces the need to manage GPU image layouts") is evidence
  that refusing to expose layouts (§4) was right, and it will delete backend code, not add any.

