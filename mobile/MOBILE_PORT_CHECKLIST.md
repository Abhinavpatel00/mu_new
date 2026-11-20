# Mobile port checklist

Per-frame gates, per-pass gates, and the review questions that catch the mistakes this document
exists to prevent. Nothing here repeats `MOBILE_VK_RESEARCH.md`; each line points at the section
that explains it.

---

## 1. Per frame

```
[ ] one vkQueueSubmit, one command buffer, one recording thread            (§8.4, doctrine 9)
[ ] FrameStats.barriers == (number of buffer/arena dependencies), not per pass   (§8.3)
[ ] FrameStats.layout_transitions == 0 in the steady state                (§8.3)
[ ] every PassDesc hit the pass cache; no vkCreateRenderPass after frame 2       (§8.1)
[ ] no VkFramebuffer created after frame 2                                       (§8.1)
[ ] no vkAllocateMemory, no vkAllocateDescriptorSets in the steady state        (§8.5)
[ ] mapped writes flushed once, at frame_end, per contiguous range             (§8.5)
[ ] no read through a mapped pointer (write-combining: ~100x slower)           (§8.5)
[ ] external read/write bytes <= the §3 ledger + 10%                            (§3)
[ ] pipelines created: 0                                                       (§8.4)
```

## 2. Per pass

```
[ ] <= MU_TILE_COLOR_MAX (4) colour attachments, all justified                 (§2.3)
[ ] single-sample attachments marked transient where they are never sampled     (§7.3)
[ ] every MSAA attachment: storeOp = DONT_CARE + a resolve attachment in-pass    (§7.3)
[ ] every attachment's storeOp is derived, not defaulted: is it read later?     (§7.3)
[ ] packed depth/stencil (D24S8): both aspects same load/store or neither        (§7.3)
[ ] no LOAD on an attachment the pass fully overwrites                          (§2.3)
[ ] no vkCmdClearAttachments, no shader clears, no vkCmdResolveImage            (§2.3)
[ ] no compute dispatch between two passes on the same attachments              (§2.5)
[ ] subpass dependencies are BY_REGION                                          (§5.1)
[ ] depth after geometry is DEPTH_STENCIL_READ_ONLY, not GENERAL                (§7.2)
[ ] the depth-writing subpass has no discard, gl_FragDepth, or alpha-to-coverage (§2.4)
[ ] passes that must merge are adjacent, same attachments, same size            (§2.5)
```

## 3. Shaders

```
[ ] push constant block <= 128 bytes; per-draw data in ARENA_DEVICE              (§7.1)
[ ] per-draw constants are wave-uniform (uniform buffer / scalar path)          (§7.1)
[ ] mediump/half where the precision allows; 16-bit storage queried, not assumed (§4.2)
[ ] no fp32 transcendental in the inner loop; LUT or half                      (§4.2)
[ ] local_size_x <= 128, multiple of 32; workgroup memory <= 32 KB             (§4.2)
[ ] subgroup-agnostic reductions (16 on Mali, 64 on Adreno, 32 on PowerVR)      (§4.2)
[ ] vertex positions in their own array (binning reads only them)               (§7.6)
[ ] bindless arrays reachable through the clamped tier's indices                (§7.5)
[ ] derivatives (dFdx/dFdy) reviewed for pre-rotation                           (§8.6)
```

## 4. Device and platform

```
[ ] push_bytes read from the device, not assumed                                (§4.2)
[ ] tier decided from api_version + dynamic rendering + sync2                   (§4.1)
[ ] subpass inputs compiled to N passes on Tier B, and counted in FrameStats    (§8.2)
[ ] minImageCount = 3, present mode FIFO, preTransform = currentTransform       (§8.6)
[ ] surface destroyed before the ANativeWindow, swapchain before the surface    (§9)
[ ] VK_ERROR_SURFACE_LOST_KHR is a state transition, not an abort               (§9)
[ ] onTrimMemory drops transient arenas, readbacks, and the pipeline blob cache (§8.5)
[ ] validation layers off in release                                             (§9)
[ ] pipeline cache path is the app's code cache dir, keyed by pipelineCacheUUID  (§8.4)
```

## 5. Review questions

Ask these about any new feature before it reaches `vk.h`.

1. **What does this cost on a tiler in bytes per pixel?** If the answer is "we'll measure", the
   design is not finished. (§3)
2. **Does this add a pass?** If yes, why can it not be a subpass, or folded into the pass next to
   it? (§2.5)
3. **Does this need a feature outside the declared tier?** Then it is an opt-in with a fallback and
   a `GpuInfo` bit, or it does not go in. (§4.1, doctrine 7)
4. **Can the driver elide the load or the store?** If the API cannot express it, the driver cannot
   know it. (§5.3)
5. **Does this make an attachment's lifetime shorter?** Then it should be transient. (§7.3)
6. **Does this allocate?** On the frame path, no. (§8.5)
7. **Does this make a new pipeline variant?** On EDS3-less devices it does, and stutter is the
   consequence. Count the variant against the pre-warm budget. (§8.4)
8. **Does it change what HSR/LRZ can do for the rest of the pass?** (§2.4)
9. **What is the fallback on the Tier B device?** If the answer is "it renders wrong", it is not
   done. (§8.2)
10. **Which counter moves, and by how much?** Name it before writing the code. (§10)

## 6. Known traps, in the order they bite

```
1.  MAX_PUSH_CONSTANTS 256                        vkCreatePipelineLayout fails on the device
2.  transient attachments not marked              full bandwidth + a real allocation
3.  one VkDeviceMemory per buffer                 4096-allocation cliff
4.  index range incoherent in one draw            Mali 180 MB parameter buffer -> DEVICE_LOST
5.  a barrier between two passes                  tile-resident chain becomes two DRAM trips
6.  a compute pass mid-frame                      same, plus no binning overlap
7.  no Tier B render path                         nothing renders on the 1.1 fleet
8.  bindless array over the granted limit         layout creation fails or indices read garbage
9.  preTransform not set                          the compositor rotates every frame
10. swapchain kept across a destroyed surface      black frames after backgrounding
11. no pipeline cache on disk                     stutter on the first draw of every pipeline
12. flush per allocation                           ~0.1 ms each on Mali
13. linear + optimal resources in one arena        bufferImageGranularity aliasing
14. global subpass dependency                       merging silently off, no error anywhere
```
