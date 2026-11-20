# Seb Aaltonen — HypeHype, and where it lands on `mu`

Three talks, one direction of travel, read against `MOBILE_VK_RESEARCH.md`. Written after fetching the
primary sources, not summaries of them.

| Talk | Year | What it is |
|---|---|---|
| **Modern Mobile Rendering @ HypeHype** (REAC) / **HypeHype Mobile Rendering Architecture** (SIGGRAPH, Advances in Real-Time Rendering) | 2023 | The mobile rendering **abstraction**. This is the direct prior art for `vk.h`. |
| **Moving HypeHype towards Physically Based Rendering** (SIGGRAPH, Moving Mobile Graphics) | 2024 | The renderer built on it: passes, PBR, post, and what they refused to ship. |
| **Reducing Graphics API Complexity: A Clean Slate Design for Modern GPUs** (SIGGRAPH, Moving Mobile Graphics) + the 84-minute **No Graphics API** blog | 2025–26 | The current thesis: the API itself is the problem. |

Sources: [No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api) ·
[2026 slides](https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/6763.2026_2D00_mmg_2D00_seb_2D00_gfx_2D00_api.pdf) ·
[REAC 2023 abstract](https://enginearchitecture.org/2023.htm) ·
[2024 slides](https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/3225.siggraph_5F00_mmg_5F00_2024_5F00_HypeHype.pdf) ·
[Moving Mobile Graphics index](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/moving-mobile-graphics)

---

## 1. The 2023 talk: what HypeHype's API actually is

From the REAC abstract, in their words:

> "HypeHype's new renderer has been designed from the ground up for Vulkan, Metal and TBDR mobile
> GPUs. Efficiency has been the key design principle. **Most of the repeated work traditionally done
> at draw call frequency is gone. Pipelines and bind groups are precreated and data update is fully
> separated from drawing. Our command stream carries only the metadata. Simulation writes the actual
> data directly to persistently mapped GPU pointers, bypassing all data translation costs.** This
> design allows us to hit stable 30 fps on low end Android phones in complex 10,000 draw call scenes."

and the headline: **10k+ draw calls at 60 fps on a $99 phone** (SIGGRAPH 2023).

The backend is called **Hyper**, wraps Vulkan / Metal / WebGPU, and the shader system leans on
SPIRV-Cross. Five claims, and every one of them is a decision `vk.h` has already made or should:

| Their claim | `mu` today | Verdict |
|---|---|---|
| Single-threaded-by-design renderer, built with threading in mind | `vk.c` is one command buffer, one submit, one thread | **Same.** Keep it. |
| Data update separated from drawing; the command stream carries metadata only | `ARENA_DEVICE` + offsets in push constants; `upload_*` separate from passes | **Same.** This is already the architecture. |
| Persistently mapped GPU pointers, no data translation | `buffer_host()` + staging copy still exists | **Adopt the strong form** — our §8.5 says "delete staging on UMA"; he says it from production. |
| Pipelines and bind groups precreated | `pipe_create()` + one descriptor set bound once | **Same.** Our §8.4 pre-warm is the missing half. |
| Designed for TBDR from the ground up | Already pass-shaped | **Same**, and our §7.2 subpass work is what is still missing. |

The interesting part is what he does *not* do: no multi-threaded command recording, no secondary
command buffers, no render-graph framework. The speed comes from removing work at draw-call frequency,
not from parallelising what is left.

## 2. The 2024 talk: the shape of the frame

The pass graph he showed, in order:

```
Depth (DS) → Deferred Lighting → Transparencies → Bilateral Blur → Bloom passes → Post → Upscaler → UI
```

Eight-ish passes on a TBDR phone, from the renderer with the best mobile numbers in the industry.
That calibrates §2.5 of the research: the rule is **"don't split what could have merged"**, not
"collapse the frame into one pass". Their savings came from bandwidth per pass, not from pass count.

Two more things from that talk worth stealing:

- **"Our team spent around 50% of our time optimising code."** Same doctrine as `AGENTS.md` rule 12.
- **"New features that cost too much can't be shipped. The last resort is to cut the feature,
  preferably early in the production."** That is the mobile equivalent of the budget table in §3 —
  it is a *shipping* rule, not an optimisation rule.

## 3. The 2026 talk: the thesis, and the concrete API

The claim: DX12 / Vulkan 1.0 / Metal 1 were designed for 2010–12 hardware — vendor-specific binding
models, no bindless, no coherent LLC, 256 MB CPU apertures. Modern GPUs converged: 100% SIMD, coherent
LLC, bindless samplers, 64-bit GPU pointers, UMA/ReBAR, DCC without layout transitions. Therefore the
retained-mode objects are now pure overhead, and **the PSO explosion is symptom #1**:

> "~100 GB PSO cache · 100 TB+ cloud server · driver update → 30 minutes waiting for shaders to
> recompile" (from *Borderlands 4* cache-size benchmarks on his slide).

The proposed API, from the slides:

```c
void* gpuMalloc(size_t bytes, MEMORY memory = MEMORY_DEFAULT);   // persistent CPU→GPU pointer
void  gpuFree(void *ptr);

GpuPipeline gpuCreateGraphicsPipeline(ByteSpan vertexIR, ByteSpan pixelIR, GpuRasterDesc desc);
// no root/push argument APIs, no descriptor set/table APIs, no buffer/texture binding APIs

void gpuDispatch(cb, const Data* root, uint3 group);             // root data = one 64-bit pointer
void gpuDrawIndexed(cb, const Data* root, const uint32* indices, uint32 count);

void gpuBarrier(cb, STAGE before, STAGE after, HAZARD_FLAGS hazards = 0);
void gpuSignalAfter(cb, STAGE before, void* ptr, uint64 value, SIGNAL s);
void gpuWaitBefore(cb, STAGE after, void* ptr, uint64 op_value, OP op, HAZARD_FLAGS h);
```

Seven design moves, with the evidence he gives and what it means for us:

1. **Root data is a 64-bit GPU pointer.** The shader kernel takes `const Data*`; the struct holds
   uniforms *and* pointers to in/out arrays; a 16-byte-aligned struct becomes two 128-bit loads.
   Justification: "the OS doesn't care about data layout, the graphics API shouldn't either."
   *Our position:* `vk.h` is 90 % of the way — binding 3 is the whole arena and the shader pulls
   through it. The last 10 % is the 128-byte push block, which exists only because Vulkan has no way
   to pass a pointer as a root argument without `VK_EXT_descriptor_buffer`-class machinery. See §4.
2. **No texture bindings: a heap of 256-bit descriptors indexed by `u32`**, in plain memory the CPU
   and the GPU can both write (`gpuMalloc<GpuTextureDescriptor>(65536)`), with one
   `gpuSetActiveTextureHeapPtr(cb, heap)`. He explicitly unifies the two hardware models — *index
   into a descriptor heap* (TMU caches descriptors) and *256-bit descriptors loaded to scalar
   registers* — behind one `Texture textureHeap[]` syntax.
   *Our position:* this is exactly our §7.5 tier A, and the reason tier C must still work: on Vulkan
   mobile the heap is a descriptor array, not a buffer of blobs, until `VK_EXT_descriptor_buffer`.
3. **Barriers are stage masks, not resource lists.** "GPU doesn't care about individual resources at
   all… the modern driver loops through your barrier resource list, populates a bitfield denoting
   which L1s to flush to LLC — wasted work on both sides. The fix: the user provides the cache-flush
   bitmask directly." Metal 2 already works this way.
   *Our position:* agree, and it's why §8.3 tries to make attachment barriers not exist. But on
   Tier B the *layouts* still have to come from somewhere — and they come from the render pass, which
   is the whole point of §5's "the pass is the object".
4. **Split barriers as GPU-pointer semaphores** (`gpuSignalAfter` / `gpuWaitBefore`) so independent
   work overlaps the tail. No persistent sync objects.
   *Our position:* this is a better `PassNote`-era idea than anything in the research doc — worth
   adding as an optional `pass_split_begin()/pass_split_end()` once the pass cache exists.
5. **Thin PSO.** Only `.topology`, `.alphaToCoverage`, `.supportDualSourceBlending`, `.sampleCount`,
   depth/stencil format, colour formats, optional blend state. Depth-stencil becomes a separate
   object; blend becomes a separate object; viewport/scissor/cull are always dynamic; the input
   assembler "is implicitly fully dynamic (no vertex buffers)".
   *Our position:* this is `VariantKey` minus `DrawState` — and §7/decision 11 says we cannot go all
   the way there on the fleet. His own slide agrees:
   > **"Limited dynamic state is a MASSIVE issue today. Android = Vulkan 1.1 (good coverage). No
   > dynamic render state. DirectX 12 is only slightly bit better than Vulkan 1.1."**

   That is the single most useful sentence in the talk for us: the clean-slate design assumes
   Vulkan 1.3-style dynamic state, and the platform he shipped it on does not have it. Which means
   the mobile `VariantKey` legitimately keeps `{samples, blend, write_mask}` on Tier B, exactly as
   the research doc says — and that his own production renderer lived with it for three years.
6. **Vertex pulling, no vertex buffers.** "Vertices are just memory. Dynamic vertex layout/stride
   doesn't require multiple PSOs."
   *Our position:* already done; §7.6 only adds "split positions from the rest" for the binning pass.
7. **Render passes stay.** "The render pass abstraction doesn't add notable API complexity, so it's
   an easy choice"—added to DX12 in 2018 for Gen11 and **Adreno 630**.

## 4. Where he changes our design

Only two places, and both are in the push-constant / root-data corner.

**(a) The 128-byte push block is a Vulkan wart, not a design constraint — but it is still our floor.**
His Sep 2026 thread on the newest extensions says he ended up pushing a *single* GPU pointer rather
than the whole root struct:

> "Pushing the whole root struct (using `vkPushDataEXT`) is slower than pushing a single
> `VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT` GPU-pointer and manually writing… This means
> GPU-generated root data."

So: `PUSH_M(DrawPush, const void *root; ...)` is the shape to design for, with the 128-byte inline
block as the Tier A/B fallback. It removes the "72 bytes now, 56 bytes later" anxiety in §7.1 and it
makes GPU-generated root data possible — which is what a GPU-driven renderer needs anyway. Put it
behind `F_ROOT_POINTER`, never depend on it; these are 2026 extensions with no mobile presence yet.

**(b) Read the "delete the binding layer" advice as *aspiration*, not as work for `vk.h`.** The
clean-slate API needs pointer support in the shading language; he says so himself:

> "Buffer device address is widely supported by all GPU vendors (including mobile), but is not part
> of core Vulkan 1.4. The main issue with BDA is lack of pointer support in GLSL and HLSL… a 64-bit
> integer can be cast to a struct… BDA will remain a niche until HLSL and GLSL support pointers
> natively."

`vk.h`'s job is to be the *ceiling* abstraction (`PassDesc`, handles, arenas) over a Vulkan that will
have descriptor sets and layouts for another decade. The right move is what §7.7 already says: make
the API refuse to expose the parts of Vulkan that do not map to hardware (layouts, framebuffers,
barrier resource lists), and let `MU_ESCAPE` be the valve.

## 5. What we should copy tomorrow, unchanged

1. **"Pipelines and bind groups precreated; data update fully separated from drawing."** Our §8.4
   pre-warms pipelines but still creates them lazily on first draw. Aaltonen's version: nothing is
   created at draw time, ever. Change `pipe_create()` on mobile to require an explicit
   `pipe_warm(g)` phase before the first frame.
2. **The command stream carries metadata only.** Auditable: `FrameStats.upload_bytes` should be
   near-zero during play, and `declared_bytes` is the thing that grows. If a frame's cost is in
   `upload_bytes`, the renderer is translating data instead of writing it.
3. **Discard is not free.** His (2025) list matches §2.4 exactly: it can "disable Z-compression /
   early-Z / Hi-Z" and "fuck up TBDR culling" — and branching around it does not help, "because the
   driver must be prepared for it, since it doesn't know the runtime state". The toon renderer's
   alpha-tested ink lines need this measured, per pass, not argued about.
4. **His min-spec line for the clean design** — "Apple M1/A14 (2020)… iPhone 11 still supported" —
   is the same tier boundary as our Tier A/B split, arrived at from the other direction (feature
   convergence instead of Android CDD). Two independent derivations of the same cut is a good sign
   the cut is real.

## 6. One-line summary

HypeHype's 2023 talk is what `vk.h` already is, plus the pre-warm discipline and the UMA write path;
the 2026 talk is where `vk.h` should be in five years, and its own slides admit the Android fleet
cannot run it yet. Neither of them changes the mobile plan — they raise the confidence in it, and they
give us one new idea worth building: **a root pointer behind the push block.**
