# The mobile rendering talks — the numbers that decide the API

Two mobile renderers, one vendor study, one hardware constraint. Everything below is quoted or
derived from the primary source; every number is one a design decision can hang on.

---

## 1. Arseny Kapoulkine (Roblox) — *Optimizing Roblox: Vulkan Best Practices for Mobile Developers*
GDC 2020 (with Arm) · also *Getting Faster and Leaner on Mobile* (Reboot Develop Red 2019, Khronos)
and *Rendering Roblox: Vulkan Optimisations on Imagination PowerVR* (GDC 2020).
Slides: [GDC 2020](https://zeux.io/data/gdc2020_arm.pdf) · [2019](https://zeux.io/data/reboot2019.pdf) ·
[Khronos 2019](https://www.khronos.org/assets/uploads/developers/presentations/Getting-Faster-and-Leaner-on-Mobile-Optimizing-Roblox-with-Vulkan_Oct19.pdf)

The framing slide is the thesis: **"It's hard to beat the driver"** — and the sub-bullet is
**"We did *not* rewrite the renderer to be 'Vulkan-friendly'"**. They kept a *"clean and easy to use
immediate-mode abstraction"* that targets D3D9/D3D11/GL/GLES/Metal/Vulkan directly with no wrapper
layer, and made the Vulkan backend fast on its own terms.

The measured result, end-to-end frame, real content, **840 draw calls, single core, 2.4 GHz
Cortex-A73 + Mali-G72**:

| | CPU time |
|---|---|
| GLES | **38 ms** |
| Vulkan | **13 ms** |

and **2–3× CPU performance gains across all vendors**, "despite not being 100% Vulkan-friendly".

How they got there — the five decisions:

1. **"We use at most 2 descriptor sets!"** with the note that *"the guaranteed limit is 4 — fitting
   into the limit is a problem for many Direct3D1x ports"* and *"some mobile hardware only supports a
   single combined set in hardware anyway"*.
2. **"All `bindResource` calls just update dirty masks and resource info"**; descriptor sets are
   allocated/updated lazily before the draw. **"Still faster to do state filtering (`vkCmdBind*`) in
   your code."**
3. **"Important: use dynamic buffer offsets!"** — per-draw constants are sub-allocated by a bump
   pointer from a **512 KB uniform buffer** (`bindBufferData`), with more buffers if a frame needs
   more. Not push constants, not bindless.
4. **"Aggressively eliminate cache misses. Reduce allocations and indirections in your abstraction."**
   Their `GeometryVulkan` is "similar to OpenGL VAO — a struct with all geometry state", i.e. one POD
   fetch per draw, not five API calls.
5. **"Don't call `vk*` functions unless you need to"**; call everything through `vkGetDeviceProcAddr`
   (that is where `volk` came from). One completion fence per frame, deferred resource deletion.

And from the Arm half of the same talk, on the GPU side: `LOAD_OP_CLEAR` **−36 % external read
bytes**, `STORE_OP_DONT_CARE` **−62 % external write bytes**, merged subpasses **−45 % reads /
−56 % writes**, and MSAA using `pResolveAttachments` in the pass is nearly free while
`vkCmdResolveImage` makes bandwidth "spike".

**Take-away:** a 840-draw frame beat GLES by 3× on a 2017 mid-range Mali phone with *immediate-mode
recording, 2 descriptor sets, dynamic offsets and zero GPU-driven anything*. Not one of the
GPU-driven techniques appears in that talk.

---

## 2. Sebastian Aaltonen (HypeHype) — the mobile renderer that ships
*Modern Mobile Rendering @ HypeHype* (REAC 2023) · *HypeHype Mobile Rendering Architecture*
(SIGGRAPH 2023, Advances in Real-Time Rendering) · *Moving HypeHype towards PBR* (SIGGRAPH 2024).

The headline: **10,000+ draw calls at 60 fps on a $99 phone** (and stable 30 fps on low-end Android).

The mechanism, in the talk abstract's own words:

> "Most of the repeated work traditionally done at draw call frequency is gone. **Pipelines and bind
> groups are precreated** and **data update is fully separated from drawing**. Our **command stream
> carries only the metadata**. Simulation writes the actual data directly to **persistently mapped GPU
> pointers**, bypassing all data translation costs."

Note the words: *pipelines* (plural, precreated), **bind groups** — not a bindless heap — and
*metadata only* in the command stream. 10k draws are affordable because each draw is a handful of
bytes and nothing per-draw allocates, compiles, or is looked up in a hash map.

The 2024 follow-up frame, for calibration: `Depth → Deferred Lighting → Transparencies → Bilateral
Blur → Bloom → Post → Upscaler → UI` — 8-ish passes, and **"50 % of our time optimising"**, with
*"new features that cost too much can't be shipped"*.

**Take-away:** 10k CPU-submitted draws per frame is a solved problem on mobile *when the per-draw cost
is metadata*. GPU-driven is not the only way to get draw-call scale.

---

## 3. Qualcomm (R. Holztrattner, Snapdragon Game Studios) — *State of GPU-Driven Rendering on Mobile*
SIGGRAPH 2026, Moving Mobile Graphics.
[Slides](https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/2026_2D00_mmg_2D00_rodrigo_2D00_gpu_2D00_driven.pdf.pdf)

A stress test (multi-LOD, 32 material groups, GPU ball simulation) run four ways on the same Adreno
device. This is the most useful table in mobile rendering right now, because it is a vendor measuring
*its own* GPU-driven feature set against the boring path:

| Configuration | FPS | Power | GPU usage / clock | Notes |
|---|---|---|---|---|
| Full CPU (CPU sim + CPU frustum + batched draw) | 14.6 | 5.59 W | 82.3 % @ 815 MHz | "obviously inefficient" |
| **GPU Indexed** — sim on GPU, **CPU frustum cull**, batched draw, standard rendering | **121.4** | **2.02 W** | 82.8 % @ 656 MHz | **"best in slot"**, stable |
| GPU Indirect — sim on GPU, **GPU frustum + HZB**, batched draw | 84.4 | 3.84 W | 98.7 % @ 1044 MHz | **thermal hit at ~4.3 min** |
| GPU Mesh Shaders — same culling | 75.1 | 3.77 W | 98.7 % @ 1012 MHz | **thermal hit at ~4.1 min** |

And the shader-level reason, from the Adreno offline compiler, comparing the *indexed* shader path to
the *indirect* one:

| | Total instructions | Mem write | **Text read** | **Cycles text** |
|---|---|---|---|---|
| Indexed | 1215 | 16 | 2 | **12.1** |
| Indirect | 1479 | 42 | 10 | **40.5** |

**3.3× the text-read cycles** for the indirect path: the extra indirection (draw data → descriptor →
texture) costs real cycles in every invocation, not just in the command processor.

The paper's own framing is also worth quoting: *"The question is not whether an engine is GPU-driven,
but how GPU-driven it is"*, *"mobile GPU-driven must respect tilers, bandwidth, thermal limits, sync
and API portability — it is not the desktop problem adopted downward"*, and mesh shaders: **"power
cost is still very high even on uniform scenes."**

**Take-away:** on Adreno, the *cheapest* configuration in a GPU-driven-tuned scene is CPU culling +
indexed batched draws: **1.44× the frame rate at 1.9× less power** than the fully GPU-driven path, and
it does not thermally throttle. On a phone, power *is* the frame rate 4 minutes later.

---

## 4. Arm — the hardware constraint that ends the argument for MDI

From Peter Harris (Arm) on the Arm developer forum:

> "Only the latest **Mali-G710** and friends support multi-draw indirect, so shipping Mali GPUs will
> only support **single** indirect draws. … We have support for `DrawIndirect`, but **`drawCount` must
> be a maximum of 1 on all pre-Mali-G710 family hardware**."

Aaltonen's own history of the hardware agrees: *"**ARM Mali-G710 (2021) is ARM's first modern
architecture.** It introduced their new command stream frontend (CSF), reducing the CPU dependency of
draw call building and **adding crucial features like multi-draw indirect** and compute queues."*

So on the installed Mali fleet, `VkPhysicalDeviceLimits::maxDrawIndirectCount == 1`. A second indirect
draw in one call is not slow — it is **invalid**. Any architecture whose *primary* draw path is MDI
cannot run on the majority of Android devices.

---

## 5. Synthesis: five facts, five API rules

| Fact | Source | Rule for the API |
|---|---|---|
| 840 draws at 13 ms CPU, 2 descriptor sets, dynamic offsets, zero GPU-driven | Roblox | **Immediate-mode-ish, CPU-submitted draws, tiny fixed binding sets, dynamic offsets.** No bindless heap. |
| 10k draws at 60 fps on a $99 phone via precreated pipelines + bind groups + metadata-only command stream | HypeHype | **Per-draw cost must be metadata**: a few bytes appended to a flat array, nothing allocated, nothing looked up. |
| Indexed > indirect by 1.44× FPS and 1.9× power; indirect costs 3.3× text-read cycles | Qualcomm, measured | **Indirect is an escape hatch, not the architecture.** Culling on the CPU; submit with indexed draws. |
| `maxDrawIndirectCount == 1` on pre-G710 Mali | Arm | **MDI cannot be the primary path.** Ever, on the fleet. |
| Bindless needs update-after-bind descriptor indexing (not in Android Baseline 2021/2022) and adds a dependent descriptor→texture load per access | Khronos profiles + Qualcomm's cycle counts | **Materials are fixed-slot bind groups.** 4 textures, not 65 536. |

The API that follows from these five is not `vk.h` with mobile tweaks. It is a different shape:
**a retained draw list of 24-byte records, sorted and coalesced, bound through a handful of
precreated bind groups, with the pass table compiled at startup.** That is `mg.h`, designed in
`MOBILE_API_DESIGN.md`.
