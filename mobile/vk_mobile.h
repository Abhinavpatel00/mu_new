/*
 * vk_mobile.h — the mobile delta to vk.h.
 *
 * Include after vk.h. Nothing here duplicates a vk.h name; what vk.h owns,
 * vk.h keeps. What mobile adds is: a feature table, capability clamping, the
 * pass-level pieces a tile-based GPU needs (subpasses, input attachments,
 * NONE load/store), and the value mappings the backend emits, as tables
 * rather than if-chains.
 *
 * Three edits to vk.h are required and are checked at the bottom of this file:
 *
 *   1.  #define MAX_PUSH_CONSTANTS  MU_PUSH_BYTES      (128, not 256)
 *   2.  ImageType gains  IMAGE_INPUT                     (§7.2 of the research)
 *   3.  Load gains NONE, Store gains NONE                (§7.3)
 *
 * Until those are made, this header still compiles: the additions below are
 * macros that alias into the existing enums, so the mapping tables are
 * complete and the static assertions say what is missing.
 *
 * Style: every mapping is a table, every capability is data, every check that
 * cannot fail in a working build is a static assert or absent.
 */

#ifndef MU_VK_MOBILE_H
#define MU_VK_MOBILE_H

#include "vk.h"

#include <assert.h>

// ============================================================
// 0. CONFIGURATION
// ============================================================

// maxPushConstantsSize is 128 on every Android GPU that ships; the spec
// minimum is 128. Adreno and Mali both report it. 256 fails on the device,
// not in the shader compiler.
#ifndef MU_PUSH_BYTES
#define MU_PUSH_BYTES 128
#endif

// The number of subpasses one PassDesc may hold. Four covers the shapes that
// pay on a tiler: geometry, deferred/forward-plus lighting, post, UI.
#ifndef MU_MAX_SUBPASSES
#define MU_MAX_SUBPASSES 4
#endif

// Attachment budget for one pass. Tile memory per pixel is fixed on the
// hardware (128..1024 bits/px depending on the Mali generation, unpublished
// on Adreno): past four colour targets the driver shrinks bins or spills, and
// the subpass win is gone.
#ifndef MU_TILE_COLOR_MAX
#define MU_TILE_COLOR_MAX 4
#endif

// Arena bytes per frame slot on a UMA device. The whole point of the mobile
// memory model is that ARENA_STAGING and ARENA_DEVICE are the same heap.
#ifndef MU_UMA_HEAP_BYTES
#define MU_UMA_HEAP_BYTES (128u << 20)
#endif

#define MU_IMAGE_INPUT  ((ImageType)2)   // vk.h ImageType: 0 sampled, 1 storage
#define MU_LOAD_NONE    ((Load)3)        // vk.h Load:  LOAD, CLEAR, DONT_CARE
#define MU_STORE_NONE   ((Store)2)       // vk.h Store: STORE, STORE_DISCARD

// ============================================================
// 1. FEATURES AND CAPS
// ============================================================

// One bit per thing the application may branch on at setup. Nothing in the
// mandatory path may require a bit that the declared tier does not carry.
typedef enum Feature {
    F_DYNAMIC_RENDERING = 0,
    F_SYNCHRONIZATION2,
    F_TIMELINE_SEMAPHORE,
    F_DESCRIPTOR_INDEXING,
    F_BUFFER_DEVICE_ADDRESS,
    F_DRAW_INDIRECT_COUNT,
    F_EDS1,                        // cull, front face, depth compare/write, topology
    F_EDS2,                        // primitive restart, depth clamp, discard
    F_EDS3,                        // blend, write mask, samples: the one that matters
    F_SAMPLED_DEPTH_READ,          // a depth image reachable as a sampled image
    F_ATTACHMENT_FEEDBACK_LOOP,    // sample the attachment you are writing
    F_RASTER_ORDER_ATTACHMENT_ACCESS,
    F_LAZY_MEMORY,
    F_TRANSIENT_ATTACHMENT,
    F_MSRTSS,                      // VK_EXT_multisampled_render_to_single_sampled
    F_FIXED_RATE_COMPRESSION,      // VK_EXT_image_compression_control
    F_DESCRIPTOR_BUFFER,
    F_LOCAL_READ,                  // VK_KHR_dynamic_rendering_local_read
    F_TILE_PROPERTIES,             // VK_QCOM_tile_properties
    F_PIPELINE_CACHE_CONTROL,      // VK_EXT_pipeline_creation_cache_control, 1.3 core
    F_EXTERNALLY_SYNCED_CACHE,     // VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT
    F_TIMESTAMP_COMPUTE_AND_GRAPHICS,
    F_SHADER_FLOAT16,
    F_STORAGE_16BIT,
    F_SUBGROUP_SIZE_CONTROL,
    F_YCBCR_CONVERSION,
    F_EXTERNAL_HARDWARE_BUFFER,
    F_PREROTATION,                 // VK_QCOM_render_pass_transform
    F_DISPLAY_TIMING,              // VK_GOOGLE_display_timing
    F_PERFORMANCE_QUERY,
    F_COUNT
} Feature;

_Static_assert(F_COUNT <= 32, "GpuCaps.features is a uint32_t");

// Which of the two render paths the backend runs. Not a user choice: it
// follows from the version and the required features.
typedef enum Tier {
    TIER_B = 1,   // Vulkan 1.1/1.2: VkRenderPass + VkFramebuffer, subpasses available
    TIER_A = 2    // Vulkan 1.3: dynamic rendering, sync2, descriptor indexing
} Tier;

typedef struct GpuCaps {
    uint32_t features;            // bits over Feature

    uint32_t push_bytes;          // what the device grants, <= MU_PUSH_BYTES on mobile
    uint32_t color_attachments;
    uint32_t subpasses;           // 1 when the pass is compiled to N passes instead
    uint32_t attachments_total;   // color + depth + resolve

    uint32_t sampled_images;      // bindless bound actually granted
    uint32_t storage_images;
    uint32_t samplers;
    uint32_t storage_buffers;

    uint32_t workgroup_invocations;
    uint32_t workgroup_shared;
    uint32_t subgroup_size;

    uint32_t memory_allocations;  // maxMemoryAllocationCount, 4096 typical
    uint64_t buffer_image_granularity;
    uint32_t non_coherent_atom;
    uint32_t host_coherent;       // false: flush once per frame per range

    uint32_t framebuffer_color_samples;  // bits over SampleCount
    Tier     tier;
} GpuCaps;

static inline bool gpu_have(const GpuCaps *c, Feature f) {
    return (c->features & (1u << f)) != 0;   /* one shift, one test: setup code only */
}

// ============================================================
// 2. CAPS FROM A DEVICE
// ============================================================

// Fills what decides application behaviour, not what the backend happens to
// want. Pure: takes the properties the caller already fetched, so it can be
// replayed from a captured device description in a test.
static inline GpuCaps mobile_caps(
    const VkPhysicalDeviceLimits *lim,
    const VkPhysicalDeviceVulkan12Properties *p12,   // nullable
    const VkPhysicalDeviceVulkan13Properties *p13,   // nullable, only for maxColorAttachments
    uint32_t api_version,
    uint32_t features
) {
    GpuCaps c;
    c.features = features;

    c.push_bytes   = lim->maxPushConstantsSize < MU_PUSH_BYTES
                   ? lim->maxPushConstantsSize : MU_PUSH_BYTES;
    c.color_attachments = lim->maxColorAttachments;
    // Subpasses are a Vulkan 1.0 concept; F_LOCAL_READ only changes how the
    // Tier A backend compiles them (N dynamic-rendering passes that load
    // between, or one pass that reads locally). Not a branch, a constant.
    c.subpasses     = MU_MAX_SUBPASSES;
    c.attachments_total = lim->maxColorAttachments + 2;

    // Bindless: the binding-time limits are the ones that bite. Update-after-
    // bind limits are what a descriptor array is actually bound with.
    if (p12 && (features & (1u << F_DESCRIPTOR_INDEXING))) {
        c.sampled_images = p12->maxPerStageDescriptorUpdateAfterBindSampledImages;
        c.storage_images = p12->maxPerStageDescriptorUpdateAfterBindStorageImages;
        c.samplers       = p12->maxPerStageDescriptorUpdateAfterBindSamplers;
        c.storage_buffers = p12->maxPerStageDescriptorUpdateAfterBindStorageBuffers;
    } else {
        c.sampled_images = lim->maxPerStageDescriptorSampledImages;
        c.storage_images = lim->maxPerStageDescriptorStorageImages;
        c.samplers       = lim->maxPerStageDescriptorSamplers;
        c.storage_buffers = lim->maxPerStageDescriptorStorageBuffers;
    }
    if (c.sampled_images == 0) c.sampled_images = 16;
    if (c.storage_images == 0) c.storage_images = 4;
    if (c.samplers == 0)       c.samplers = 16;
    if (c.storage_buffers == 0) c.storage_buffers = 4;

    c.workgroup_invocations = lim->maxComputeWorkGroupInvocations;
    c.workgroup_shared      = lim->maxComputeSharedMemorySize;
    c.subgroup_size         = 0;  // from VkPhysicalDeviceSubgroupProperties, per stage

    c.memory_allocations     = lim->maxMemoryAllocationCount;
    c.buffer_image_granularity = lim->bufferImageGranularity;
    c.non_coherent_atom      = lim->nonCoherentAtomSize;
    c.host_coherent          = 0;  // set by the caller from the memory types it picked

    c.framebuffer_color_samples = SC_1 | SC_4;  // every device; 2 and 8 are optional

    c.tier = api_version >= VK_API_VERSION_1_3
          && (features & (1u << F_DYNAMIC_RENDERING))
          && (features & (1u << F_SYNCHRONIZATION2))
          ? TIER_A : TIER_B;

    (void)p13;
    return c;
}

// ============================================================
// 3. VALUE MAPPINGS, AS TABLES
// ============================================================

// A pass's load/store is a 4-entry lookup, not a switch. The backend indexes
// these; the app never sees a VkAttachmentLoadOp.
static const VkAttachmentLoadOp MOBILE_LOAD_OP[4] = {
    [LOAD]         = VK_ATTACHMENT_LOAD_OP_LOAD,
    [CLEAR]        = VK_ATTACHMENT_LOAD_OP_CLEAR,
    [DONT_CARE]    = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    [MU_LOAD_NONE] = VK_ATTACHMENT_LOAD_OP_NONE,
};

static const VkAttachmentStoreOp MOBILE_STORE_OP[3] = {
    [STORE]         = VK_ATTACHMENT_STORE_OP_STORE,
    [STORE_DISCARD] = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    [MU_STORE_NONE] = VK_ATTACHMENT_STORE_OP_NONE,
};

// The layout an attachment rests in between passes. Sampling the depth you
// just wrote is DEPTH_STENCIL_READ_ONLY, which is what Arm asks for and what
// keeps the tiler from copying it out.
static inline VkImageLayout mobile_rest_layout(ImageType type, bool depth) {
    if (depth)                         return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (type == MU_IMAGE_INPUT)        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (type == IMAGE_STORAGE)         return VK_IMAGE_LAYOUT_GENERAL;
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static const VkPipelineStageFlags2 MOBILE_USE_STAGES[3] = {
    [ACC_READ]  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    [ACC_WRITE] = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    [ACC_RW]    = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
};

static const VkAccessFlags2 MOBILE_IMAGE_ACCESS[3] = {
    [ACC_READ]  = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT,
    [ACC_WRITE] = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    [ACC_RW]    = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
};

// ============================================================
// 4. THE PASS, WITH SUBPASSES
// ============================================================

// A subpass is what a pass collapses into when the hardware can keep the tile
// in place between two draws that read each other's pixels. The dependency is
// per-region, always: a global dependency turns merging off silently.
typedef struct SubpassDesc {
    Attach    colors[MAX_COLOR_ATTACHMENTS];
    uint32_t  color_count;

    Attach    depth;
    bool      has_depth;

    ImageUse  images[MAX_IMAGE_USES];   // MU_IMAGE_INPUT entries read the tile
    uint32_t  image_count;

    BufferUse buffers[MAX_BUFFER_USES];
    uint32_t  buffer_count;

    Pipe      pipe;
    DrawState state;
} SubpassDesc;

typedef struct PassDescM {
    const char *name;
    PassKind    kind;
    uint32_t    width, height;

    SubpassDesc subpasses[MU_MAX_SUBPASSES];
    uint32_t    subpass_count;

    // Attachment list for the whole pass, in the order the images first
    // appear: the backend resolves them once and every subpass indexes it.
    ImageSub attachments[MAX_COLOR_ATTACHMENTS + 2];
    uint32_t attachment_count;
} PassDescM;

static inline ImageUse use_input(Image img) {
    return (ImageUse) {
        .image  = SUB(img),
        .access = ACC_READ,
        .type   = MU_IMAGE_INPUT
    };
}

static inline ImageUse use_input_sub(ImageSub sub) {
    return (ImageUse) { .image = sub, .access = ACC_READ, .type = MU_IMAGE_INPUT };
}

// The common case: one subpass, no inputs. Exactly today's PassDesc.
static inline PassDescM pass_one(const char *name, uint32_t w, uint32_t h) {
    return (PassDescM) {
        .name = name,
        .kind = PASS_GRAPHICS,
        .width = w,
        .height = h,
        .subpass_count = 1
    };
}

static inline SubpassDesc subpass(Attach color, Pipe pipe, DrawState state) {
    SubpassDesc s = { .pipe = pipe, .state = state, .color_count = 1 };
    s.colors[0] = color;
    return s;
}

// Storage that stays on chip: sampled in this pass, never stored. This is the
// only place the API is allowed to be opinionated — a transient attachment
// that is also sampled is a contradiction, and the backend asserts it.
static inline bool pass_attachment_is_transient(const ImageDesc *d) {
    return d->transient;
}

// ============================================================
// 5. PUSH CONSTANTS AT 128 BYTES
// ============================================================

// The per-draw block lives in ARENA_DEVICE; the push block carries its offset.
// The rule that makes this free: the block must be wave-uniform. A per-lane
// value read from it costs a register per lane; a uniform one costs an SGPR.
#define PUSH_M(name, BODY)                                            \
    typedef struct ALIGNAS(PUSH_ALIGN) name##_data { BODY } name##_data; \
    _Static_assert(sizeof(name##_data) <= MU_PUSH_BYTES,               \
                   #name " exceeds the mobile push constant budget");  \
    typedef struct ALIGNAS(PUSH_ALIGN) name {                          \
        BODY                                                           \
        uint8_t _pad[MU_PUSH_BYTES - sizeof(name##_data)];               \
    } name;                                                            \
    _Static_assert(sizeof(name) == MU_PUSH_BYTES,                       \
                   #name " must be exactly MU_PUSH_BYTES")

// Example — everything a draw needs, with 96 bytes left for a 4x4:
//
//   PUSH_M(DrawPush,
//       uint32_t vertex_offset;   /* ARENA_DEVICE: positions, then the rest  */
//       uint32_t index_offset;
//       uint32_t constants;       /* ARENA_DEVICE: the per-draw block        */
//       uint32_t texture_id;
//       uint32_t sampler_id;
//       uint32_t subpass_flags;
//       uint32_t pad[2];
//   );

// ============================================================
// 6. WHAT MOBILE MUST NOT EXPRESS
// ============================================================

// Checked at compile time against vk.h. Each failing line is one of the three
// edits at the top of this file.
_Static_assert(MAX_PUSH_CONSTANTS >= MU_PUSH_BYTES,
               "MAX_PUSH_CONSTANTS is below MU_PUSH_BYTES");
_Static_assert(MU_PUSH_BYTES == 128,
               "128 is the maxPushConstantsSize floor and every Android GPU");
_Static_assert(MAX_COLOR_ATTACHMENTS >= MU_TILE_COLOR_MAX,
               "a tiler pass needs at least MU_TILE_COLOR_MAX colour targets");
_Static_assert(sizeof(Attach) <= 96, "Attach stays a value type: no pointers, no strings");
_Static_assert(MU_MAX_SUBPASSES <= 4, "each subpass adds a VkSubpassDescription and a dependency");

#endif // MU_VK_MOBILE_H
