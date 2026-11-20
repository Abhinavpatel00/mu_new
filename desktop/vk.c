/*
 * vk.c — the Vulkan 1.3 backend behind vk.h.
 *
 * What the header promises and how this file keeps it:
 *
 *   SHADER ABI     One descriptor set, bound once per command buffer:
 *                    binding 0  sampled images[]   image_sampled_id(), image_sub_sampled_id()
 *                    binding 1  samplers[]         sampler_id()
 *                    binding 2  storage images[]   image_storage_id(), image_sub_storage_id()
 *                    binding 3  ONE storage buffer, the whole ARENA_DEVICE pool;
 *                               shaders address it with the byte offsets that
 *                               arena_alloc() hands out.
 *                  Index 0 of every array is a live dummy (1x1 white image,
 *                  linear-clamp sampler), so a zeroed push block is safe to run.
 *                  Any other buffer reaches a shader through its device address
 *                  (buffer_info().address). Nothing else is ever bound.
 *
 *   GEOMETRY       There are no vertex or index buffers. draw() and
 *                  draw_indexed() both launch N vertex shader invocations; the
 *                  shader pulls its vertex (draw) or its index and then its
 *                  vertex (draw_indexed) out of binding 3 or a device address.
 *                  That keeps vertex layout out of the pipeline key.
 *
 *   CONVENTIONS    Negative-height viewport: clip +Y is up, uv (0,0) is the top
 *                  left texel, counter-clockwise is front. The draw_state_*
 *                  presets are reverse-Z (CMP_GREATER, clear depth to 0).
 *
 *   PIPELINES      A Pipe is a description. The VkPipeline behind it is built
 *                  on first use for the attachment formats of the pass, and only
 *                  the state this device cannot change dynamically is part of
 *                  that key. With VK_EXT_extended_dynamic_state3 that leaves
 *                  shaders + formats + topology class; without it blend, write
 *                  mask and sample count are baked in as well (MU_NO_EDS3=1
 *                  forces that path for A/B runs).
 *                  What the driver compiled is kept between runs: gpu_create
 *                  loads the pipeline cache from
 *                    <cache>/<name>/pipelines-<vendor>-<device>.bin
 *                  and gpu_destroy writes it back when the run added to it.
 *                  <cache> is $XDG_CACHE_HOME when that is an absolute path and
 *                  ~/.cache otherwise; <name> is GpuDesc.name. A first draw
 *                  then costs a lookup on every driver, not only on those that
 *                  keep a cache of their own.
 *
 *   SYNC           A pass declares what it touches; begin_pass() turns that
 *                  into one vkCmdPipelineBarrier2. Images are tracked per mip,
 *                  buffers as a whole. Data written by an upload is readable
 *                  everywhere afterwards without a declaration; data written by
 *                  a pass must be declared by the passes that read it. A depth
 *                  attachment declared read-only is in the layout a sampled
 *                  depth image has, so one pass can test against it and sample
 *                  it: no feedback loop, and nothing to write back.
 *
 *   TIMELINE       One timeline semaphore numbers every submit. Destroyed
 *                  resources, bindless slots, staging bytes and readbacks are
 *                  all retired by comparing against it. There is one submit per
 *                  frame, and the queue itself is only ever waited on when a
 *                  swapchain goes away.
 *
 *   PRESENT        A frame uses two binary semaphores, and they have different
 *                  owners. The one the acquire signals belongs to the frame
 *                  slot: the slot's submit waits on it, and the timeline says
 *                  when that submit is over. The one the present waits on
 *                  belongs to the swapchain image and is indexed by the
 *                  acquired image index: nothing reports when a present has let
 *                  go of its semaphore, except the same image being acquired
 *                  again.
 *
 *   FAILURE        Setup failure is fatal where it happens: the message says
 *                  what failed and the process exits. Callers do not re-check.
 *
 * Sections, in dependency order:
 *    1 state            2 errors and names     3 timeline and retirement
 *    4 command buffers  5 barriers             6 gpu create / destroy
 *    7 buffers, arenas  8 images, samplers     9 shaders and pipelines
 *   10 swapchain, frame 11 passes and commands 12 upload and readback
 *   13 profiling
 */

#include "external/mu_lib/mu/mu_macros.h" /* first: owns the POSIX feature test */
#include "external/mu_lib/mu/mu_fast_codec.h"
#include "external/mu_lib/mu/mu_files.h"
#include "external/mu_lib/mu/mu_id_pool.h"
#include "external/mu_lib/mu/mu_time.h"

#define VOLK_IMPLEMENTATION
#include "external/volk/volk.h"

#define MU_ESCAPE
#include "vk.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ============================================================================
 * 1. STATE
 * ==========================================================================*/

enum {
    MAX_BUFFERS        = 4096,
    MAX_IMAGES         = 8192,
    MAX_SHADERS        = 512,
    MAX_VARIANTS       = 1024, /* VkPipeline objects behind the Pipe handles */
    MAX_IMAGE_VIEWS    = 16,   /* per image; entry 0 is the whole image, and every mip can have one of its own */
    MAX_RETIRED        = 8192,
    MAX_READBACKS      = 64,
    MAX_SWAP_IMAGES    = 8,
    MAX_IMAGE_BARRIERS = 256,
    MAX_STAGING_MARKS  = 256,
    MAX_UPLOAD_IMAGES  = 64,
    MAX_VIEWS          = 16, /* bits in PipeDesc.view_mask */
    /* timestamps of one frame: see Frame.query_count */
    QUERIES_PER_FRAME  = 2 + 2 * MAX_PASSES + 2 * MAX_PROFILE_SCOPES * MAX_VIEWS,
    STATISTIC_COUNT    = 5,
    INLINE_UPLOAD_MAX  = 65536, /* vkCmdUpdateBuffer limit: below it no staging */
    PIPELINE_MAGIC     = 0x4350554d, /* "MUPC", first bytes of the pipeline cache file */
    PIPELINE_ROOT_MAX  = 768,        /* longest cache directory taken from the environment */
    POOL_GRANULE       = 65536  /* arena sizes are multiples of this */
};

#define STAGES_SHADER (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
/* what a pipeline creation reports when the pipeline cache already held the pipeline */
#define CACHE_HIT     (VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT | VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)
#define STAGES_PUSH   (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)

typedef struct FormatInfo {
    VkFormat           vk;
    VkImageAspectFlags aspect;
    uint32_t           bytes;   /* per texel */
    uint32_t           integer; /* cannot blend, resolves by sample zero */
} FormatInfo;

static const FormatInfo FORMAT_TABLE[FMT_COUNT] = {
    [FMT_NONE]          = { VK_FORMAT_UNDEFINED, 0, 0, 0 },
    [FMT_RGBA8_UNORM]   = { VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 },
    [FMT_RGBA8_SRGB]    = { VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 },
    [FMT_BGRA8_UNORM]   = { VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 },
    [FMT_BGRA8_SRGB]    = { VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 },
    [FMT_RGBA16F]       = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 8, 0 },
    [FMT_RGBA32F]       = { VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 16, 0 },
    [FMT_RG16F]         = { VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 },
    [FMT_RG8_UNORM]     = { VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 2, 0 },
    [FMT_R32F]          = { VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 },
    [FMT_R16F]          = { VK_FORMAT_R16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 2, 0 },
    [FMT_R8_UNORM]      = { VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 0 },
    [FMT_RGB10A2_UNORM] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 },
    [FMT_R11G11B10F]    = { VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 },
    [FMT_D16_UNORM]     = { VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT, 2, 0 },
    [FMT_D24S8]         = { VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 4, 0 },
    [FMT_D32F]          = { VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 4, 0 },
    [FMT_R32_UINT]      = { VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, 4, 1 },
    [FMT_RGBA8_UINT]    = { VK_FORMAT_R8G8B8A8_UINT, VK_IMAGE_ASPECT_COLOR_BIT, 4, 1 },
    [FMT_RGBA8_SNORM]   = { VK_FORMAT_R8G8B8A8_SNORM, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 },
};

/* What the GPU last did with a resource. `stages`/`access` describe the last
 * write, or every read since it; all of those reads already sit behind a
 * barrier, so a new read only needs one when it adds a stage or access. */
typedef struct Sync {
    VkPipelineStageFlags2 stages;
    VkAccessFlags2        access;
    VkImageLayout         layout;  /* images only */
    uint32_t              written; /* the last access was a write */
    uint32_t              epoch;   /* barrier batch that last touched it */
    uint32_t              barrier; /* its barrier inside that batch */
} Sync;

typedef struct ImageView {
    VkImageView        view;
    VkImageAspectFlags aspect;
    uint16_t           mip, mip_count, layer, layer_count;
    uint32_t           sampled_id, storage_id; /* 0 = no shader reaches this view that way */
} ImageView;

typedef struct ImageSlot {
    VkImage        image;
    VkDeviceMemory memory; /* null for swapchain images, which are not ours */
    ImageView      views[MAX_IMAGE_VIEWS];
    Sync           sync[MAX_MIPS];
    FormatInfo     format;
    uint32_t       width, height, depth, layers, mips, samples;
    uint32_t       usage;                  /* ImageUsage bits */
    uint32_t       view_count;
    uint32_t       sampled_id, storage_id; /* of the whole image: views[0]. 0 = the dummy = none of its own */
    uint32_t       gen;
    /* What image_create() was asked for, kept because an image that follows
     * the frame is built from it again at every other frame size. `name` is
     * the copy that desc.name points at. */
    ImageDesc      desc;
    char           name[48];
    bool           follows; /* no size of its own: the frame's, halved desc.frame_shift times */
} ImageSlot;

typedef struct BufferSlot {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    void          *mapped;
    uint64_t       address, size;
    Sync           sync;
    uint32_t       gen;
} BufferSlot;

typedef struct SamplerSlot {
    VkSampler   sampler; /* stays set until the timeline retires it: the slot is the bindless index */
    SamplerDesc desc;
    uint32_t    refs, gen;
} SamplerSlot;

typedef struct ShaderSlot {
    VkShaderModule        module;
    VkShaderStageFlagBits stage;
    uint64_t              key; /* hash of path, entry, stage and defines */
    char                  entry[64];
} ShaderSlot;

typedef struct PipeKey {
    ShaderId vertex, fragment, compute;
    uint16_t view_mask;
    char     entries[3][64];
} PipeKey;

typedef struct PipeSlot {
    PipeKey    key;
    VkPipeline compute;
    uint32_t   refs, gen;
    char       name[32];
} PipeSlot;

typedef struct VariantKey {
    VkFormat  colors[MAX_COLOR_ATTACHMENTS];
    VkFormat  depth;
    uint32_t  color_count;
    uint32_t  pipe, pipe_gen;
    DrawState state; /* only what is static on this device, see Gpu.static_state */
} VariantKey;

typedef struct Variant {
    VariantKey key;
    VkPipeline pipeline;
} Variant;

/* The file the pipeline cache is kept in between runs: this header, then the
 * bytes of vkGetPipelineCacheData. Drivers differ in how carefully they read
 * data they did not write or that was cut short, so none reaches one unless
 * every field here matches. It is compared and written as bytes: no padding. */
typedef struct PipelineFile {
    uint32_t magic, header_bytes; /* PIPELINE_MAGIC, sizeof(PipelineFile) */
    uint32_t pointer_bytes;       /* a 32-bit and a 64-bit build of one driver do not share data */
    uint32_t vendor, device, driver_version;
    uint8_t  uuid[VK_UUID_SIZE]; /* VkPhysicalDeviceProperties.pipelineCacheUUID */
    uint64_t size, hash;         /* of the driver's bytes; everything above is who wrote them */
} PipelineFile;

_Static_assert(sizeof(PipelineFile) == 56 && offsetof(PipelineFile, size) == 40, "PipelineFile must have no padding");
_Static_assert(MAX_IMAGE_VIEWS >= MAX_MIPS + 2, "a view for the whole image, one for each mip, one to spare");
_Static_assert(MAX_VIEWS == 8 * sizeof(((PipeDesc *)0)->view_mask), "a query for every view a view mask can name");
_Static_assert(QUERIES_PER_FRAME <= UINT16_MAX, "Frame keeps query indices in 16 bits");

/* Everything a destroyed object leaves behind. Vulkan destroy calls accept null
 * handles, so one record shape serves every kind and the free path has no
 * switch. */
typedef struct Retired {
    uint64_t        value; /* freed once the timeline has passed this */
    VkBuffer        buffer;
    VkImage         image;
    VkImageView     views[MAX_IMAGE_VIEWS];
    uint32_t        sampled_ids[MAX_IMAGE_VIEWS]; /* of those views */
    uint32_t        storage_ids[MAX_IMAGE_VIEWS];
    uint32_t        view_count; /* zero for everything that is not an image */
    VkDeviceMemory  memory;
    VkSampler       sampler;
    VkPipeline      pipeline;
    VkCommandBuffer cmd;
    uint32_t        sampler_slot;
} Retired;

typedef struct StagingMark {
    uint64_t end, value; /* staging bytes below `end` are free once `value` completes */
} StagingMark;

typedef struct Frame {
    VkCommandPool   pool;
    VkCommandBuffer cmd;
    VkSemaphore     acquired; /* signalled by the acquire, waited on by this slot's submit */
    VkQueryPool     timestamps, statistics;
    uint64_t        value; /* timeline value of this slot's last submit */
    uint64_t        begin_ns;
    FrameStats      stats;
    /* Timestamps take the queries of `timestamps` in the order they are
     * written. The first and the last are the frame's own, a pass has the two
     * it lies between, and so has a scope: the one that begins it, and the
     * next one of its pass, which begins another scope or is where end_pass()
     * closed the last. Inside a pass a timestamp takes a query for every view,
     * which is what QUERIES_PER_FRAME allows for. In this order a driver that
     * spends more queries on a timestamp than it should (lavapipe 25.0 does,
     * outside a pass, once a pass with views has been drawn) only reaches
     * queries that are written again later or never read. */
    uint32_t        query_count;
    uint16_t        pass_queries[MAX_PASSES][2];
    uint16_t        scope_queries[MAX_PROFILE_SCOPES][2];
    bool            profiled; /* timestamps were written this frame */
    bool            pending;  /* stats are waiting for the GPU */
} Frame;

struct Pass {
    Gpu            *g;
    VkCommandBuffer cmd;
    PassStat       *stat;
    VkFormat        color_formats[MAX_COLOR_ATTACHMENTS];
    uint32_t        color_integer[MAX_COLOR_ATTACHMENTS];
    VkFormat        depth_format, stencil_format;
    uint32_t        color_count, samples;
    uint32_t        row;   /* of `stat` in FrameStats.passes; MAX_PASSES for a pass beyond that */
    uint32_t        views; /* queries a timestamp takes while this pass renders: one for each view */
    PassKind        kind;
    Pipe            pipe;
    DrawState       state;   /* requested */
    DrawState       applied; /* dynamic state already in the command buffer */
    VkPipeline      bound;
    VkExtent2D      area;     /* what the pass renders to; the scissor never leaves it */
    VkViewport      viewport; /* the ones in the command buffer, like `applied` */
    VkRect2D        scissor;
    bool            applied_valid, open, timed;
    bool            scoped; /* a timed scope is open: end_pass() owes it a timestamp */
    bool            depth_read_only; /* the depth attachment is tested, never written */
    bool            run_open;        /* a draw has used the pipeline, state, viewport and scissor as they are now */
};

struct Upload {
    Gpu     *g;
    Image    images[MAX_UPLOAD_IMAGES]; /* waiting for their resting layout */
    uint32_t image_count;
    bool     open, wrote_buffers;
};

struct Readback {
    Buffer   staging;
    void    *dst;
    uint64_t size, value;
    bool     used, done;
};

struct Gpu {
    /* device */
    VkInstance                       instance;
    VkDebugUtilsMessengerEXT         messenger;
    VkPhysicalDevice                 physical;
    VkDevice                         device;
    VkQueue                          queue;
    uint32_t                         queue_family;
    VkPhysicalDeviceMemoryProperties memory;
    VkPipelineCache                  pipeline_cache;
    PipelineFile                     pipeline_file;       /* this device's header; size and hash are the file's as it was loaded */
    char                             pipeline_path[1024]; /* empty: there is nowhere to keep it */
    bool                             pipeline_added;      /* the cache holds something the file does not */
    GpuDesc                          desc;
    GpuInfo                          info;
    FormatInfo                       formats[FMT_COUNT];
    DrawState                        static_state; /* all-ones in the fields a pipeline has to bake in */
    double                           ms_per_tick;
    char                             error[512];
    bool                             debug_utils, swapchain_ext, eds3, timestamps, statistics, anisotropy;

    /* bindless */
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool      set_pool;
    VkDescriptorSet       set;
    VkPipelineLayout      layout;
    mu_id_pool            sampled_ids, storage_ids;

    /* timeline */
    VkSemaphore timeline;
    uint64_t    submitted, completed;

    /* recording */
    VkCommandPool   batch_pool;
    VkCommandBuffer cmd; /* the open command buffer, null between batches */
    Frame           frames[MAX_FRAMES_IN_FLIGHT];
    uint32_t        frame_index;
    bool            in_frame, profile;
    Image           frame_target;
    Pass            pass;
    Upload          upload;
    FrameStats     *recording; /* the frame being recorded, or `offline` */
    FrameStats      offline;   /* work recorded outside a frame */
    FrameStats      stats;     /* newest frame the GPU has finished */
    PassStat        spill;     /* passes beyond MAX_PASSES count here */
    uint64_t        statistics_result[STATISTIC_COUNT];

    /* barriers of the batch being built */
    VkImageMemoryBarrier2  image_barriers[MAX_IMAGE_BARRIERS];
    VkBufferMemoryBarrier2 buffer_barriers[MAX_BUFFER_USES];
    uint32_t               image_barrier_count, buffer_barrier_count, epoch;

    /* presentation */
    VkSurfaceKHR   surface;
    VkSwapchainKHR swapchain;
    Image          swap_images[MAX_SWAP_IMAGES];
    /* Signalled by the frame's submit, waited on by its present. One per
     * swapchain image, indexed by the acquired image: with more images than
     * frame slots, a slot comes round again before the image it last
     * presented has been re-acquired, and until then that present may still
     * hold the semaphore. */
    VkSemaphore    swap_rendered[MAX_SWAP_IMAGES];
    uint32_t       swap_count, swap_index;
    bool           swap_stale;
    /* A surface without a size of its own takes the swapchain's, and the
     * swapchain takes the window's from gpu_resize(). */
    bool           swap_sizeless;
    uint32_t       window_width, window_height;
    Image          headless;

    /* arenas: ARENA_DEVICE bumps forever, ARENA_TRANSIENT bumps inside one
     * frame's slice, ARENA_STAGING is a ring freed by the timeline */
    Buffer      arena_buffers[3];
    ArenaStats  arena_stats[3];
    uint64_t    staging_head, staging_tail; /* monotonic byte counters */
    uint64_t    staging_marked;             /* head at the last submit */
    StagingMark staging_marks[MAX_STAGING_MARKS];
    uint32_t    staging_mark_head, staging_mark_count;

    /* tables: fixed slots, generation handles, slot 0 is the nil/dummy */
    mu_id_pool  buffer_ids, image_ids;
    BufferSlot *buffers;
    ImageSlot  *images;
    SamplerSlot samplers[MAX_BINDLESS_SAMPLERS];
    ShaderSlot  shaders[MAX_SHADERS];
    PipeSlot    pipes[MAX_PIPELINES];
    Variant     variants[MAX_VARIANTS];
    uint32_t    shader_count, variant_count;
    bool        compiler_missing; /* slangc was looked for once and is not there */
    Retired    *retired;
    uint32_t    retired_head, retired_count;
    Readback    readbacks[MAX_READBACKS];
};

/* ============================================================================
 * 2. ERRORS AND NAMES
 * ==========================================================================*/

__attribute__((format(printf, 1, 2))) static _Noreturn void gpu_fatal(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "vk: fatal: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

#define VK_CHECK(call)                                                                                  \
    do {                                                                                                \
        VkResult vk_check_result = (call);                                                              \
        if (vk_check_result != VK_SUCCESS)                                                              \
            gpu_fatal("%s returned VkResult %d (%s:%d)", #call, (int)vk_check_result, __FILE__, __LINE__); \
    } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT             types,
                                                     const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                     void                                       *user)
{
    Gpu        *g     = user;
    const char *label = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT     ? "error"
                        : severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? "warning"
                                                                                      : "info";

    (void)types;
    fprintf(stderr, "vk: validation %s: %s\n", label, data->pMessage);
    /* gpu_error() keeps the latest error so a test can fail on it */
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        snprintf(g->error, sizeof g->error, "%s", data->pMessage);
    return VK_FALSE;
}

static void name_object(Gpu *g, VkObjectType type, uint64_t handle, const char *name)
{
    VkDebugUtilsObjectNameInfoEXT info = {
        .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType   = type,
        .objectHandle = handle,
        .pObjectName  = name,
    };

    if (g->debug_utils && name)
        vkSetDebugUtilsObjectNameEXT(g->device, &info);
}

static BufferSlot *buffer_slot(const Gpu *g, Buffer b)
{
    MU_ASSERT(b.index < MAX_BUFFERS && g->buffers[b.index].gen == b.gen);
    return &g->buffers[b.index];
}

static ImageSlot *image_slot(const Gpu *g, Image img)
{
    MU_ASSERT(img.index < MAX_IMAGES && g->images[img.index].gen == img.gen);
    return &g->images[img.index];
}

/* ============================================================================
 * 3. TIMELINE AND RETIREMENT
 * ==========================================================================*/

/* Reads how far the GPU has got and frees everything that was waiting on it. */
static void timeline_poll(Gpu *g)
{
    Retired *r;
    uint32_t i;

    VK_CHECK(vkGetSemaphoreCounterValue(g->device, g->timeline, &g->completed));

    while (g->retired_count && g->retired[g->retired_head].value <= g->completed)
    {
        r = &g->retired[g->retired_head];
        for (i = 0; i < r->view_count; ++i)
        {
            vkDestroyImageView(g->device, r->views[i], NULL);
            /* id 0 is the dummy and is never handed back, so 0 doubles as "none" */
            if (r->sampled_ids[i])
                mu_id_pool_destroy_id(&g->sampled_ids, r->sampled_ids[i]);
            if (r->storage_ids[i])
                mu_id_pool_destroy_id(&g->storage_ids, r->storage_ids[i]);
        }
        vkDestroyBuffer(g->device, r->buffer, NULL);
        vkDestroyImage(g->device, r->image, NULL);
        vkFreeMemory(g->device, r->memory, NULL);
        vkDestroySampler(g->device, r->sampler, NULL);
        vkDestroyPipeline(g->device, r->pipeline, NULL);
        if (r->cmd)
            vkFreeCommandBuffers(g->device, g->batch_pool, 1, &r->cmd);
        /* a sampler's table slot is its bindless index, so it frees only now;
         * every other record carries slot 0 and rewrites the default with itself */
        g->samplers[r->sampler_slot].sampler = r->sampler_slot ? VK_NULL_HANDLE : g->samplers[0].sampler;
        g->retired_head = (g->retired_head + 1) % MAX_RETIRED;
        g->retired_count--;
    }

    while (g->staging_mark_count && g->staging_marks[g->staging_mark_head].value <= g->completed)
    {
        g->staging_tail      = g->staging_marks[g->staging_mark_head].end;
        g->staging_mark_head = (g->staging_mark_head + 1) % MAX_STAGING_MARKS;
        g->staging_mark_count--;
    }
}

static void timeline_wait(Gpu *g, uint64_t value)
{
    VkSemaphoreWaitInfo info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &g->timeline,
        .pValues        = &value,
    };

    /* a value nobody submitted would block forever */
    MU_ASSERT(value <= g->submitted);
    VK_CHECK(vkWaitSemaphores(g->device, &info, UINT64_MAX));
    timeline_poll(g);
}

/* A zeroed record that the next submit covers: anything recorded so far may
 * still reference the object. */
static Retired *retire(Gpu *g)
{
    Retired *r;

    if (g->retired_count == MAX_RETIRED)
        timeline_poll(g);
    if (g->retired_count == MAX_RETIRED)
        gpu_fatal("%d objects are waiting for the GPU; submit a frame before destroying more", MAX_RETIRED);

    r = &g->retired[(g->retired_head + g->retired_count++) % MAX_RETIRED];
    memset(r, 0, sizeof *r);
    r->value = g->submitted + 1;
    return r;
}

/* ============================================================================
 * 4. COMMAND BUFFERS
 * ==========================================================================*/

/* Every command buffer starts the same way: the one descriptor set, bound for
 * both bind points, and nothing is bound after that. */
static void cmd_begin(Gpu *g, VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VK_CHECK(vkBeginCommandBuffer(cmd, &info));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, 0, 1, &g->set, 0, NULL);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->layout, 0, 1, &g->set, 0, NULL);
    g->cmd = cmd;
}

/* The command buffer being recorded. Inside a frame that is the frame's; outside
 * one this opens a batch, so uploads, readbacks and bake passes need no frame. */
static VkCommandBuffer cmd_open(Gpu *g)
{
    VkCommandBufferAllocateInfo info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = g->batch_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;

    if (g->cmd)
        return g->cmd;
    VK_CHECK(vkAllocateCommandBuffers(g->device, &info, &cmd));
    cmd_begin(g, cmd);
    return cmd;
}

/* The only vkQueueSubmit2 in the file. Every submit advances the timeline by
 * one; `wait` and `signal` are the swapchain's binary semaphores or null. */
static void cmd_submit(Gpu *g, VkSemaphore wait, VkSemaphore signal)
{
    VkCommandBufferSubmitInfo cmd_info = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = g->cmd,
    };
    VkSemaphoreSubmitInfo wait_info = {
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = wait,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal_infos[2] = {
        { .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = g->timeline,
          .value     = g->submitted + 1,
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
        { .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = signal,
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
    };
    VkSubmitInfo2 info = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount   = wait != VK_NULL_HANDLE,
        .pWaitSemaphoreInfos      = &wait_info,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cmd_info,
        .signalSemaphoreInfoCount = 1 + (signal != VK_NULL_HANDLE),
        .pSignalSemaphoreInfos    = signal_infos,
    };
    uint32_t mark;

    VK_CHECK(vkEndCommandBuffer(g->cmd));
    VK_CHECK(vkQueueSubmit2(g->queue, 1, &info, VK_NULL_HANDLE));
    g->submitted++;
    g->cmd = VK_NULL_HANDLE;

    /* Staging bytes handed out since the last submit ride this one. When the
     * mark ring is full the newest mark absorbs them: it then frees later than
     * it could have, never earlier. */
    if (g->staging_head != g->staging_marked)
    {
        g->staging_mark_count += g->staging_mark_count < MAX_STAGING_MARKS;
        mark                   = (g->staging_mark_head + g->staging_mark_count - 1) % MAX_STAGING_MARKS;
        g->staging_marks[mark] = (StagingMark){ g->staging_head, g->submitted };
        g->staging_marked      = g->staging_head;
    }
}

/* Submits work recorded outside a frame. Inside a frame it does nothing: that
 * work rides the frame's single submit. */
static void flush(Gpu *g)
{
    if (!g->cmd || g->in_frame)
        return;
    retire(g)->cmd = g->cmd;
    cmd_submit(g, VK_NULL_HANDLE, VK_NULL_HANDLE);
    g->offline.pass_count  = 0;
    g->offline.scope_count = 0;
}

/* ============================================================================
 * 5. BARRIERS
 *    sync_image / sync_buffer queue what a declaration needs; barriers_flush
 *    emits the batch as one vkCmdPipelineBarrier2.
 * ==========================================================================*/

static void sync_image(Gpu *g, ImageSub sub, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access, bool write)
{
    ImageSlot             *s       = image_slot(g, sub.image);
    uint32_t               mip_end = sub.mip + MU_MAX(sub.mip_count, 1);
    uint32_t               mip;
    Sync                  *sync;
    VkImageMemoryBarrier2 *barrier;
    bool                   more_readers;

    MU_ASSERT(mip_end <= s->mips);
    for (mip = sub.mip; mip < mip_end; ++mip)
    {
        sync = &s->sync[mip];

        /* listed twice in one pass: one barrier, widened to cover both uses */
        if (sync->epoch == g->epoch)
        {
            MU_ASSERT(sync->layout == layout);
            barrier                 = &g->image_barriers[sync->barrier];
            barrier->dstStageMask  |= stages;
            barrier->dstAccessMask |= access;
            sync->stages           |= stages;
            sync->access           |= access;
            sync->written          |= write;
            continue;
        }

        /* a read in the same layout joins the readers; one that an earlier
         * barrier already covers needs nothing at all */
        more_readers = sync->layout == layout && !write && !sync->written;
        if (more_readers && !(stages & ~sync->stages) && !(access & ~sync->access))
            continue;

        MU_ASSERT(g->image_barrier_count < MAX_IMAGE_BARRIERS);
        g->recording->layout_transitions           += sync->layout != layout;
        g->image_barriers[g->image_barrier_count]  = (VkImageMemoryBarrier2){
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = sync->stages,
            .srcAccessMask       = sync->access,
            .dstStageMask        = stages,
            .dstAccessMask       = access,
            .oldLayout           = sync->layout,
            .newLayout           = layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = s->image,
            .subresourceRange    = { s->format.aspect, mip, 1, 0, VK_REMAINING_ARRAY_LAYERS },
        };
        sync->stages  = stages | (more_readers ? sync->stages : 0);
        sync->access  = access | (more_readers ? sync->access : 0);
        sync->layout  = layout;
        sync->written = write;
        sync->epoch   = g->epoch;
        sync->barrier = g->image_barrier_count++;
    }
}

/* Buffers are tracked whole: a range inside the device arena is synchronised
 * as the arena. That is conservative and never wrong. */
static void sync_buffer(Gpu *g, Buffer buffer, VkPipelineStageFlags2 stages, VkAccessFlags2 access, bool write)
{
    BufferSlot             *b            = buffer_slot(g, buffer);
    Sync                   *sync         = &b->sync;
    bool                    more_readers = !write && !sync->written;
    VkBufferMemoryBarrier2 *barrier;

    if (sync->epoch == g->epoch)
    {
        barrier                 = &g->buffer_barriers[sync->barrier];
        barrier->dstStageMask  |= stages;
        barrier->dstAccessMask |= access;
        sync->stages           |= stages;
        sync->access           |= access;
        sync->written          |= write;
        return;
    }
    if (more_readers && !(stages & ~sync->stages) && !(access & ~sync->access))
        return;

    /* a buffer nobody has touched has nothing to wait for */
    if (sync->stages)
    {
        MU_ASSERT(g->buffer_barrier_count < MAX_BUFFER_USES);
        g->buffer_barriers[g->buffer_barrier_count] = (VkBufferMemoryBarrier2){
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask        = sync->stages,
            .srcAccessMask       = sync->access,
            .dstStageMask        = stages,
            .dstAccessMask       = access,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer              = b->buffer,
            .size                = VK_WHOLE_SIZE,
        };
        sync->epoch   = g->epoch;
        sync->barrier = g->buffer_barrier_count++;
    }
    sync->stages  = stages | (more_readers ? sync->stages : 0);
    sync->access  = access | (more_readers ? sync->access : 0);
    sync->written = write;
}

static void barriers_flush(Gpu *g, VkCommandBuffer cmd)
{
    VkDependencyInfo info = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = g->buffer_barrier_count,
        .pBufferMemoryBarriers    = g->buffer_barriers,
        .imageMemoryBarrierCount  = g->image_barrier_count,
        .pImageMemoryBarriers     = g->image_barriers,
    };

    if (info.bufferMemoryBarrierCount + info.imageMemoryBarrierCount)
        vkCmdPipelineBarrier2(cmd, &info);
    g->recording->barriers  += info.bufferMemoryBarrierCount + info.imageMemoryBarrierCount;
    g->buffer_barrier_count  = 0;
    g->image_barrier_count   = 0;
    g->epoch++;
}

/* ============================================================================
 * 6. GPU CREATE / DESTROY
 * ==========================================================================*/

static void frame_stats_resolve(Gpu *g, Frame *f); /* section 13 */
static void surface_drop(Gpu *g);                  /* section 10 */

Gpu *gpu_create(const GpuDesc *desc)
{
    static const char *const INSTANCE_EXTENSIONS[] = {
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME, /* index 0: see debug_utils below */
        VK_KHR_SURFACE_EXTENSION_NAME,
        "VK_KHR_xcb_surface",
        "VK_KHR_xlib_surface",
        "VK_KHR_wayland_surface",
        "VK_KHR_win32_surface",
        "VK_EXT_headless_surface",
    };
    static const char *const VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";
    static const int         TYPE_RANK[5]     = {
        [VK_PHYSICAL_DEVICE_TYPE_OTHER]          = 0,
        [VK_PHYSICAL_DEVICE_TYPE_CPU]            = 1,
        [VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU]    = 2,
        [VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU] = 3,
        [VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU]   = 4,
    };
    struct Candidate {
        VkPhysicalDevice                                 device;
        VkPhysicalDeviceProperties2                      props;
        VkPhysicalDeviceVulkan11Properties               props11;
        VkPhysicalDeviceVulkan12Properties               props12;
        VkPhysicalDeviceFeatures2                        features;
        VkPhysicalDeviceVulkan11Features                 v11;
        VkPhysicalDeviceVulkan12Features                 v12;
        VkPhysicalDeviceVulkan13Features                 v13;
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
        uint32_t                                         queue_family, timestamp_bits;
        bool                                             swapchain_ext, eds3_ext;
        int                                              score;
    } candidate, chosen;
    Gpu                    *g = MU_CALLOC(1, sizeof *g);
    VkLayerProperties       layers[64];
    VkExtensionProperties  *extensions = MU_MALLOC(1024 * sizeof *extensions);
    VkPhysicalDevice        devices[16];
    VkQueueFamilyProperties families[16];
    const char             *enabled_extensions[MU_ARRAY_COUNT(INSTANCE_EXTENSIONS) + 1];
    const char             *missing;
    char                    rejected[1024] = "";
    uint32_t                layer_count = 64, device_count = 16, extension_count, family_count, enabled_count = 0;
    uint32_t                sampled_count, sampler_count, storage_count;
    uint32_t                i, j;
    bool                    validation = false;

    g->desc             = *desc;
    g->desc.width       = desc->width ? desc->width : 1280;
    g->desc.height      = desc->height ? desc->height : 720;
    g->window_width     = g->desc.width;
    g->window_height    = g->desc.height;
    g->recording        = &g->offline;
    g->epoch            = 1; /* zeroed Sync records must not look current */
    g->buffers          = MU_CALLOC(MAX_BUFFERS, sizeof *g->buffers);
    g->images           = MU_CALLOC(MAX_IMAGES, sizeof *g->images);
    g->retired          = MU_CALLOC(MAX_RETIRED, sizeof *g->retired);

    /* ---- instance ------------------------------------------------------- */
    if (volkInitialize() != VK_SUCCESS)
        gpu_fatal("no Vulkan loader (libvulkan) found on this machine");
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3)
        gpu_fatal("the Vulkan loader is older than 1.3");

    vkEnumerateInstanceLayerProperties(&layer_count, layers);
    for (i = 0; i < layer_count; ++i)
        validation |= desc->enable_validation && !strcmp(layers[i].layerName, VALIDATION_LAYER);
    /* a missing layer is a note, not a failure: release machines do not have it */
    if (desc->enable_validation && !validation)
        fprintf(stderr, "vk: note: %s is not installed, running without validation\n", VALIDATION_LAYER);

    extension_count = 1024;
    vkEnumerateInstanceExtensionProperties(NULL, &extension_count, extensions);
    for (i = 0; i < MU_ARRAY_COUNT(INSTANCE_EXTENSIONS); ++i)
        for (j = 0; j < extension_count; ++j)
            if (!strcmp(extensions[j].extensionName, INSTANCE_EXTENSIONS[i]))
            {
                enabled_extensions[enabled_count++]  = INSTANCE_EXTENSIONS[i];
                g->debug_utils                      |= i == 0;
            }
    {
        VkValidationFeatureEnableEXT feature_enable = desc->enable_debug_printf ? VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT
                                                                                : VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT;
        bool                         wants_features = validation && (desc->enable_debug_printf || desc->enable_gpu_based_validation);
        VkValidationFeaturesEXT      features       = {
            .sType                         = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
            .enabledValidationFeatureCount = 1,
            .pEnabledValidationFeatures    = &feature_enable,
        };
        VkDebugUtilsMessengerCreateInfoEXT messenger = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext           = wants_features ? &features : NULL,
            /* debug printf arrives at INFO severity */
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                               | (desc->enable_debug_printf ? VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT : 0),
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = debug_callback,
            .pUserData       = g,
        };
        VkApplicationInfo app = {
            .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = desc->name ? desc->name : "mu",
            .pEngineName      = "mu",
            .apiVersion       = VK_API_VERSION_1_3,
        };
        VkInstanceCreateInfo info = {
            .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext                   = validation && g->debug_utils ? &messenger : NULL,
            .pApplicationInfo        = &app,
            .enabledLayerCount       = validation,
            .ppEnabledLayerNames     = &VALIDATION_LAYER,
            .enabledExtensionCount   = enabled_count,
            .ppEnabledExtensionNames = enabled_extensions,
        };

        if (wants_features)
            enabled_extensions[info.enabledExtensionCount++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
        VK_CHECK(vkCreateInstance(&info, NULL, &g->instance));
        volkLoadInstance(g->instance);
        if (validation && g->debug_utils)
            VK_CHECK(vkCreateDebugUtilsMessengerEXT(g->instance, &messenger, NULL, &g->messenger));
    }

    /* ---- physical device ------------------------------------------------ */
    vkEnumeratePhysicalDevices(g->instance, &device_count, devices);
    memset(&chosen, 0, sizeof chosen);
    chosen.score = -1;
    for (i = 0; i < device_count; ++i)
    {
        memset(&candidate, 0, sizeof candidate);
        candidate.device       = devices[i];
        candidate.queue_family = UINT32_MAX;
        candidate.props        = (VkPhysicalDeviceProperties2){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &candidate.props11 };
        candidate.props11      = (VkPhysicalDeviceVulkan11Properties){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &candidate.props12 };
        candidate.props12      = (VkPhysicalDeviceVulkan12Properties){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
        candidate.features     = (VkPhysicalDeviceFeatures2){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &candidate.v11 };
        candidate.v11          = (VkPhysicalDeviceVulkan11Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &candidate.v12 };
        candidate.v12          = (VkPhysicalDeviceVulkan12Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &candidate.v13 };
        candidate.v13          = (VkPhysicalDeviceVulkan13Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        candidate.eds3         = (VkPhysicalDeviceExtendedDynamicState3FeaturesEXT){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT };

        /* the 1.0 query first: the 1.2/1.3 structs are only legal on a 1.3 device */
        vkGetPhysicalDeviceProperties(devices[i], &candidate.props.properties);
        if (candidate.props.properties.apiVersion >= VK_API_VERSION_1_3)
        {
            extension_count = 1024;
            vkEnumerateDeviceExtensionProperties(devices[i], NULL, &extension_count, extensions);
            for (j = 0; j < extension_count; ++j)
            {
                candidate.swapchain_ext |= !strcmp(extensions[j].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                candidate.eds3_ext      |= !strcmp(extensions[j].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
            }
            candidate.v13.pNext = candidate.eds3_ext ? &candidate.eds3 : NULL;
            vkGetPhysicalDeviceProperties2(devices[i], &candidate.props);
            vkGetPhysicalDeviceFeatures2(devices[i], &candidate.features);

            family_count = 16;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families);
            for (j = family_count; j-- > 0;) /* downwards, so the lowest matching family wins */
                if ((families[j].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                {
                    candidate.queue_family   = j;
                    candidate.timestamp_bits = families[j].timestampValidBits;
                }
        }

        /* the header's contract: unsupported devices are rejected, by name */
        missing = candidate.props.properties.apiVersion < VK_API_VERSION_1_3                    ? "Vulkan 1.3"
                  : candidate.queue_family == UINT32_MAX                                        ? "a graphics+compute queue"
                  : !candidate.v13.dynamicRendering                                             ? "dynamicRendering"
                  : !candidate.v13.synchronization2                                             ? "synchronization2"
                  : !candidate.v12.timelineSemaphore                                            ? "timelineSemaphore"
                  : !candidate.v12.bufferDeviceAddress                                          ? "bufferDeviceAddress"
                  : !candidate.v12.runtimeDescriptorArray                                       ? "runtimeDescriptorArray"
                  : !candidate.v12.descriptorBindingPartiallyBound                              ? "descriptorBindingPartiallyBound"
                  : !candidate.v12.descriptorBindingSampledImageUpdateAfterBind                 ? "descriptorBindingSampledImageUpdateAfterBind"
                  : !candidate.v12.descriptorBindingStorageImageUpdateAfterBind                 ? "descriptorBindingStorageImageUpdateAfterBind"
                  : !candidate.v12.descriptorBindingUpdateUnusedWhilePending                    ? "descriptorBindingUpdateUnusedWhilePending"
                  : !candidate.v12.shaderSampledImageArrayNonUniformIndexing                    ? "shaderSampledImageArrayNonUniformIndexing"
                  : !candidate.v11.shaderDrawParameters                                         ? "shaderDrawParameters"
                  : candidate.props.properties.limits.maxPushConstantsSize < MAX_PUSH_CONSTANTS ? "256 bytes of push constants"
                                                                                                : NULL;
        if (missing)
        {
            j = (uint32_t)strlen(rejected);
            snprintf(rejected + j, sizeof rejected - j, "\n  %s: no %s", candidate.props.properties.deviceName, missing);
            continue;
        }
        candidate.score = TYPE_RANK[MU_MIN((uint32_t)candidate.props.properties.deviceType, 4u)];
        if (candidate.score > chosen.score)
            chosen = candidate;
    }
    if (!chosen.device)
        gpu_fatal("no usable Vulkan device%s", rejected[0] ? rejected : " (none enumerated)");
    MU_FREE(extensions);

    g->physical      = chosen.device;
    g->queue_family  = chosen.queue_family;
    g->swapchain_ext = chosen.swapchain_ext;
    g->anisotropy    = chosen.features.features.samplerAnisotropy;
    g->statistics    = desc->enable_pipeline_stats && chosen.features.features.pipelineStatisticsQuery;
    g->timestamps    = chosen.timestamp_bits && chosen.props.properties.limits.timestampPeriod > 0.0f;
    g->profile       = desc->enable_graphics_profiler && g->timestamps;
    g->ms_per_tick   = (double)chosen.props.properties.limits.timestampPeriod * 1e-6;
    g->eds3          = chosen.eds3_ext && chosen.eds3.extendedDynamicState3RasterizationSamples && chosen.eds3.extendedDynamicState3ColorBlendEnable
                       && chosen.eds3.extendedDynamicState3ColorBlendEquation && chosen.eds3.extendedDynamicState3ColorWriteMask && !getenv("MU_NO_EDS3");
    /* what a VkPipeline has to bake in here: topology class always, the rest
     * only when extended dynamic state 3 is missing */
    g->static_state = (DrawState){
        .samples    = g->eds3 ? 0 : 0xff,
        .blend      = g->eds3 ? 0 : 0xff,
        .topology   = 0xff,
        .write_mask = g->eds3 ? 0 : 0xffff,
    };
    vkGetPhysicalDeviceMemoryProperties(g->physical, &g->memory);

    /* ---- device --------------------------------------------------------- */
    {
        const VkPhysicalDeviceFeatures *have = &chosen.features.features;
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {
            .sType                                     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
            .extendedDynamicState3RasterizationSamples = VK_TRUE,
            .extendedDynamicState3ColorBlendEnable     = VK_TRUE,
            .extendedDynamicState3ColorBlendEquation   = VK_TRUE,
            .extendedDynamicState3ColorWriteMask       = VK_TRUE,
        };
        VkPhysicalDeviceVulkan13Features v13 = {
            .sType                          = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext                          = g->eds3 ? &eds3 : NULL,
            .dynamicRendering               = VK_TRUE,
            .synchronization2               = VK_TRUE,
            .maintenance4                   = chosen.v13.maintenance4,
            .shaderDemoteToHelperInvocation = chosen.v13.shaderDemoteToHelperInvocation,
            .shaderTerminateInvocation      = chosen.v13.shaderTerminateInvocation,
            .subgroupSizeControl            = chosen.v13.subgroupSizeControl,
            .computeFullSubgroups           = chosen.v13.computeFullSubgroups,
            .shaderIntegerDotProduct        = chosen.v13.shaderIntegerDotProduct,
        };
        VkPhysicalDeviceVulkan12Features v12 = {
            .sType                                        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext                                        = &v13,
            .timelineSemaphore                            = VK_TRUE,
            .bufferDeviceAddress                          = VK_TRUE,
            .runtimeDescriptorArray                       = VK_TRUE,
            .descriptorBindingPartiallyBound              = VK_TRUE,
            .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
            .descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
            .descriptorBindingUpdateUnusedWhilePending    = VK_TRUE,
            .shaderSampledImageArrayNonUniformIndexing    = VK_TRUE,
            .shaderStorageImageArrayNonUniformIndexing    = chosen.v12.shaderStorageImageArrayNonUniformIndexing,
            .descriptorIndexing                           = chosen.v12.descriptorIndexing,
            .drawIndirectCount                            = chosen.v12.drawIndirectCount,
            .scalarBlockLayout                            = chosen.v12.scalarBlockLayout,
            .hostQueryReset                               = chosen.v12.hostQueryReset,
            .shaderFloat16                                = chosen.v12.shaderFloat16,
            .shaderInt8                                   = chosen.v12.shaderInt8,
            .shaderOutputLayer                            = chosen.v12.shaderOutputLayer,
            .shaderOutputViewportIndex                    = chosen.v12.shaderOutputViewportIndex,
            .uniformBufferStandardLayout                  = chosen.v12.uniformBufferStandardLayout,
            .shaderSubgroupExtendedTypes                  = chosen.v12.shaderSubgroupExtendedTypes,
        };
        VkPhysicalDeviceVulkan11Features v11 = {
            .sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            .pNext                = &v12,
            .shaderDrawParameters = VK_TRUE,
            .multiview            = chosen.v11.multiview,
        };
        VkPhysicalDeviceFeatures2 features = {
            .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext    = &v11,
            .features = {
                .samplerAnisotropy                    = have->samplerAnisotropy,
                .multiDrawIndirect                    = have->multiDrawIndirect,
                .drawIndirectFirstInstance            = have->drawIndirectFirstInstance,
                .independentBlend                     = have->independentBlend,
                .sampleRateShading                    = have->sampleRateShading,
                .depthClamp                           = have->depthClamp,
                .depthBiasClamp                       = have->depthBiasClamp,
                .fillModeNonSolid                     = have->fillModeNonSolid,
                .wideLines                            = have->wideLines,
                .largePoints                          = have->largePoints,
                .imageCubeArray                       = have->imageCubeArray,
                .pipelineStatisticsQuery              = have->pipelineStatisticsQuery,
                .shaderInt64                          = have->shaderInt64,
                .shaderInt16                          = have->shaderInt16,
                .fragmentStoresAndAtomics             = have->fragmentStoresAndAtomics,
                .vertexPipelineStoresAndAtomics       = have->vertexPipelineStoresAndAtomics,
                .shaderStorageImageExtendedFormats    = have->shaderStorageImageExtendedFormats,
                .shaderStorageImageReadWithoutFormat  = have->shaderStorageImageReadWithoutFormat,
                .shaderStorageImageWriteWithoutFormat = have->shaderStorageImageWriteWithoutFormat,
                .shaderImageGatherExtended            = have->shaderImageGatherExtended,
                .textureCompressionBC                 = have->textureCompressionBC,
            },
        };
        float                   priority   = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = g->queue_family,
            .queueCount       = 1,
            .pQueuePriorities = &priority,
        };
        const char        *device_extensions[2];
        VkDeviceCreateInfo info = {
            .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext                   = &features,
            .queueCreateInfoCount    = 1,
            .pQueueCreateInfos       = &queue_info,
            .ppEnabledExtensionNames = device_extensions,
        };

        if (g->swapchain_ext)
            device_extensions[info.enabledExtensionCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        if (g->eds3)
            device_extensions[info.enabledExtensionCount++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
        VK_CHECK(vkCreateDevice(g->physical, &info, NULL, &g->device));
        volkLoadDevice(g->device);
        vkGetDeviceQueue(g->device, g->queue_family, 0, &g->queue);
    }

    /* ---- what the device is --------------------------------------------- */
    sampled_count = desc->bindless_sampled_image_count ? desc->bindless_sampled_image_count : MAX_BINDLESS_TEXTURES;
    sampler_count = desc->bindless_sampler_count ? desc->bindless_sampler_count : MAX_BINDLESS_SAMPLERS;
    storage_count = desc->bindless_storage_image_count ? desc->bindless_storage_image_count : MAX_BINDLESS_STORAGE;
    sampled_count = MU_MIN(sampled_count, MU_MIN(chosen.props12.maxDescriptorSetUpdateAfterBindSampledImages, chosen.props12.maxPerStageDescriptorUpdateAfterBindSampledImages));
    sampler_count = MU_MIN(sampler_count, MU_MIN(chosen.props12.maxDescriptorSetUpdateAfterBindSamplers, chosen.props12.maxPerStageDescriptorUpdateAfterBindSamplers));
    sampler_count = MU_MIN(sampler_count, (uint32_t)MAX_BINDLESS_SAMPLERS); /* the sampler table is the descriptor array */
    storage_count = MU_MIN(storage_count, MU_MIN(chosen.props12.maxDescriptorSetUpdateAfterBindStorageImages, chosen.props12.maxPerStageDescriptorUpdateAfterBindStorageImages));

    snprintf(g->info.name, sizeof g->info.name, "%.63s", chosen.props.properties.deviceName);
    g->info.width                               = g->desc.width;
    g->info.height                              = g->desc.height;
    g->info.subgroup_size                       = chosen.props11.subgroupSize;
    g->info.max_compute_workgroup_invocations   = chosen.props.properties.limits.maxComputeWorkGroupInvocations;
    g->info.max_compute_shared_memory_size      = chosen.props.properties.limits.maxComputeSharedMemorySize;
    g->info.min_storage_buffer_offset_alignment = (uint32_t)chosen.props.properties.limits.minStorageBufferOffsetAlignment;
    g->info.non_coherent_atom_size              = chosen.props.properties.limits.nonCoherentAtomSize;
    g->info.max_push_constants_size             = chosen.props.properties.limits.maxPushConstantsSize;
    g->info.max_sampled_images                  = sampled_count;
    g->info.max_storage_images                  = storage_count;
    g->info.max_samplers                        = sampler_count;
    g->info.sample_counts                       = chosen.props.properties.limits.framebufferColorSampleCounts
                                                  & chosen.props.properties.limits.framebufferDepthSampleCounts & (SC_1 | SC_2 | SC_4 | SC_8);
    g->info.buffer_device_address               = true;
    g->info.descriptor_indexing                 = true;
    g->info.synchronization2                    = true;
    g->info.dynamic_rendering                   = true;
    g->info.draw_indirect_count                 = chosen.v12.drawIndirectCount;
    for (i = 0; i < g->memory.memoryHeapCount; ++i)
    {
        bool device_local = g->memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        bool visible      = false;

        /* "visible" VRAM is a device-local heap the CPU can map directly */
        for (j = 0; j < g->memory.memoryTypeCount; ++j)
            visible |= g->memory.memoryTypes[j].heapIndex == i && (g->memory.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        g->info.device_memory         += device_local ? g->memory.memoryHeaps[i].size : 0;
        g->info.visible_device_memory += device_local && visible ? g->memory.memoryHeaps[i].size : 0;
    }

    /* D24S8 does not exist on every GPU (AMD has none); D32F+S8 always does */
    memcpy(g->formats, FORMAT_TABLE, sizeof g->formats);
    {
        VkFormatProperties properties;

        vkGetPhysicalDeviceFormatProperties(g->physical, VK_FORMAT_D24_UNORM_S8_UINT, &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            g->formats[FMT_D24S8].vk = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    /* ---- the pipeline cache, with what earlier runs compiled ------------- */
    {
        const VkPhysicalDeviceProperties *props      = &chosen.props.properties;
        const char                       *name       = desc->name && desc->name[0] ? desc->name : "mu";
        const char                       *xdg        = getenv("XDG_CACHE_HOME");
        const char                       *home       = getenv("HOME");
        bool                              use_xdg    = xdg && xdg[0] == '/'; /* XDG: a relative path there does not count */
        const char                       *root       = use_xdg ? xdg : home && home[0] == '/' ? home : NULL;
        VkPipelineCacheCreateInfo         cache_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        PipelineFile                      kept       = { 0 };
        void                             *bytes      = NULL;
        uint64_t                          byte_count = 0;
        char                              folder[64];
        size_t                            length;

        g->pipeline_file = (PipelineFile){
            .magic          = PIPELINE_MAGIC,
            .header_bytes   = sizeof(PipelineFile),
            .pointer_bytes  = sizeof(void *),
            .vendor         = props->vendorID,
            .device         = props->deviceID,
            .driver_version = props->driverVersion,
        };
        memcpy(g->pipeline_file.uuid, props->pipelineCacheUUID, VK_UUID_SIZE);

        /* <cache>/<name>/pipelines-<vendor>-<device>.bin: one file per program
         * and device. The name becomes a folder, so only letters, digits, '-',
         * '_' and bytes of UTF-8 pass as they are. */
        for (i = 0; name[i] && i < sizeof folder - 1; ++i)
            folder[i] = isalnum((unsigned char)name[i]) || (unsigned char)name[i] >= 0x80 || name[i] == '-' || name[i] == '_' ? name[i] : '_';
        folder[i] = 0;
        if (root && strlen(root) < PIPELINE_ROOT_MAX)
        {
            length = (size_t)snprintf(g->pipeline_path, sizeof g->pipeline_path, "%s%s/%s", root, use_xdg ? "" : "/.cache", folder);
            mu_files_mkdirs(g->pipeline_path); /* if that fails so does the write in gpu_destroy, and it says so */
            snprintf(g->pipeline_path + length, sizeof g->pipeline_path - length, "/pipelines-%x-%x.bin", props->vendorID, props->deviceID);
        }
        else
            fprintf(stderr, "vk: note: neither XDG_CACHE_HOME nor HOME is a usable directory: compiled pipelines are not kept between runs\n");

        /* A file that is not there reads as no bytes, which is a first run. */
        mu_files_read_all(g->pipeline_path, &bytes, &byte_count);
        if (byte_count >= sizeof kept)
            memcpy(&kept, bytes, sizeof kept);
        g->pipeline_added = true; /* until a file turns out to hold everything already */
        if (!memcmp(&kept, &g->pipeline_file, offsetof(PipelineFile, size)) && kept.size == byte_count - sizeof kept
            && kept.hash == mu_wyhash64((uint8_t *)bytes + sizeof kept, (size_t)kept.size, 0))
        {
            g->pipeline_added          = false;
            g->pipeline_file           = kept;
            cache_info.initialDataSize = (size_t)kept.size;
            cache_info.pInitialData    = (uint8_t *)bytes + sizeof kept;
        }
        else if (byte_count)
            fprintf(stderr, "vk: note: %s is from another driver or device, or is damaged: starting without it\n", g->pipeline_path);
        VK_CHECK(vkCreatePipelineCache(g->device, &cache_info, NULL, &g->pipeline_cache));
        MU_FREE(bytes);
    }

    /* ---- timeline, command pools, frames -------------------------------- */
    {
        VkSemaphoreTypeCreateInfo timeline_type = {
            .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        };
        VkSemaphoreCreateInfo   timeline_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timeline_type };
        VkSemaphoreCreateInfo   binary_info   = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkCommandPoolCreateInfo pool_info     = {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = g->queue_family,
        };
        VkQueryPoolCreateInfo timestamp_info = {
            .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType  = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = QUERIES_PER_FRAME,
        };
        VkQueryPoolCreateInfo statistics_info = {
            .sType              = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType          = VK_QUERY_TYPE_PIPELINE_STATISTICS,
            .queryCount         = 1,
            /* STATISTIC_COUNT bits, read back in this (ascending bit) order */
            .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
                                  | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT
                                  | VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT,
        };

        VK_CHECK(vkCreateSemaphore(g->device, &timeline_info, NULL, &g->timeline));
        VK_CHECK(vkCreateCommandPool(g->device, &pool_info, NULL, &g->batch_pool));
        for (i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            Frame                      *f          = &g->frames[i];
            VkCommandBufferAllocateInfo alloc_info = {
                .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };

            VK_CHECK(vkCreateCommandPool(g->device, &pool_info, NULL, &f->pool));
            alloc_info.commandPool = f->pool;
            VK_CHECK(vkAllocateCommandBuffers(g->device, &alloc_info, &f->cmd));
            VK_CHECK(vkCreateSemaphore(g->device, &binary_info, NULL, &f->acquired));
            if (g->timestamps)
                VK_CHECK(vkCreateQueryPool(g->device, &timestamp_info, NULL, &f->timestamps));
            if (g->statistics)
                VK_CHECK(vkCreateQueryPool(g->device, &statistics_info, NULL, &f->statistics));
        }
    }

    /* ---- the one descriptor set ----------------------------------------- */
    {
        VkDescriptorBindingFlags live_array = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                                              | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        /* arrays change while command buffers that use the set are in flight;
         * the storage buffer is written once, below, and never again */
        VkDescriptorBindingFlags     binding_flags[4] = { live_array, live_array, live_array, 0 };
        VkDescriptorSetLayoutBinding bindings[4]      = {
            [BIND_SAMPLED_IMAGES] = { BIND_SAMPLED_IMAGES, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled_count, STAGES_PUSH, NULL },
            [BIND_SAMPLERS]       = { BIND_SAMPLERS, VK_DESCRIPTOR_TYPE_SAMPLER, sampler_count, STAGES_PUSH, NULL },
            [BIND_STORAGE_IMAGES] = { BIND_STORAGE_IMAGES, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage_count, STAGES_PUSH, NULL },
            [BIND_STORAGE_BUFFER] = { BIND_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, STAGES_PUSH, NULL },
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount  = 4,
            .pBindingFlags = binding_flags,
        };
        VkDescriptorSetLayoutCreateInfo layout_info = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext        = &flags_info,
            .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = 4,
            .pBindings    = bindings,
        };
        VkDescriptorPoolSize pool_sizes[4] = {
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampled_count },
            { VK_DESCRIPTOR_TYPE_SAMPLER, sampler_count },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage_count },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
        };
        VkDescriptorPoolCreateInfo pool_info = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
            .maxSets       = 1,
            .poolSizeCount = 4,
            .pPoolSizes    = pool_sizes,
        };
        VkDescriptorSetAllocateInfo set_info = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorSetCount = 1,
            .pSetLayouts        = &g->set_layout,
        };
        VkPushConstantRange        push_range       = { STAGES_PUSH, 0, MAX_PUSH_CONSTANTS };
        VkPipelineLayoutCreateInfo pipe_layout_info = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &g->set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &push_range,
        };

        VK_CHECK(vkCreateDescriptorSetLayout(g->device, &layout_info, NULL, &g->set_layout));
        VK_CHECK(vkCreateDescriptorPool(g->device, &pool_info, NULL, &g->set_pool));
        set_info.descriptorPool = g->set_pool;
        VK_CHECK(vkAllocateDescriptorSets(g->device, &set_info, &g->set));
        VK_CHECK(vkCreatePipelineLayout(g->device, &pipe_layout_info, NULL, &g->layout));
    }

    /* ---- tables: slot 0 of each is the nil handle and holds the dummy ---- */
    mu_id_pool_init(&g->buffer_ids, MAX_BUFFERS);
    mu_id_pool_init(&g->image_ids, MAX_IMAGES);
    mu_id_pool_init(&g->sampled_ids, sampled_count);
    mu_id_pool_init(&g->storage_ids, storage_count);
    mu_id_pool_create_id(&g->buffer_ids, &i); /* buffer 0 stays empty: a nil buffer has no bytes */
    for (i = 1; i < MAX_BUFFERS; ++i)
        g->buffers[i].gen = 1;
    for (i = 1; i < MAX_IMAGES; ++i)
        g->images[i].gen = 1;
    for (i = 1; i < MAX_BINDLESS_SAMPLERS; ++i)
        g->samplers[i].gen = 1;
    for (i = 1; i < MAX_PIPELINES; ++i)
        g->pipes[i].gen = 1;

    /* ---- arenas, and binding 3 ------------------------------------------ */
    {
        static const char *const ARENA_NAMES[3] = { "arena device", "arena transient", "arena staging" };
        VkDeviceSize             sizes[3]       = {
            [ARENA_DEVICE]    = desc->size_of_gpu_pool ? desc->size_of_gpu_pool : MU_MB(64),
            [ARENA_TRANSIENT] = desc->size_of_cpu_pool ? desc->size_of_cpu_pool : MU_MB(16),
            [ARENA_STAGING]   = desc->size_of_staging_pool ? desc->size_of_staging_pool : MU_MB(32),
        };
        VkDescriptorBufferInfo buffer_info = { .range = VK_WHOLE_SIZE };
        VkWriteDescriptorSet   write       = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g->set,
            .dstBinding      = BIND_STORAGE_BUFFER,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &buffer_info,
        };

        for (i = 0; i < 3; ++i)
        {
            /* a whole number of granules per frame slice: the transient pool
             * divides into slices and the staging ring wraps, both on
             * boundaries that keep every alignment arena_alloc accepts */
            g->arena_stats[i].reserved = MU_CEIL(sizes[i], (VkDeviceSize)POOL_GRANULE * MAX_FRAMES_IN_FLIGHT) * POOL_GRANULE * MAX_FRAMES_IN_FLIGHT;
            g->arena_buffers[i]        = buffer_create(g, &(BufferDesc){
                .name         = ARENA_NAMES[i],
                .size         = g->arena_stats[i].reserved,
                .usage        = BUF_DEVICE | BUF_INDIRECT | BUF_VERTEX | BUF_INDEX | BUF_XFER_SRC | BUF_XFER_DST,
                .host_visible = i != ARENA_DEVICE,
            });
        }
        buffer_info.buffer = buffer_slot(g, g->arena_buffers[ARENA_DEVICE])->buffer;
        vkUpdateDescriptorSets(g->device, 1, &write, 0, NULL);
    }

    /* ---- dummies at index 0, then the headless frame image --------------- */
    {
        static const uint8_t WHITE[4] = { 255, 255, 255, 255 };
        Upload              *upload;
        Image                dummy;

        sampler_linear_clamp(g); /* the table is empty, so this lands in slot 0 */
        dummy = image_create(g, &(ImageDesc){
            .name   = "dummy white",
            .width  = 1,
            .height = 1,
            .format = FMT_RGBA8_UNORM,
            .usage  = IMG_SAMPLED | IMG_STORAGE | IMG_XFER_DST,
        });
        MU_ASSERT(dummy.index == 0 && image_slot(g, dummy)->sampled_id == 0 && image_slot(g, dummy)->storage_id == 0);
        upload = upload_begin(g);
        upload_image(upload, SUB(dummy), BYTE_SPAN(WHITE));
        upload_end(g, upload);

        g->headless = image_create(g, &(ImageDesc){
            .name   = "frame (headless)",
            .width  = g->desc.width,
            .height = g->desc.height,
            .format = FMT_RGBA8_UNORM,
            .usage  = IMG_ATTACH | IMG_SAMPLED | IMG_STORAGE | IMG_XFER_SRC | IMG_XFER_DST,
        });
    }
    return g;
}

void gpu_destroy(Gpu *g)
{
    PipelineFile fresh       = g->pipeline_file;
    size_t       cache_bytes = 0;
    uint8_t     *file;
    uint32_t     i, j;

    gpu_wait_idle(g);
    surface_drop(g);

    /* ---- keep what was compiled for the next run ------------------------ */
    /* A run in which the cache already held every pipeline leaves the file
     * alone. Each pipeline says so as it is created (CACHE_HIT); the bytes do
     * not, because a driver may return the same pipelines in another order.
     * A driver that reports no hits has its bytes compared instead. The write
     * goes to a temporary file that is then renamed over the old one, so
     * neither a crash nor a second instance of the program can leave half a
     * file behind. */
    if (g->pipeline_path[0] && g->pipeline_added)
    {
        VK_CHECK(vkGetPipelineCacheData(g->device, g->pipeline_cache, &cache_bytes, NULL));
        file = MU_MALLOC(sizeof fresh + cache_bytes);
        VK_CHECK(vkGetPipelineCacheData(g->device, g->pipeline_cache, &cache_bytes, file + sizeof fresh));
        fresh.size = cache_bytes;
        fresh.hash = mu_wyhash64(file + sizeof fresh, cache_bytes, 0);
        memcpy(file, &fresh, sizeof fresh);
        if (memcmp(&fresh, &g->pipeline_file, sizeof fresh) && !mu_files_write_atomic(g->pipeline_path, file, sizeof fresh + cache_bytes))
            fprintf(stderr, "vk: note: cannot write %s: what this run compiled is not kept\n", g->pipeline_path);
        MU_FREE(file);
    }

    /* the GPU is idle: everything queued for retirement can go now */
    for (i = 0; i < MAX_RETIRED; ++i)
        g->retired[i].value = 0;
    timeline_poll(g);

    /* Vulkan destroy calls accept null, so the tables are swept without tests */
    for (i = 0; i < MAX_IMAGES; ++i)
    {
        for (j = 0; j < g->images[i].view_count; ++j)
            vkDestroyImageView(g->device, g->images[i].views[j].view, NULL);
        vkDestroyImage(g->device, g->images[i].image, NULL);
        vkFreeMemory(g->device, g->images[i].memory, NULL);
    }
    for (i = 0; i < MAX_BUFFERS; ++i)
    {
        vkDestroyBuffer(g->device, g->buffers[i].buffer, NULL);
        vkFreeMemory(g->device, g->buffers[i].memory, NULL);
    }
    for (i = 0; i < MAX_BINDLESS_SAMPLERS; ++i)
        vkDestroySampler(g->device, g->samplers[i].sampler, NULL);
    for (i = 0; i < MAX_SHADERS; ++i)
        vkDestroyShaderModule(g->device, g->shaders[i].module, NULL);
    for (i = 0; i < MAX_PIPELINES; ++i)
        vkDestroyPipeline(g->device, g->pipes[i].compute, NULL);
    for (i = 0; i < g->variant_count; ++i)
        vkDestroyPipeline(g->device, g->variants[i].pipeline, NULL);
    for (i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        vkDestroyQueryPool(g->device, g->frames[i].timestamps, NULL);
        vkDestroyQueryPool(g->device, g->frames[i].statistics, NULL);
        vkDestroySemaphore(g->device, g->frames[i].acquired, NULL);
        vkDestroyCommandPool(g->device, g->frames[i].pool, NULL);
    }
    vkDestroyCommandPool(g->device, g->batch_pool, NULL);
    vkDestroyPipelineCache(g->device, g->pipeline_cache, NULL);
    vkDestroyPipelineLayout(g->device, g->layout, NULL);
    vkDestroyDescriptorPool(g->device, g->set_pool, NULL);
    vkDestroyDescriptorSetLayout(g->device, g->set_layout, NULL);
    vkDestroySemaphore(g->device, g->timeline, NULL);
    vkDestroyDevice(g->device, NULL);
    if (g->messenger)
        vkDestroyDebugUtilsMessengerEXT(g->instance, g->messenger, NULL);
    vkDestroyInstance(g->instance, NULL);

    mu_id_pool_destroy_all(&g->buffer_ids);
    mu_id_pool_destroy_all(&g->image_ids);
    mu_id_pool_destroy_all(&g->sampled_ids);
    mu_id_pool_destroy_all(&g->storage_ids);
    mu_id_pool_deinit(&g->buffer_ids);
    mu_id_pool_deinit(&g->image_ids);
    mu_id_pool_deinit(&g->sampled_ids);
    mu_id_pool_deinit(&g->storage_ids);
    MU_FREE(g->buffers);
    MU_FREE(g->images);
    MU_FREE(g->retired);
    MU_FREE(g);
}

GpuInfo gpu_info(const Gpu *g)
{
    return g->info;
}

VkInstance gpu_raw_instance(const Gpu *g)
{
    return g->instance;
}

VkDevice gpu_raw_device(Gpu *g)
{
    return g->device;
}

const char *gpu_error(const Gpu *g)
{
    return g->error;
}

void gpu_wait_idle(Gpu *g)
{
    uint32_t i;

    flush(g);
    timeline_wait(g, g->submitted);
    /* oldest slot first, so gpu_stats() ends on the newest finished frame */
    for (i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        frame_stats_resolve(g, &g->frames[(g->frame_index + i) % MAX_FRAMES_IN_FLIGHT]);
}

/* ============================================================================
 * 7. BUFFERS AND ARENAS
 * ==========================================================================*/

/* Dedicated allocation. Takes the lowest memory type that has `required` and
 * `preferred`, or failing that the lowest with `required` alone. */
static VkDeviceMemory memory_allocate(Gpu *g, VkMemoryRequirements requirements, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, bool device_address)
{
    VkMemoryAllocateFlagsInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = device_address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0,
    };
    VkMemoryAllocateInfo info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &flags_info,
        .allocationSize  = requirements.size,
        .memoryTypeIndex = UINT32_MAX,
    };
    VkMemoryPropertyFlags wanted[2] = { required | preferred, required };
    VkDeviceMemory        memory;
    uint32_t              pass, i;

    /* loosest pass first and indices downwards: the last assignment is the
     * lowest index of the strictest pass that matched anything */
    for (pass = 2; pass-- > 0;)
        for (i = g->memory.memoryTypeCount; i-- > 0;)
            if ((requirements.memoryTypeBits & (1u << i)) && (g->memory.memoryTypes[i].propertyFlags & wanted[pass]) == wanted[pass])
                info.memoryTypeIndex = i;
    if (info.memoryTypeIndex == UINT32_MAX)
        gpu_fatal("no memory type offers properties 0x%x for a %llu byte allocation", required, (unsigned long long)requirements.size);
    VK_CHECK(vkAllocateMemory(g->device, &info, NULL, &memory));
    return memory;
}

Buffer buffer_create(Gpu *g, const BufferDesc *desc)
{
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = desc->size,
        /* every buffer has a device address and can be uploaded to and read back;
         * those usages cost nothing and remove a class of "forgot the flag" errors */
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                 | VK_BUFFER_USAGE_TRANSFER_DST_BIT | (desc->usage & BUF_INDIRECT ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT : 0)
                 | (desc->usage & BUF_VERTEX ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : 0) | (desc->usage & BUF_INDEX ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : 0),
    };
    VkBufferDeviceAddressInfo address_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    VkMemoryRequirements      requirements;
    BufferSlot               *b;
    uint32_t                  index;

    MU_ASSERT(desc->size);
    if (!mu_id_pool_create_id(&g->buffer_ids, &index))
        gpu_fatal("all %d buffer slots are in use", MAX_BUFFERS);
    b = &g->buffers[index];

    VK_CHECK(vkCreateBuffer(g->device, &info, NULL, &b->buffer));
    vkGetBufferMemoryRequirements(g->device, b->buffer, &requirements);
    /* host memory is read back as well as written, so it should be cached:
     * reading a write-combined mapping is two orders of magnitude slower */
    b->memory = desc->host_visible ? memory_allocate(g, requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true)
                                   : memory_allocate(g, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true);
    VK_CHECK(vkBindBufferMemory(g->device, b->buffer, b->memory, 0));
    if (desc->host_visible)
        VK_CHECK(vkMapMemory(g->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped));

    address_info.buffer = b->buffer;
    b->address          = vkGetBufferDeviceAddress(g->device, &address_info);
    b->size             = desc->size;
    name_object(g, VK_OBJECT_TYPE_BUFFER, (uint64_t)b->buffer, desc->name);
    return (Buffer){ index, b->gen };
}

Buffer buffer_device(Gpu *g, uint64_t size)
{
    return buffer_create(g, &(BufferDesc){ .size = size, .usage = BUF_DEVICE | BUF_INDIRECT | BUF_XFER_SRC | BUF_XFER_DST });
}

Buffer buffer_host(Gpu *g, uint64_t size)
{
    return buffer_create(g, &(BufferDesc){ .size = size, .usage = BUF_DEVICE | BUF_INDIRECT | BUF_XFER_SRC | BUF_XFER_DST, .host_visible = true });
}

void buffer_destroy(Gpu *g, Buffer b)
{
    BufferSlot *slot = buffer_slot(g, b);
    Retired    *r;

    if (!HANDLE_VALID(b)) /* like free(NULL) */
        return;
    r         = retire(g);
    r->buffer = slot->buffer;
    r->memory = slot->memory;
    *slot     = (BufferSlot){ .gen = slot->gen + 1 };
    mu_id_pool_destroy_id(&g->buffer_ids, b.index);
}

BufferInfo buffer_info(const Gpu *g, Buffer b)
{
    BufferSlot *slot = buffer_slot(g, b);

    return (BufferInfo){ slot->address, slot->size, slot->mapped };
}

Range arena_alloc(Gpu *g, Arena arena, uint64_t size, uint64_t alignment)
{
    static const char *const POOL_FIELDS[3] = { "size_of_gpu_pool", "size_of_cpu_pool", "size_of_staging_pool" };
    ArenaStats              *stats          = &g->arena_stats[arena];
    uint64_t                 align          = alignment ? alignment : 16;
    uint64_t                 total          = stats->reserved;
    uint64_t                 slice          = total / MAX_FRAMES_IN_FLIGHT;
    uint64_t                 capacity, base, offset, position;

    MU_ASSERT(MU_IS_POW2(align) && align <= POOL_GRANULE);
    stats->allocations++;

    if (arena == ARENA_STAGING)
    {
        if (size > total)
            gpu_fatal("%llu bytes do not fit the %llu byte staging pool; raise GpuDesc.%s", (unsigned long long)size, (unsigned long long)total, POOL_FIELDS[arena]);
        for (;;)
        {
            offset    = MU_ALIGN_UP(g->staging_head, align);
            position  = offset % total;
            offset   += position + size > total ? total - position : 0; /* a range never wraps: skip the tail of the ring */
            if (offset + size - g->staging_tail <= total)
                break;

            /* The ring is full of bytes the GPU has not consumed. Outside a
             * frame the open batch can go out now; then wait for the oldest
             * submit that still holds staging bytes. Never a queue-wide wait. */
            flush(g);
            if (!g->staging_mark_count)
                gpu_fatal("one frame's uploads exceed the %llu byte staging pool; raise GpuDesc.%s", (unsigned long long)total, POOL_FIELDS[arena]);
            timeline_wait(g, g->staging_marks[g->staging_mark_head].value);
        }
        g->staging_head = offset + size;
        stats->used     = g->staging_head - g->staging_tail;
        stats->peak     = MU_MAX(stats->peak, stats->used);
        return (Range){ g->arena_buffers[arena], offset % total, size };
    }

    /* the device arena bumps forever; the transient arena bumps inside the
     * slice of the frame being recorded and frame_begin() rewinds it */
    capacity = arena == ARENA_TRANSIENT ? slice : total;
    base     = arena == ARENA_TRANSIENT ? slice * g->frame_index : 0;
    offset   = MU_ALIGN_UP(stats->used, align);
    if (offset + size > capacity)
        gpu_fatal("arena %d is full (%llu of %llu bytes used, %llu more asked); raise GpuDesc.%s", (int)arena, (unsigned long long)stats->used,
                  (unsigned long long)capacity, (unsigned long long)size, POOL_FIELDS[arena]);
    stats->used = offset + size;
    stats->peak = MU_MAX(stats->peak, stats->used);
    return (Range){ g->arena_buffers[arena], base + offset, size };
}

Range arena_alias(Gpu *g, Arena arena, Range previous, uint64_t size, uint64_t alignment)
{
    uint64_t align = alignment ? alignment : 16;
    /* The earlier range is handed out again when it can hold the new one. The
     * caller is saying the two are never live at the same time. */
    bool fits = previous.buffer.index == g->arena_buffers[arena].index && previous.size >= size && !(previous.offset & (align - 1));

    if (!fits)
        return arena_alloc(g, arena, size, alignment);
    return (Range){ previous.buffer, previous.offset, size };
}

ArenaStats arena_stats(const Gpu *g, Arena arena)
{
    return g->arena_stats[arena];
}

/* ============================================================================
 * 8. IMAGES AND SAMPLERS
 * ==========================================================================*/

/* Bindless arrays are written here and nowhere else: when a view or a sampler
 * gets its index, and again for a view that images_follow_frame() makes anew. */
static void descriptor_write(Gpu *g, uint32_t binding, VkDescriptorType type, uint32_t index, VkDescriptorImageInfo image)
{
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = g->set,
        .dstBinding      = binding,
        .dstArrayElement = index,
        .descriptorCount = 1,
        .descriptorType  = type,
        .pImageInfo      = &image,
    };

    vkUpdateDescriptorSets(g->device, 1, &write, 0, NULL);
    g->recording->descriptor_writes++;
}

/* The Vulkan view behind one entry of an image's views. */
static void view_make(Gpu *g, const ImageSlot *s, ImageView *v)
{
    VkImageViewCreateInfo info = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = s->image,
        .viewType         = s->depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : v->layer_count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format           = s->format.vk,
        .subresourceRange = { v->aspect, v->mip, v->mip_count, v->layer, v->layer_count },
    };

    VK_CHECK(vkCreateImageView(g->device, &info, NULL, &v->view));
}

/* The view of one sub-range, created on first use and kept with the image.
 * Entry 0 is always the whole image. */
static ImageView *image_view(Gpu *g, ImageSlot *s, ImageSub sub, VkImageAspectFlags aspect)
{
    uint16_t   mip_count   = MU_MAX(sub.mip_count, 1);
    uint16_t   layer_count = MU_MAX(sub.layer_count, 1);
    ImageView *v;
    uint32_t   i;

    /* an image that follows the frame can have fewer mips than it had a frame ago */
    MU_ASSERT((uint32_t)sub.mip + mip_count <= s->mips && (uint32_t)sub.layer + layer_count <= s->layers);
    for (i = 0; i < s->view_count; ++i)
    {
        v = &s->views[i];
        if (v->mip == sub.mip && v->mip_count == mip_count && v->layer == sub.layer && v->layer_count == layer_count && v->aspect == aspect)
            return v;
    }
    if (s->view_count == MAX_IMAGE_VIEWS)
        gpu_fatal("one image is used through more than %d different sub-ranges", MAX_IMAGE_VIEWS);
    v  = &s->views[s->view_count++];
    *v = (ImageView){ .mip = sub.mip, .mip_count = mip_count, .layer = sub.layer, .layer_count = layer_count, .aspect = aspect };
    view_make(g, s, v);
    return v;
}

/* A view that is sampled through gets a descriptor of its own on first use.
 * image_create() asks for the whole image's: that one is image_sampled_id(). */
uint32_t image_sub_sampled_id(Gpu *g, ImageSub sub)
{
    ImageSlot *s            = image_slot(g, sub.image);
    bool       depth_format = s->format.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
    ImageView *v;

    MU_ASSERT((s->usage & IMG_SAMPLED) && !sub.stencil_only);
    /* a depth-stencil image is sampled through its depth aspect alone */
    v = image_view(g, s, sub, depth_format ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    if (v->sampled_id)
        return v->sampled_id;
    if (!mu_id_pool_create_id(&g->sampled_ids, &v->sampled_id))
        gpu_fatal("all %u sampled image descriptors are in use; raise GpuDesc.bindless_sampled_image_count", g->info.max_sampled_images);
    descriptor_write(g, BIND_SAMPLED_IMAGES, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, v->sampled_id, (VkDescriptorImageInfo){
        .imageView   = v->view,
        .imageLayout = depth_format ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    });
    return v->sampled_id;
}

/* The same for a view that is written or read as a storage image. Such an
 * access reaches the first mip of the view and no other, which is why a mip
 * below the top needs a view, and an index, of its own. */
uint32_t image_sub_storage_id(Gpu *g, ImageSub sub)
{
    ImageSlot *s = image_slot(g, sub.image);
    ImageView *v;

    MU_ASSERT((s->usage & IMG_STORAGE) && !sub.stencil_only);
    v = image_view(g, s, sub, s->format.aspect);
    if (v->storage_id)
        return v->storage_id;
    if (!mu_id_pool_create_id(&g->storage_ids, &v->storage_id))
        gpu_fatal("all %u storage image descriptors are in use; raise GpuDesc.bindless_storage_image_count", g->info.max_storage_images);
    descriptor_write(g, BIND_STORAGE_IMAGES, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, v->storage_id, (VkDescriptorImageInfo){
        .imageView   = v->view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    });
    return v->storage_id;
}

/* The Vulkan image and its memory, from the description the slot keeps.
 * image_create() does this once. An image that follows the frame has it done
 * again whenever the frame has another size. */
static void image_build(Gpu *g, ImageSlot *s)
{
    const ImageDesc  *desc         = &s->desc;
    bool              depth_format = s->format.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t          width        = s->follows ? MU_MAX(g->info.width >> desc->frame_shift, 1u) : desc->width;
    uint32_t          height       = s->follows ? MU_MAX(g->info.height >> desc->frame_shift, 1u) : desc->height;
    VkImageCreateInfo info         = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = desc->depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format      = s->format.vk,
        .extent      = { width, height, MU_MAX(desc->depth, 1u) },
        /* a mip chain ends at 1x1: of the mips asked for, an image that follows the frame has the ones its size has */
        .mipLevels   = MU_MAX(s->follows ? MU_MIN(desc->mips, 32u - mu_leading_zeroes_u32(MU_MAX(width, height))) : desc->mips, 1u),
        .arrayLayers = MU_MAX(desc->layers, 1u),
        .samples     = MU_MAX((uint32_t)desc->samples, 1u),
        .tiling      = VK_IMAGE_TILING_OPTIMAL,
        .usage       = (desc->usage & IMG_SAMPLED ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) | (desc->usage & IMG_STORAGE ? VK_IMAGE_USAGE_STORAGE_BIT : 0)
                       | (desc->usage & IMG_XFER_SRC ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) | (desc->usage & IMG_XFER_DST ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0)
                       | (desc->usage & IMG_ATTACH ? (depth_format ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) : 0)
                       | (desc->transient ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT : 0),
    };
    VkImageFormatProperties limits = { 0 };
    VkMemoryRequirements    requirements;

    /* What this device cannot make is said here, in the caller's terms. A query
     * that fails leaves the limits at zero, which no image fits. */
    vkGetPhysicalDeviceImageFormatProperties(g->physical, info.format, info.imageType, info.tiling, info.usage, info.flags, &limits);
    if ((limits.sampleCounts & info.samples) != (VkSampleCountFlags)info.samples || info.extent.width > limits.maxExtent.width || info.extent.height > limits.maxExtent.height
        || info.extent.depth > limits.maxExtent.depth || info.mipLevels > limits.maxMipLevels || info.arrayLayers > limits.maxArrayLayers)
        gpu_fatal("image \"%s\" (%ux%ux%u, %u mips, %u layers, %ux MSAA, Format %d, usage 0x%x) is beyond this device: "
                  "it allows up to %ux%ux%u, %u mips, %u layers and the sample counts in bit mask 0x%x for that format and usage",
                  s->name, info.extent.width, info.extent.height, info.extent.depth, info.mipLevels, info.arrayLayers, (uint32_t)info.samples,
                  (int)desc->format, (uint32_t)desc->usage, limits.maxExtent.width, limits.maxExtent.height, limits.maxExtent.depth, limits.maxMipLevels,
                  limits.maxArrayLayers, (uint32_t)limits.sampleCounts);
    VK_CHECK(vkCreateImage(g->device, &info, NULL, &s->image));
    vkGetImageMemoryRequirements(g->device, s->image, &requirements);
    /* a transient attachment never leaves the tile on GPUs that can keep it there */
    s->memory = memory_allocate(g, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, desc->transient ? VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT : 0, false);
    VK_CHECK(vkBindImageMemory(g->device, s->image, s->memory, 0));
    s->width   = width;
    s->height  = height;
    s->depth   = info.extent.depth;
    s->layers  = info.arrayLayers;
    s->mips    = info.mipLevels;
    s->samples = info.samples;
    s->usage   = desc->usage;
    name_object(g, VK_OBJECT_TYPE_IMAGE, (uint64_t)s->image, desc->name);
}

Image image_create(Gpu *g, const ImageDesc *desc)
{
    ImageSub   whole;
    ImageSlot *s;
    uint32_t   index;

    /* a width and a height, or neither: then the image is as large as the frame, halved frame_shift times */
    MU_ASSERT(g->formats[desc->format].vk && desc->mips <= MAX_MIPS && !desc->width == !desc->height && desc->frame_shift < 32 && !(desc->width && desc->frame_shift));
    if (!mu_id_pool_create_id(&g->image_ids, &index))
        gpu_fatal("all %d image slots are in use", MAX_IMAGES);
    s          = &g->images[index];
    s->format  = g->formats[desc->format];
    s->desc    = *desc;
    s->follows = !desc->width;
    /* the caller's string need not outlive this call, and building the image again names it again */
    snprintf(s->name, sizeof s->name, "%.47s", desc->name ? desc->name : "");
    s->desc.name = desc->name ? s->name : NULL;
    image_build(g, s);

    /* entry 0 is the whole image. Attachments use it, and its indices are the image's own: image_sampled_id(), image_storage_id() */
    whole = (ImageSub){ .image = { index, s->gen }, .mip_count = (uint16_t)s->mips, .layer_count = (uint16_t)s->layers };
    image_view(g, s, whole, s->format.aspect);
    if (desc->usage & IMG_SAMPLED)
        s->sampled_id = image_sub_sampled_id(g, whole);
    if (desc->usage & IMG_STORAGE)
        s->storage_id = image_sub_storage_id(g, whole);
    return whole.image;
}

/* The frame has another size: every image that follows it is built again in
 * its slot, so that its handle and its shader indices stay what they were. A
 * view of a mip the new size does not have goes, and its indices with it.
 * Every caller has waited for the queue and nothing is being recorded, so
 * nothing reads through the descriptors that are rewritten here. */
static void images_follow_frame(Gpu *g)
{
    ImageSlot *s;
    ImageView *v;
    Retired   *r;
    bool       depth_format;
    uint32_t   i, j, kept;

    MU_ASSERT(!g->cmd);
    for (i = 1; i < MAX_IMAGES; ++i)
    {
        s = &g->images[i];
        if (!s->follows || (s->width == MU_MAX(g->info.width >> s->desc.frame_shift, 1u) && s->height == MU_MAX(g->info.height >> s->desc.frame_shift, 1u)))
            continue;
        depth_format = s->format.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
        /* the old image goes the way a destroyed one does, except that the indices stay with the slot */
        r             = retire(g);
        r->image      = s->image;
        r->memory     = s->memory;
        r->view_count = s->view_count;
        for (j = 0; j < s->view_count; ++j)
            r->views[j] = s->views[j].view;
        image_build(g, s);
        memset(s->sync, 0, sizeof s->sync);        /* nothing has touched the new image */
        s->views[0].mip_count = (uint16_t)s->mips; /* entry 0 is the whole image, whatever that is now */
        for (j = 0, kept = 0; j < s->view_count; ++j)
        {
            v = &s->views[j];
            if ((uint32_t)v->mip + v->mip_count > s->mips)
            {
                r->sampled_ids[j] = v->sampled_id;
                r->storage_ids[j] = v->storage_id;
                continue;
            }
            view_make(g, s, v);
            /* what image_sub_sampled_id() and image_sub_storage_id() wrote, for the view that replaces theirs */
            if (v->sampled_id)
                descriptor_write(g, BIND_SAMPLED_IMAGES, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, v->sampled_id, (VkDescriptorImageInfo){
                    .imageView   = v->view,
                    .imageLayout = depth_format ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                });
            if (v->storage_id)
                descriptor_write(g, BIND_STORAGE_IMAGES, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, v->storage_id, (VkDescriptorImageInfo){
                    .imageView   = v->view,
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                });
            s->views[kept++] = *v;
        }
        memset(&s->views[kept], 0, (s->view_count - kept) * sizeof *v);
        s->view_count = kept;
    }
}

Image image_color(Gpu *g, uint32_t w, uint32_t h, Format fmt)
{
    return image_create(g, &(ImageDesc){ .width = w, .height = h, .format = fmt, .usage = IMG_ATTACH | IMG_SAMPLED | IMG_XFER_SRC });
}

Image image_storage(Gpu *g, uint32_t w, uint32_t h, Format fmt)
{
    return image_create(g, &(ImageDesc){ .width = w, .height = h, .format = fmt, .usage = IMG_STORAGE | IMG_SAMPLED | IMG_XFER_SRC });
}

Image image_depth(Gpu *g, uint32_t w, uint32_t h)
{
    return image_create(g, &(ImageDesc){ .width = w, .height = h, .format = FMT_D32F, .usage = IMG_ATTACH | IMG_SAMPLED });
}

Image image_from_bytes(Gpu *g, uint32_t w, uint32_t h, Format fmt, const void *data, uint64_t size)
{
    Image   img = image_create(g, &(ImageDesc){ .width = w, .height = h, .format = fmt, .usage = IMG_SAMPLED | IMG_XFER_DST });
    Upload *u   = upload_begin(g);

    upload_image(u, SUB(img), (DataSpan){ data, (uint32_t)size });
    upload_end(g, u);
    return img;
}

void image_destroy(Gpu *g, Image img)
{
    ImageSlot *s = image_slot(g, img);
    Retired   *r;
    uint32_t   i;

    if (!HANDLE_VALID(img)) /* like free(NULL) */
        return;
    MU_ASSERT(s->memory); /* the frame image belongs to the swapchain */
    r             = retire(g);
    r->image      = s->image;
    r->memory     = s->memory;
    r->view_count = s->view_count;
    for (i = 0; i < s->view_count; ++i)
    {
        r->views[i]       = s->views[i].view;
        r->sampled_ids[i] = s->views[i].sampled_id;
        r->storage_ids[i] = s->views[i].storage_id;
    }
    *s = (ImageSlot){ .gen = s->gen + 1 };
    mu_id_pool_destroy_id(&g->image_ids, img.index);
}

uint32_t image_sampled_id(const Gpu *g, Image img)
{
    return image_slot(g, img)->sampled_id;
}

uint32_t image_storage_id(const Gpu *g, Image img)
{
    return image_slot(g, img)->storage_id;
}

Sampler sampler_create(Gpu *g, const SamplerDesc *desc)
{
    static const VkFilter             FILTERS[]   = { [FILT_NEAREST] = VK_FILTER_NEAREST, [FILT_LINEAR] = VK_FILTER_LINEAR };
    static const VkSamplerMipmapMode  MIP_MODES[] = { [FILT_NEAREST] = VK_SAMPLER_MIPMAP_MODE_NEAREST, [FILT_LINEAR] = VK_SAMPLER_MIPMAP_MODE_LINEAR };
    static const VkSamplerAddressMode ADDRESSES[] = {
        [ADDR_REPEAT]       = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        [ADDR_CLAMP]        = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        [ADDR_MIRROR]       = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        [ADDR_CLAMP_BORDER] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
    };
    static const VkCompareOp COMPARES[] = {
        [CMP_NONE]   = VK_COMPARE_OP_ALWAYS,
        [CMP_NEVER]  = VK_COMPARE_OP_NEVER,
        [CMP_LESS]   = VK_COMPARE_OP_LESS,
        [CMP_EQUAL]  = VK_COMPARE_OP_EQUAL,
        [CMP_LEQUAL] = VK_COMPARE_OP_LESS_OR_EQUAL,
        [CMP_GREATER] = VK_COMPARE_OP_GREATER,
        [CMP_GEQUAL] = VK_COMPARE_OP_GREATER_OR_EQUAL,
        [CMP_ALWAYS] = VK_COMPARE_OP_ALWAYS,
    };
    VkSamplerCreateInfo info = {
        .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter        = FILTERS[desc->mag_filter],
        .minFilter        = FILTERS[desc->min_filter],
        .mipmapMode       = MIP_MODES[desc->mip_filter],
        .addressModeU     = ADDRESSES[desc->addr_u],
        .addressModeV     = ADDRESSES[desc->addr_v],
        .addressModeW     = ADDRESSES[desc->addr_w],
        .mipLodBias       = desc->lod_bias,
        .anisotropyEnable = g->anisotropy && desc->max_aniso > 1.0f,
        .maxAnisotropy    = MU_MIN(desc->max_aniso, 16.0f),
        .compareEnable    = desc->compare != CMP_NONE,
        .compareOp        = COMPARES[desc->compare],
        .minLod           = desc->min_lod,
        .maxLod           = desc->max_lod,
        /* outside a shadow map is lit; outside anything else is nothing */
        .borderColor      = desc->compare != CMP_NONE ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    /* slot 0 is the default sampler behind SAMPLER_NIL; gpu_create fills it
     * with the first call and user samplers start at 1 */
    uint32_t     first = g->samplers[0].sampler ? 1 : 0;
    uint32_t     index = UINT32_MAX;
    uint32_t     i;
    SamplerSlot *s;

    /* same description, same sampler. Downwards, so `index` ends on the lowest free slot. */
    for (i = g->info.max_samplers; i-- > first;)
    {
        s = &g->samplers[i];
        if (s->refs && !memcmp(&s->desc, desc, sizeof *desc))
        {
            s->refs++;
            return (Sampler){ i, s->gen };
        }
        if (!s->refs && !s->sampler)
            index = i;
    }
    if (index == UINT32_MAX)
        gpu_fatal("all %u sampler descriptors are in use", g->info.max_samplers);

    s       = &g->samplers[index];
    s->desc = *desc;
    s->refs = 1;
    VK_CHECK(vkCreateSampler(g->device, &info, NULL, &s->sampler));
    descriptor_write(g, BIND_SAMPLERS, VK_DESCRIPTOR_TYPE_SAMPLER, index, (VkDescriptorImageInfo){ .sampler = s->sampler });
    return (Sampler){ index, s->gen };
}

void sampler_destroy(Gpu *g, Sampler sampler)
{
    SamplerSlot *s = &g->samplers[sampler.index % MAX_BINDLESS_SAMPLERS];
    Retired     *r;

    MU_ASSERT(sampler.index < MAX_BINDLESS_SAMPLERS && s->gen == sampler.gen);
    if (!HANDLE_VALID(sampler) || --s->refs)
        return;
    /* the VkSampler stays in the slot until the timeline frees both */
    r               = retire(g);
    r->sampler      = s->sampler;
    r->sampler_slot = sampler.index;
    s->gen++;
}

/* The table slot is the descriptor index. */
uint32_t sampler_id(const Gpu *g, Sampler sampler)
{
    const SamplerSlot *s = &g->samplers[sampler.index % MAX_BINDLESS_SAMPLERS];

    MU_ASSERT(sampler.index < MAX_BINDLESS_SAMPLERS && s->gen == sampler.gen);
    return (uint32_t)(s - g->samplers);
}

/* ============================================================================
 * 9. SHADERS AND PIPELINES
 * ==========================================================================*/

/* FNV-1a, continued across calls. */
static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    size_t         i;

    for (i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

/* A path ending in .spv is loaded as it is. Anything else is Slang source: it
 * is compiled with slangc ($SLANGC, or `slangc` from PATH) into a .spv next to
 * it, and recompiled when any .slang in its directory is newer than that
 * file. A machine without slangc still runs from the .spv files that are
 * already there. MU_SHADER_REBUILD=1 forces the compile. */
ShaderId shader_load(Gpu *g, const ShaderDesc *desc)
{
    static const VkShaderStageFlagBits STAGE_BITS[] = {
        [STAGE_VERTEX]   = VK_SHADER_STAGE_VERTEX_BIT,
        [STAGE_FRAGMENT] = VK_SHADER_STAGE_FRAGMENT_BIT,
        [STAGE_COMPUTE]  = VK_SHADER_STAGE_COMPUTE_BIT,
        [STAGE_TASK]     = VK_SHADER_STAGE_TASK_BIT_EXT,
        [STAGE_MESH]     = VK_SHADER_STAGE_MESH_BIT_EXT,
    };
    static const char *const STAGE_NAMES[] = {
        [STAGE_VERTEX] = "vertex", [STAGE_FRAGMENT] = "fragment", [STAGE_COMPUTE] = "compute", [STAGE_TASK] = "amplification", [STAGE_MESH] = "mesh",
    };
    const char              *entry       = desc->entry ? desc->entry : "main";
    const char              *compiler    = getenv("SLANGC") ? getenv("SLANGC") : "slangc";
    const char              *extension   = strrchr(desc->path, '.');
    const char              *slash       = strrchr(desc->path, '/');
    bool                     precompiled = extension && !strcmp(extension, ".spv");
    int                      stem        = extension ? (int)(extension - desc->path) : (int)strlen(desc->path);
    uint64_t                 key         = 0xcbf29ce484222325ull, defines_key = 0xcbf29ce484222325ull;
    uint64_t                 source_time = 0, spv_time;
    VkShaderModuleCreateInfo info        = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    char                     spv_path[1024], dir_path[512], file_path[1024], command[4096];
    struct stat              st;
    struct dirent           *item;
    DIR                     *dir;
    FILE                    *file;
    uint32_t                *words;
    ShaderSlot              *s;
    long                     bytes;
    int                      length, status, code;
    uint32_t                 i;

    for (i = 0; i < desc->define_count; ++i)
        defines_key = hash_bytes(defines_key, desc->defines[i], strlen(desc->defines[i]) + 1);
    key = hash_bytes(key, desc->path, strlen(desc->path) + 1);
    key = hash_bytes(key, entry, strlen(entry) + 1);
    key = hash_bytes(key, &desc->stage, sizeof desc->stage);
    key = hash_bytes(key, &defines_key, sizeof defines_key);

    /* same source, entry, stage and defines: same shader */
    for (i = 1; i <= g->shader_count; ++i)
        if (g->shaders[i].key == key)
            return (ShaderId)i;
    if (g->shader_count + 1 == MAX_SHADERS)
        gpu_fatal("all %d shader slots are in use", MAX_SHADERS);

    if (precompiled)
        snprintf(spv_path, sizeof spv_path, "%s", desc->path);
    else if (desc->define_count)
        snprintf(spv_path, sizeof spv_path, "%.*s.%s.%08x.spv", stem, desc->path, entry, (uint32_t)defines_key);
    else
        snprintf(spv_path, sizeof spv_path, "%.*s.%s.spv", stem, desc->path, entry);

    /* the newest .slang beside the source, so an edited #include rebuilds too */
    snprintf(dir_path, sizeof dir_path, "%.*s", slash ? (int)(slash - desc->path) : 1, slash ? desc->path : ".");
    dir = precompiled ? NULL : opendir(dir_path);
    while (dir && (item = readdir(dir)))
    {
        extension = strrchr(item->d_name, '.');
        snprintf(file_path, sizeof file_path, "%s/%s", dir_path, item->d_name);
        if (extension && !strcmp(extension, ".slang") && !stat(file_path, &st))
            source_time = MU_MAX(source_time, (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec);
    }
    if (dir)
        closedir(dir);
    spv_time = stat(spv_path, &st) ? 0 : (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;

    /* a compiler already known to be missing is not asked again while a .spv exists */
    if (source_time && (spv_time < source_time || getenv("MU_SHADER_REBUILD")) && !(g->compiler_missing && spv_time))
    {
        /* column-major matrices match mu_math and mul(M, v); the entry point
         * keeps its own name so one .slang can hold many stages */
        length = snprintf(command, sizeof command, "\"%s\" \"%s\" -target spirv -entry %s -stage %s -O2 -matrix-layout-column-major -fvk-use-entrypoint-name -o \"%s\"",
                          compiler, desc->path, entry, STAGE_NAMES[desc->stage], spv_path);
        for (i = 0; i < desc->define_count && length < (int)sizeof command; ++i)
            length += snprintf(command + length, sizeof command - (size_t)length, " -D%s", desc->defines[i]);
        status = system(command);
        code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        /* 127 is the shell saying there is no compiler; a .spv that already
         * exists is then the best there is. Said once, not once per shader. */
        if (code == 127 && spv_time)
        {
            fprintf(stderr, "vk: note: %s not found: shaders run from the .spv files already beside them, edits to .slang are not picked up\n", compiler);
            g->compiler_missing = true;
        }
        else if (code != 0)
            gpu_fatal("%s could not compile %s:%s (exit code %d)%s", compiler, desc->path, entry, code,
                      code == 127 ? "; install Slang or set SLANGC to the slangc binary" : "");
    }

    file = fopen(spv_path, "rb");
    if (!file)
        gpu_fatal("cannot open shader %s (from %s:%s)", spv_path, desc->path, entry);
    fseek(file, 0, SEEK_END);
    bytes = ftell(file);
    fseek(file, 0, SEEK_SET);
    words = MU_MALLOC((size_t)MU_MAX(bytes, 4l));
    if (fread(words, 1, (size_t)bytes, file) != (size_t)bytes || bytes < 20 || words[0] != 0x07230203u)
        gpu_fatal("%s is not a SPIR-V module", spv_path);
    fclose(file);

    s             = &g->shaders[++g->shader_count];
    s->key        = key;
    s->stage      = STAGE_BITS[desc->stage];
    info.codeSize = (size_t)bytes;
    info.pCode    = words;
    snprintf(s->entry, sizeof s->entry, "%s", entry);
    VK_CHECK(vkCreateShaderModule(g->device, &info, NULL, &s->module));
    name_object(g, VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)s->module, spv_path);
    MU_FREE(words);
    return (ShaderId)g->shader_count;
}

Pipe pipe_create(Gpu *g, const PipeDesc *desc)
{
    VkPipelineCreationFeedback           feedback[2]   = { { 0 } }; /* the pipeline, then its one stage */
    VkPipelineCreationFeedbackCreateInfo feedback_info = {
        .sType                              = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
        .pPipelineCreationFeedback          = &feedback[0],
        .pipelineStageCreationFeedbackCount = 1,
        .pPipelineStageCreationFeedbacks    = &feedback[1],
    };
    VkComputePipelineCreateInfo info = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext  = &feedback_info,
        .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT },
        .layout = g->layout,
    };
    PipeKey   key;
    PipeSlot *slot;
    uint32_t  index = 0;
    uint32_t  i;

    /* an entry point the description leaves out is the one the shader was loaded with */
    memset(&key, 0, sizeof key);
    key.vertex    = desc->vertex;
    key.fragment  = desc->fragment;
    key.compute   = desc->compute;
    key.view_mask = desc->view_mask;
    snprintf(key.entries[0], sizeof key.entries[0], "%s", desc->vertex_entry ? desc->vertex_entry : g->shaders[desc->vertex].entry);
    snprintf(key.entries[1], sizeof key.entries[1], "%s", desc->fragment_entry ? desc->fragment_entry : g->shaders[desc->fragment].entry);
    snprintf(key.entries[2], sizeof key.entries[2], "%s", desc->compute_entry ? desc->compute_entry : g->shaders[desc->compute].entry);

    /* same description, same Pipe. Downwards, so `index` ends on the lowest free slot. */
    for (i = MAX_PIPELINES; i-- > 1;)
    {
        slot = &g->pipes[i];
        if (slot->refs && !memcmp(&slot->key, &key, sizeof key))
        {
            slot->refs++;
            return (Pipe){ i, slot->gen };
        }
        if (!slot->refs)
            index = i;
    }
    if (!index)
        gpu_fatal("all %d pipe slots are in use", MAX_PIPELINES);

    slot       = &g->pipes[index];
    slot->key  = key;
    slot->refs = 1;
    snprintf(slot->name, sizeof slot->name, "%.31s", desc->name ? desc->name : key.entries[desc->compute ? 2 : 0]);

    /* A compute pipeline depends on nothing but its shader, so it exists now.
     * A graphics pipeline also depends on the attachments it meets: pass_apply
     * builds it on first use. */
    if (desc->compute)
    {
        info.stage.module = g->shaders[desc->compute].module;
        info.stage.pName  = slot->key.entries[2];
        VK_CHECK(vkCreateComputePipelines(g->device, g->pipeline_cache, 1, &info, NULL, &slot->compute));
        g->pipeline_added |= (feedback[0].flags & CACHE_HIT) != CACHE_HIT; /* one more for gpu_destroy to keep */
        name_object(g, VK_OBJECT_TYPE_PIPELINE, (uint64_t)slot->compute, slot->name);
    }
    return (Pipe){ index, slot->gen };
}

void pipe_destroy(Gpu *g, Pipe p)
{
    PipeSlot *slot = &g->pipes[p.index % MAX_PIPELINES];
    uint32_t  i;

    MU_ASSERT(p.index < MAX_PIPELINES && slot->gen == p.gen);
    if (!HANDLE_VALID(p) || --slot->refs)
        return;
    retire(g)->pipeline = slot->compute;
    /* its VkPipelines go with it; the last variant fills each hole, and going
     * downwards means the one moved in has already been looked at */
    for (i = g->variant_count; i-- > 0;)
        if (g->variants[i].key.pipe == p.index)
        {
            retire(g)->pipeline = g->variants[i].pipeline;
            g->variants[i]      = g->variants[--g->variant_count];
        }
    *slot = (PipeSlot){ .gen = slot->gen + 1 };
}

/* The presets are reverse-Z: near is 1, far is 0, clear depth to 0.
 * write_mask is 4 bits (RGBA) per colour attachment for attachments 0..3;
 * attachments 4..7 are always written. */
DrawState draw_state_opaque(uint8_t samples, uint8_t cull)
{
    return (DrawState){
        .samples = samples, .blend = BLEND_OPAQUE, .depth_compare = CMP_GREATER, .depth_write = 1, .cull = cull, .topology = TOPO_TRI, .write_mask = 0xffff,
    };
}

DrawState draw_state_alpha(uint8_t samples, uint8_t cull)
{
    return (DrawState){
        .samples = samples, .blend = BLEND_ALPHA, .depth_compare = CMP_GREATER, .depth_write = 1, .cull = cull, .topology = TOPO_TRI, .write_mask = 0xffff,
    };
}

/* Depth only: no colour is written even when colour attachments are bound. */
DrawState draw_state_depth(uint8_t samples)
{
    return (DrawState){
        .samples = samples, .blend = BLEND_OPAQUE, .depth_compare = CMP_GREATER, .depth_write = 1, .cull = CULL_BACK, .topology = TOPO_TRI, .write_mask = 0,
    };
}

/* ============================================================================
 * 10. SWAPCHAIN AND FRAME
 * ==========================================================================*/

/* The swapchain goes, and everything made for its images goes with it.
 * Whatever used them has to be over first, and that includes presents, which
 * are queue work the timeline does not number: this is the one place the
 * queue itself is waited on. The semaphores go last, once the swapchain their
 * presents went to no longer exists. */
static void swapchain_release(Gpu *g)
{
    ImageSlot *s;
    uint32_t   i, j;

    VK_CHECK(vkQueueWaitIdle(g->queue));
    for (i = 0; i < g->swap_count; ++i)
    {
        s = image_slot(g, g->swap_images[i]);
        for (j = 0; j < MAX_IMAGE_VIEWS; ++j)
            vkDestroyImageView(g->device, s->views[j].view, NULL);
        *s = (ImageSlot){ .gen = s->gen + 1 };
        mu_id_pool_destroy_id(&g->image_ids, g->swap_images[i].index);
    }
    /* the KHR entry points only exist when the extension was enabled */
    if (g->swapchain)
        vkDestroySwapchainKHR(g->device, g->swapchain, NULL);
    for (i = 0; i < g->swap_count; ++i)
        vkDestroySemaphore(g->device, g->swap_rendered[i], NULL);
    g->swapchain  = VK_NULL_HANDLE;
    g->swap_count = 0;
}

static void surface_drop(Gpu *g)
{
    swapchain_release(g);
    if (g->surface)
        vkDestroySurfaceKHR(g->instance, g->surface, NULL);
    g->surface = VK_NULL_HANDLE;
}

/* Builds the swapchain for the surface's current size and releases the one
 * before it. A zero-area surface (minimised window) leaves no swapchain and
 * stays stale, so the next frame_begin tries again. */
static void swapchain_create(Gpu *g)
{
    static const VkFormat    PREFERRED[]    = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    VkSemaphoreCreateInfo    semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSwapchainKHR           fresh          = VK_NULL_HANDLE;
    VkPresentModeKHR         mode           = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t                 format_count = 64, mode_count = 16, image_count = 0;
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR       formats[64], format;
    VkPresentModeKHR         modes[16];
    VkImage                  images[MAX_SWAP_IMAGES];
    VkImageUsageFlags        usage;
    VkExtent2D               extent;
    ImageSlot               *s;
    uint32_t                 i, j, index;

    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->physical, g->surface, &caps));
    vkGetPhysicalDeviceSurfaceFormatsKHR(g->physical, g->surface, &format_count, formats);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g->physical, g->surface, &mode_count, modes);

    /* the frame image is 8-bit UNORM everywhere: shaders write display-encoded colour */
    format = formats[0];
    for (i = MU_ARRAY_COUNT(PREFERRED); i-- > 0;)
        for (j = 0; j < format_count; ++j)
            if (formats[j].format == PREFERRED[i] && formats[j].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                format = formats[j];
    /* FIFO always exists; without vsync take the lowest-latency mode on offer */
    for (i = 0; i < mode_count; ++i)
        if (!g->desc.vsync && modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    for (i = 0; i < mode_count; ++i)
        if (!g->desc.vsync && modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            mode = VK_PRESENT_MODE_MAILBOX_KHR;

    extent           = caps.currentExtent;
    g->swap_sizeless = extent.width == UINT32_MAX;
    if (g->swap_sizeless) /* the surface takes whatever size the swapchain has */
    {
        extent.width  = MU_CLAMP(g->window_width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = MU_CLAMP(g->window_height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    /* transfer usage lets readback_image() take screenshots of the frame image */
    usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));

    /* The new swapchain is made from the old one, which is how the window
     * system hands the surface over. Only after that is the old one released. */
    if (extent.width && extent.height)
    {
        VkSwapchainCreateInfoKHR info = {
            .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface          = g->surface,
            .minImageCount    = MU_MIN(caps.minImageCount + 1, caps.maxImageCount ? caps.maxImageCount : UINT32_MAX),
            .imageFormat      = format.format,
            .imageColorSpace  = format.colorSpace,
            .imageExtent      = extent,
            .imageArrayLayers = 1,
            .imageUsage       = usage,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform     = caps.currentTransform,
            .compositeAlpha   = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                    ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                    : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1)),
            .presentMode      = mode,
            .clipped          = VK_TRUE,
            .oldSwapchain     = g->swapchain,
        };

        VK_CHECK(vkCreateSwapchainKHR(g->device, &info, NULL, &fresh));
        image_count = MAX_SWAP_IMAGES; /* more than that is VK_INCOMPLETE, which is fatal */
        VK_CHECK(vkGetSwapchainImagesKHR(g->device, fresh, &image_count, images));
        g->swap_stale  = false;
        g->info.width  = extent.width;
        g->info.height = extent.height;
    }
    swapchain_release(g);
    g->swapchain  = fresh;
    g->swap_count = image_count;

    for (i = 0; i < image_count; ++i)
    {
        if (!mu_id_pool_create_id(&g->image_ids, &index))
            gpu_fatal("all %d image slots are in use", MAX_IMAGES);
        s          = &g->images[index];
        s->image   = images[i];
        s->format  = (FormatInfo){ format.format, VK_IMAGE_ASPECT_COLOR_BIT, 4, 0 };
        s->width   = extent.width;
        s->height  = extent.height;
        s->depth   = 1;
        s->layers  = 1;
        s->mips    = 1;
        s->samples = 1;
        s->usage   = IMG_ATTACH | (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT ? IMG_XFER_SRC : 0) | (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT ? IMG_XFER_DST : 0);
        image_view(g, s, (ImageSub){ .mip_count = 1, .layer_count = 1 }, VK_IMAGE_ASPECT_COLOR_BIT);
        g->swap_images[i] = (Image){ index, s->gen };
        /* the semaphore this image's presents wait on: see swap_rendered */
        VK_CHECK(vkCreateSemaphore(g->device, &semaphore_info, NULL, &g->swap_rendered[i]));
    }
    images_follow_frame(g); /* swapchain_release() has waited for the queue */
}

/* The surface becomes the backend's: it is destroyed with the Gpu, or when
 * another surface (or VK_NULL_HANDLE, back to headless) replaces it. */
bool gpu_set_surface(Gpu *g, VkSurfaceKHR surface)
{
    VkBool32 supported = VK_FALSE;

    MU_ASSERT(!g->in_frame);
    flush(g);
    timeline_wait(g, g->submitted);
    surface_drop(g);
    /* until a swapchain says otherwise the frame is the headless image */
    g->info.width  = g->desc.width;
    g->info.height = g->desc.height;
    images_follow_frame(g);
    if (!surface)
        return true;

    /* Not fatal: a window system can refuse a device, and the caller can
     * carry on headless. gpu_error() says why. */
    if (g->swapchain_ext)
        vkGetPhysicalDeviceSurfaceSupportKHR(g->physical, g->queue_family, surface, &supported);
    if (!supported)
    {
        snprintf(g->error, sizeof g->error, "%s cannot present to this surface%s", g->info.name, g->swapchain_ext ? "" : " (no VK_KHR_swapchain)");
        vkDestroySurfaceKHR(g->instance, surface, NULL);
        return false;
    }
    g->surface    = surface;
    g->swap_stale = true;
    swapchain_create(g);
    return true;
}

/* On X11 and Win32 the surface knows the window's size, and a swapchain of
 * another size is reported stale when it is used. A Wayland surface is as
 * large as the images presented to it: only the caller knows what the window
 * asks for, and only a change of that makes the swapchain stale. */
void gpu_resize(Gpu *g, uint32_t width, uint32_t height)
{
    g->swap_stale    = g->swap_stale || (g->swap_sizeless && (width != g->window_width || height != g->window_height));
    g->window_width  = width;
    g->window_height = height;
}

/* Writes a timestamp into the frame's next free queries and says which those
 * are. `views` is 1 outside a pass and Pass.views inside one. */
static uint16_t timestamp(Frame *f, VkCommandBuffer cmd, uint32_t views)
{
    uint32_t query = f->query_count;

    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f->timestamps, query);
    f->query_count += views;
    return (uint16_t)query;
}

bool frame_begin(Gpu *g)
{
    Frame   *f      = &g->frames[g->frame_index];
    VkResult result = VK_SUCCESS;

    MU_ASSERT(!g->in_frame && !g->upload.open && !g->pass.open);
    flush(g);                   /* whatever was recorded before the frame runs before it */
    timeline_wait(g, f->value); /* this slot's last frame has left the GPU: its pool and queries are free */
    frame_stats_resolve(g, f);

    /* An out-of-date swapchain is rebuilt and acquired again. A suboptimal one
     * still renders this frame and is rebuilt before the next. */
    while (g->surface)
    {
        if (g->swap_stale)
            swapchain_create(g);
        if (!g->swapchain)
            return false; /* minimised: nothing to draw into */
        result        = vkAcquireNextImageKHR(g->device, g->swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &g->swap_index);
        g->swap_stale = result != VK_SUCCESS;
        if (result != VK_ERROR_OUT_OF_DATE_KHR)
            break;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        gpu_fatal("vkAcquireNextImageKHR returned VkResult %d", (int)result);

    g->frame_target = g->swapchain ? g->swap_images[g->swap_index] : g->headless;
    /* An acquired image has undefined contents, and its first barrier has to
     * chain onto the stage the acquire semaphore is waited at. */
    if (g->swapchain)
        image_slot(g, g->frame_target)->sync[0] = (Sync){ .stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };

    VK_CHECK(vkResetCommandPool(g->device, f->pool, 0));
    memset(&f->stats, 0, sizeof f->stats);
    f->begin_ns                          = mu_clock_ns();
    f->profiled                          = g->profile;
    g->recording                         = &f->stats;
    g->in_frame                          = true;
    g->arena_stats[ARENA_TRANSIENT].used = 0;
    cmd_begin(g, f->cmd);
    if (f->profiled)
    {
        vkCmdResetQueryPool(f->cmd, f->timestamps, 0, QUERIES_PER_FRAME);
        f->query_count = 0;
        timestamp(f, f->cmd, 1);
    }
    if (g->statistics)
    {
        vkCmdResetQueryPool(f->cmd, f->statistics, 0, 1);
        vkCmdBeginQuery(f->cmd, f->statistics, 0, 0);
    }
    return true;
}

void frame_end(Gpu *g)
{
    Frame           *f        = &g->frames[g->frame_index];
    VkSemaphore      acquired = g->swapchain ? f->acquired : VK_NULL_HANDLE;
    /* the acquired image's own, not the frame slot's: see swap_rendered */
    VkSemaphore      rendered = g->swapchain ? g->swap_rendered[g->swap_index] : VK_NULL_HANDLE;
    VkPresentInfoKHR present  = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &rendered,
        .swapchainCount     = 1,
        .pSwapchains        = &g->swapchain,
        .pImageIndices      = &g->swap_index,
    };
    VkResult result;

    MU_ASSERT(g->in_frame && !g->pass.open && !g->upload.open);
    if (g->swapchain)
    {
        sync_image(g, SUB(g->frame_target), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0, false);
        barriers_flush(g, f->cmd);
    }
    if (g->statistics)
        vkCmdEndQuery(f->cmd, f->statistics, 0);
    if (f->profiled)
        timestamp(f, f->cmd, 1);

    f->stats.cpu_ms         = (double)(mu_clock_ns() - f->begin_ns) * 1e-6;
    f->stats.transient_peak = g->arena_stats[ARENA_TRANSIENT].used;
    f->value                = g->submitted + 1;
    f->pending              = true;
    g->recording            = &g->offline;
    g->in_frame             = false;
    g->frame_index          = (g->frame_index + 1) % MAX_FRAMES_IN_FLIGHT;
    cmd_submit(g, acquired, rendered); /* the frame's one submit */

    if (g->swapchain)
    {
        result        = vkQueuePresentKHR(g->queue, &present);
        g->swap_stale = g->swap_stale || result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
        if (result != VK_SUCCESS && !g->swap_stale)
            gpu_fatal("vkQueuePresentKHR returned VkResult %d", (int)result);
    }
}

Image frame_image(Gpu *g)
{
    MU_ASSERT(g->in_frame);
    return g->frame_target;
}

/* ============================================================================
 * 11. PASSES AND COMMANDS
 *     declaration -> barriers -> dynamic rendering -> pipeline
 * ==========================================================================*/

/* One attachment: its barrier and its VkRenderingAttachmentInfo. Colour and
 * depth differ only in layout, stage and access, which the caller passes in. */
static ImageSlot *attach(Pass *p, const Attach *a, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access, VkRenderingAttachmentInfo *info, VkExtent2D *area)
{
    static const VkAttachmentLoadOp  LOAD_OPS[]  = { [LOAD] = VK_ATTACHMENT_LOAD_OP_LOAD, [CLEAR] = VK_ATTACHMENT_LOAD_OP_CLEAR, [DONT_CARE] = VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    static const VkAttachmentStoreOp STORE_OPS[] = { [STORE] = VK_ATTACHMENT_STORE_OP_STORE, [STORE_DISCARD] = VK_ATTACHMENT_STORE_OP_DONT_CARE };
    Gpu                             *g           = p->g;
    ImageSlot                       *s           = image_slot(g, a->image.image);
    /* an attachment is exactly one mip */
    ImageSub target  = { .image = a->image.image, .mip = a->image.mip, .mip_count = 1, .layer = a->image.layer, .layer_count = a->image.layer_count };
    ImageSub resolve = { .image = a->resolve.image, .mip = a->resolve.mip, .mip_count = 1, .layer = a->resolve.layer, .layer_count = a->resolve.layer_count };
    bool     depth   = s->format.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t width   = MU_MAX(s->width >> target.mip, 1u);
    uint32_t height  = MU_MAX(s->height >> target.mip, 1u);
    uint32_t i;

    /* read-only is a promise that the pass writes nothing: no clear, no resolve */
    MU_ASSERT(!a->read_only || (a->load == LOAD && !HANDLE_VALID(a->resolve.image)));
    sync_image(g, target, layout, stages, access, !a->read_only);
    *info = (VkRenderingAttachmentInfo){
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = image_view(g, s, target, s->format.aspect)->view,
        .imageLayout = layout,
        .loadOp      = LOAD_OPS[a->load],
        /* what was only read has nothing to store, and a store would count as a write */
        .storeOp     = a->read_only ? VK_ATTACHMENT_STORE_OP_NONE : STORE_OPS[a->store],
    };
    /* the header's clear[4] is a colour, or depth followed by stencil */
    for (i = 0; i < 4; ++i)
        info->clearValue.color.float32[i] = a->clear[i];
    for (i = 0; i < 4 && s->format.integer; ++i)
        info->clearValue.color.uint32[i] = (uint32_t)a->clear[i];
    if (depth)
        info->clearValue.depthStencil = (VkClearDepthStencilValue){ a->clear[0], (uint32_t)a->clear[1] };

    if (HANDLE_VALID(a->resolve.image))
    {
        sync_image(g, resolve, layout, stages, access, true);
        /* resolve_mode is a VkResolveModeFlagBits; 0 picks the one that is
         * always legal: average for colour, sample zero for depth and integers */
        info->resolveMode        = a->resolve_mode ? (VkResolveModeFlagBits)a->resolve_mode
                                   : depth || s->format.integer ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                                                : VK_RESOLVE_MODE_AVERAGE_BIT;
        info->resolveImageView   = image_view(g, image_slot(g, a->resolve.image), resolve, s->format.aspect)->view;
        info->resolveImageLayout = layout;
    }

    p->samples               = s->samples;
    p->stat->declared_bytes += (uint64_t)width * height * s->format.bytes * s->samples;
    area->width              = area->width ? area->width : width;
    area->height             = area->height ? area->height : height;
    return s;
}

/* Resolves the VkPipeline for (pipe, attachments, static state), binds it when
 * it changed, and emits the dynamic state when the DrawState changed. */
static void pass_apply(Pass *p)
{
    /* the pipeline bakes in the class; the member is dynamic */
    static const uint8_t             TOPOLOGY_CLASS[] = { [TOPO_TRI] = TOPO_TRI, [TOPO_TRI_STRIP] = TOPO_TRI, [TOPO_LINE] = TOPO_LINE, [TOPO_POINT] = TOPO_POINT };
    static const VkPrimitiveTopology TOPOLOGIES[]     = {
        [TOPO_TRI]       = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        [TOPO_TRI_STRIP] = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        [TOPO_LINE]      = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        [TOPO_POINT]     = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
    };
    static const VkCullModeFlags CULLS[]    = { [CULL_NONE] = VK_CULL_MODE_NONE, [CULL_FRONT] = VK_CULL_MODE_FRONT_BIT, [CULL_BACK] = VK_CULL_MODE_BACK_BIT };
    static const VkCompareOp     COMPARES[] = {
        [CMP_NONE]    = VK_COMPARE_OP_ALWAYS,
        [CMP_NEVER]   = VK_COMPARE_OP_NEVER,
        [CMP_LESS]    = VK_COMPARE_OP_LESS,
        [CMP_EQUAL]   = VK_COMPARE_OP_EQUAL,
        [CMP_LEQUAL]  = VK_COMPARE_OP_LESS_OR_EQUAL,
        [CMP_GREATER] = VK_COMPARE_OP_GREATER,
        [CMP_GEQUAL]  = VK_COMPARE_OP_GREATER_OR_EQUAL,
        [CMP_ALWAYS]  = VK_COMPARE_OP_ALWAYS,
    };
    static const VkPipelineColorBlendAttachmentState BLENDS[] = {
        [BLEND_OPAQUE] = { .blendEnable = VK_FALSE },
        [BLEND_ALPHA]  = { VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                           VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0 },
        [BLEND_PREMUL] = { VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                           VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0 },
        [BLEND_ADD]    = { VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
                           VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0 },
    };
    Gpu                                *g       = p->g;
    PipeSlot                           *pipe    = &g->pipes[p->pipe.index];
    DrawState                           s       = p->state;
    Variant                            *variant = NULL;
    VkPipelineColorBlendAttachmentState blends[MAX_COLOR_ATTACHMENTS];
    VkColorBlendEquationEXT             equations[MAX_COLOR_ATTACHMENTS];
    VkColorComponentFlags               masks[MAX_COLOR_ATTACHMENTS];
    VkBool32                            enables[MAX_COLOR_ATTACHMENTS];
    VariantKey                          key;
    uint32_t                            i;

    MU_ASSERT(pipe->gen == p->pipe.gen && pipe->refs && pipe->key.vertex);
    /* the attachments decide the sample count; a state that names another is a bug */
    MU_ASSERT(!s.samples || s.samples == p->samples);

    for (i = 0; i < p->color_count; ++i)
    {
        /* integer targets cannot blend, whatever the state asks for */
        blends[i]                = BLENDS[p->color_integer[i] ? BLEND_OPAQUE : s.blend];
        blends[i].colorWriteMask = i < 4 ? (s.write_mask >> (4 * i)) & 0xfu : 0xfu;
        masks[i]                 = blends[i].colorWriteMask;
        enables[i]               = blends[i].blendEnable;
        equations[i]             = (VkColorBlendEquationEXT){ blends[i].srcColorBlendFactor, blends[i].dstColorBlendFactor, blends[i].colorBlendOp,
                                                              blends[i].srcAlphaBlendFactor, blends[i].dstAlphaBlendFactor, blends[i].alphaBlendOp };
    }

    /* the key holds only what this device cannot change after the fact */
    memset(&key, 0, sizeof key);
    memcpy(key.colors, p->color_formats, sizeof key.colors);
    key.depth            = p->depth_format;
    key.color_count      = p->color_count;
    key.pipe             = p->pipe.index;
    key.pipe_gen         = p->pipe.gen;
    key.state.samples    = (uint8_t)p->samples & g->static_state.samples;
    key.state.blend      = s.blend & g->static_state.blend;
    key.state.write_mask = s.write_mask & g->static_state.write_mask;
    key.state.topology   = TOPOLOGY_CLASS[s.topology];
    for (i = 0; i < g->variant_count; ++i)
        if (!memcmp(&g->variants[i].key, &key, sizeof key))
            variant = &g->variants[i];

    if (!variant)
    {
        VkPipelineShaderStageCreateInfo stages[2] = {
            { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage  = VK_SHADER_STAGE_VERTEX_BIT,
              .module = g->shaders[pipe->key.vertex].module,
              .pName  = pipe->key.entries[0] },
            { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
              .module = g->shaders[pipe->key.fragment].module,
              .pName  = pipe->key.entries[1] },
        };
        /* empty on purpose: shaders pull their vertices, so vertex layout is never a pipeline axis */
        VkPipelineVertexInputStateCreateInfo   vertex_input = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly     = {
            .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = TOPOLOGIES[TOPOLOGY_CLASS[s.topology]],
        };
        VkPipelineViewportStateCreateInfo viewport = {
            .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount  = 1,
        };
        VkPipelineRasterizationStateCreateInfo raster = {
            .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth   = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo multisample = {
            .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = (VkSampleCountFlagBits)p->samples,
        };
        VkPipelineDepthStencilStateCreateInfo depth_stencil = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        VkPipelineColorBlendStateCreateInfo   blend         = {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = p->color_count,
            .pAttachments    = blends,
        };
        /* The first 8 are Vulkan 1.3 core; the last 4 need extended dynamic
         * state 3. The three blend states have nothing to act on in a
         * depth-only pass, and Vulkan then forbids declaring them. */
        VkDynamicState dynamics[12] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT,
            VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
            VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
            VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
        };
        VkPipelineDynamicStateCreateInfo dynamic = {
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = !g->eds3 ? 8 : p->color_count ? 12 : 9,
            .pDynamicStates    = dynamics,
        };
        VkPipelineCreationFeedback           feedback[3]   = { { 0 } }; /* the pipeline, then its stages */
        VkPipelineCreationFeedbackCreateInfo feedback_info = {
            .sType                              = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
            .pPipelineCreationFeedback          = &feedback[0],
            .pipelineStageCreationFeedbackCount = 1 + (pipe->key.fragment != SHADER_NONE),
            .pPipelineStageCreationFeedbacks    = &feedback[1],
        };
        VkPipelineRenderingCreateInfo rendering = {
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext                   = &feedback_info,
            .viewMask                = pipe->key.view_mask,
            .colorAttachmentCount    = p->color_count,
            .pColorAttachmentFormats = p->color_formats,
            .depthAttachmentFormat   = p->depth_format,
            .stencilAttachmentFormat = p->stencil_format,
        };
        VkGraphicsPipelineCreateInfo info = {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &rendering,
            .stageCount          = 1 + (pipe->key.fragment != SHADER_NONE), /* no fragment shader: depth only */
            .pStages             = stages,
            .pVertexInputState   = &vertex_input,
            .pInputAssemblyState = &assembly,
            .pViewportState      = &viewport,
            .pRasterizationState = &raster,
            .pMultisampleState   = &multisample,
            .pDepthStencilState  = &depth_stencil,
            .pColorBlendState    = &blend,
            .pDynamicState       = &dynamic,
            .layout              = g->layout,
        };

        if (g->variant_count == MAX_VARIANTS)
            gpu_fatal("all %d pipeline variants are in use", MAX_VARIANTS);
        variant      = &g->variants[g->variant_count++];
        variant->key = key;
        VK_CHECK(vkCreateGraphicsPipelines(g->device, g->pipeline_cache, 1, &info, NULL, &variant->pipeline));
        g->pipeline_added |= (feedback[0].flags & CACHE_HIT) != CACHE_HIT; /* one more for gpu_destroy to keep */
        name_object(g, VK_OBJECT_TYPE_PIPELINE, (uint64_t)variant->pipeline, pipe->name);
    }

    if (variant->pipeline != p->bound)
    {
        vkCmdBindPipeline(p->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, variant->pipeline);
        p->bound    = variant->pipeline;
        p->run_open = false;
    }

    /* one burst per state change; a run of draws in one state emits nothing */
    if (p->applied_valid && !memcmp(&p->applied, &s, sizeof s))
        return;
    vkCmdSetCullMode(p->cmd, CULLS[s.cull]);
    vkCmdSetFrontFace(p->cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(p->cmd, TOPOLOGIES[s.topology]);
    vkCmdSetDepthTestEnable(p->cmd, s.depth_compare != CMP_NONE);
    vkCmdSetDepthWriteEnable(p->cmd, s.depth_write && !p->depth_read_only); /* the attachment has the last word */
    vkCmdSetDepthCompareOp(p->cmd, COMPARES[s.depth_compare]);
    if (g->eds3)
        vkCmdSetRasterizationSamplesEXT(p->cmd, (VkSampleCountFlagBits)p->samples);
    if (g->eds3 && p->color_count)
    {
        vkCmdSetColorBlendEnableEXT(p->cmd, 0, p->color_count, enables);
        vkCmdSetColorBlendEquationEXT(p->cmd, 0, p->color_count, equations);
        vkCmdSetColorWriteMaskEXT(p->cmd, 0, p->color_count, masks);
    }
    p->applied       = s;
    p->applied_valid = true;
    p->run_open      = false;
}

Pass *begin_pass(Gpu *g, const PassDesc *desc)
{
    static const VkAccessFlags2 STORAGE_ACCESS[] = {
        [ACC_READ]  = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        [ACC_WRITE] = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        [ACC_RW]    = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    static const VkAccessFlags2 BUFFER_ACCESS[] = {
        [ACC_READ]  = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
        [ACC_WRITE] = VK_ACCESS_2_SHADER_WRITE_BIT,
        [ACC_RW]    = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
    };
    Pass                     *p       = &g->pass;
    VkCommandBuffer           cmd     = cmd_open(g);
    FrameStats               *stats   = g->recording;
    Frame                    *frame   = &g->frames[g->frame_index];
    bool                      counted = stats->pass_count < MAX_PASSES;
    VkPipelineStageFlags2     stages  = desc->kind == PASS_COMPUTE ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                                                   : VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    uint32_t                  barriers_before = stats->barriers;
    VkExtent2D                area            = { desc->width, desc->height };
    VkDebugUtilsLabelEXT      label           = { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = desc->name ? desc->name : "pass" };
    VkRenderingAttachmentInfo colors[MAX_COLOR_ATTACHMENTS], depth;
    VkRenderingInfo           rendering = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .layerCount           = MU_MAX(MU_MAX(desc->colors[0].image.layer_count, desc->depth.image.layer_count), 1),
        /* slot 0 is zeroed, so a pass without an initial pipe gets view mask 0 */
        .viewMask             = g->pipes[desc->pipe.index % MAX_PIPELINES].key.view_mask,
        .colorAttachmentCount = desc->color_count,
        .pColorAttachments    = colors,
    };
    const ImageUse  *image_use;
    const BufferUse *buffer_use;
    ImageSlot       *s;
    bool             sampled, is_depth;
    uint32_t         i;

    MU_ASSERT(!p->open && !g->upload.open);
    MU_ASSERT(desc->color_count <= MAX_COLOR_ATTACHMENTS && desc->image_count <= MAX_IMAGE_USES && desc->buffer_count <= MAX_BUFFER_USES);

    memset(p, 0, sizeof *p);
    p->g       = g;
    p->cmd     = cmd;
    p->kind    = desc->kind;
    p->state   = desc->state;
    p->samples = 1;
    p->open    = true;
    /* passes beyond MAX_PASSES still run; they just share one stat row and are not timed */
    p->row   = stats->pass_count;
    p->stat  = counted ? &stats->passes[p->row] : &g->spill;
    p->timed = counted && g->in_frame && frame->profiled;
    p->views = 1;
    stats->pass_count += counted;
    memset(p->stat, 0, sizeof *p->stat);
    snprintf(p->stat->name, sizeof p->stat->name, "%s", label.pLabelName);

    /* ---- declaration -> barriers ---------------------------------------- */
    for (i = 0; i < desc->color_count; ++i)
    {
        MU_ASSERT(!desc->colors[i].read_only); /* a colour nobody writes is not an attachment */
        s = attach(p, &desc->colors[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, &colors[i], &area);
        p->color_formats[i] = s->format.vk;
        p->color_integer[i] = s->format.integer;
    }
    p->color_count = desc->color_count;
    if (desc->has_depth)
    {
        /* Read-only depth takes the layout a sampled depth image has, the one
         * its bindless descriptor was written with. A pass that also lists the
         * image below then gets one barrier for both uses: tested and sampled. */
        s = attach(p, &desc->depth, desc->depth.read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | (desc->depth.read_only ? 0 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT), &depth, &area);
        p->depth_read_only           = desc->depth.read_only;
        p->depth_format              = s->format.vk;
        p->stencil_format            = s->format.aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? s->format.vk : VK_FORMAT_UNDEFINED;
        rendering.pDepthAttachment   = &depth;
        rendering.pStencilAttachment = p->stencil_format ? &depth : NULL;
    }
    for (i = 0; i < desc->image_count; ++i)
    {
        image_use = &desc->images[i];
        s         = image_slot(g, image_use->image.image);
        sampled   = image_use->type == IMAGE_SAMPLED;
        is_depth  = s->format.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
        MU_ASSERT(!sampled || image_use->access == ACC_READ);
        /* the layouts the bindless descriptors were written with */
        sync_image(g, image_use->image,
                  !sampled ? VK_IMAGE_LAYOUT_GENERAL : is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  stages, sampled ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : STORAGE_ACCESS[image_use->access], !sampled && image_use->access != ACC_READ);
        p->stat->declared_bytes += (uint64_t)MU_MAX(s->width >> image_use->image.mip, 1u) * MU_MAX(s->height >> image_use->image.mip, 1u) * s->format.bytes;
    }
    for (i = 0; i < desc->buffer_count; ++i)
    {
        buffer_use = &desc->buffers[i];
        /* DRAW_INDIRECT is the stage that reads indirect arguments, for draws and dispatches alike */
        sync_buffer(g, buffer_use->range.buffer, stages | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, BUFFER_ACCESS[buffer_use->access], buffer_use->access != ACC_READ);
        p->stat->declared_bytes += buffer_use->range.size;
    }
    barriers_flush(g, cmd);
    p->stat->barriers = stats->barriers - barriers_before;

    if (g->debug_utils)
        vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
    if (p->timed)
        frame->pass_queries[p->row][0] = timestamp(frame, cmd, 1);

    /* ---- dynamic rendering ---------------------------------------------- */
    if (desc->kind == PASS_GRAPHICS)
    {
        MU_ASSERT(area.width && area.height);
        rendering.renderArea.extent = area;
        vkCmdBeginRendering(cmd, &rendering);
        p->views = MU_MAX(mu_popcount_u32(rendering.viewMask), 1u);
        /* every pass starts with all of its area in view */
        p->area = area;
        set_viewport(p, 0.0f, 0.0f, (float)area.width, (float)area.height);
        set_scissor(p, 0, 0, area.width, area.height);
    }

    /* ---- pipeline -------------------------------------------------------- */
    /* An all-zero state would write nothing at all. In a PassDesc it means the
     * default instead: opaque, no depth test, no culling, every channel written. */
    if (!memcmp(&p->state, &(DrawState){ 0 }, sizeof p->state))
        p->state.write_mask = 0xffff;
    if (HANDLE_VALID(desc->pipe))
        bind_pipe(p, desc->pipe);
    return p;
}

void end_pass(Pass *p)
{
    Gpu   *g     = p->g;
    Frame *frame = &g->frames[g->frame_index];

    MU_ASSERT(p->open);
    /* The last scope ends here, while the pass still renders: what ends a
     * pass (resolve, store) is the pass's time and no scope's. */
    if (p->scoped)
        timestamp(frame, p->cmd, p->views);
    if (p->kind == PASS_GRAPHICS)
        vkCmdEndRendering(p->cmd);
    if (p->timed)
        frame->pass_queries[p->row][1] = timestamp(frame, p->cmd, 1);
    if (g->debug_utils)
        vkCmdEndDebugUtilsLabelEXT(p->cmd);
    p->open = false;
}

void bind_pipe(Pass *p, Pipe pipe)
{
    PipeSlot *slot = &p->g->pipes[pipe.index % MAX_PIPELINES];

    MU_ASSERT(p->open && HANDLE_VALID(pipe) && pipe.index < MAX_PIPELINES && slot->gen == pipe.gen);
    p->pipe = pipe;
    if (p->kind == PASS_COMPUTE)
    {
        MU_ASSERT(slot->compute);
        vkCmdBindPipeline(p->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, slot->compute);
        return;
    }
    pass_apply(p);
}

void set_state(Pass *p, DrawState state)
{
    MU_ASSERT(p->open);
    p->state = state;
    /* without a pipe yet the state waits for bind_pipe; compute has none */
    if (p->kind == PASS_GRAPHICS && HANDLE_VALID(p->pipe))
        pass_apply(p);
}

/* The rectangle is given the way pixels are counted, from the top left. The
 * height is negated so that clip +Y is up and uv (0,0) is the top-left texel
 * inside it: the convention the mu shader library is written against. One that
 * is already set emits nothing, like the draw state. */
void set_viewport(Pass *p, float x, float y, float width, float height)
{
    VkViewport viewport = { x, y + height, width, -height, 0.0f, 1.0f };

    MU_ASSERT(p->open && p->kind == PASS_GRAPHICS && width > 0.0f && height > 0.0f);
    if (!memcmp(&viewport, &p->viewport, sizeof viewport))
        return;
    vkCmdSetViewport(p->cmd, 0, 1, &viewport);
    p->viewport = viewport;
    p->run_open = false;
}

/* Cut down to the pass: Vulkan leaves whatever is drawn outside the render
 * area undefined, and the clip rectangles of a UI are free to start off screen. */
void set_scissor(Pass *p, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    int64_t  left    = MU_CLAMP((int64_t)x, 0, (int64_t)p->area.width);
    int64_t  top     = MU_CLAMP((int64_t)y, 0, (int64_t)p->area.height);
    int64_t  right   = MU_CLAMP((int64_t)x + width, 0, (int64_t)p->area.width);
    int64_t  bottom  = MU_CLAMP((int64_t)y + height, 0, (int64_t)p->area.height);
    VkRect2D scissor = { { (int32_t)left, (int32_t)top }, { (uint32_t)(right - left), (uint32_t)(bottom - top) } };

    MU_ASSERT(p->open && p->kind == PASS_GRAPHICS);
    if (!memcmp(&scissor, &p->scissor, sizeof scissor))
        return;
    vkCmdSetScissor(p->cmd, 0, 1, &scissor);
    p->scissor  = scissor;
    p->run_open = false;
}

/* Push constants are part of the work command: every draw and dispatch
 * carries its own block. */
static void pass_push(Pass *p, DataSpan push)
{
    MU_ASSERT(p->open && push.size <= MAX_PUSH_CONSTANTS && !(push.size & 3));
    if (push.size)
        vkCmdPushConstants(p->cmd, p->g->layout, STAGES_PUSH, 0, push.size, push.data);
}

/* What every draw does besides its Vulkan call: the push block, and the books.
 * A run is a stretch of draws with no change of pipeline, state, viewport or
 * scissor in between: the unit a state change is paid for in. */
static void pass_draw(Pass *p, DataSpan push)
{
    FrameStats *stats = p->g->recording;

    pass_push(p, push);
    p->stat->draws++;
    stats->draws++;
    stats->state_runs += !p->run_open;
    p->run_open        = true;
}

void draw(Pass *p, DataSpan push, uint32_t vertices, uint32_t instances)
{
    pass_draw(p, push);
    vkCmdDraw(p->cmd, vertices, instances, 0, 0);
}

/* The same launch as draw(): `indices` vertex shader invocations. There is no
 * index buffer to bind, so the shader reads index SV_VertexID from the storage
 * buffer itself and then the vertex that index names. */
void draw_indexed(Pass *p, DataSpan push, uint32_t indices, uint32_t instances)
{
    pass_draw(p, push);
    vkCmdDraw(p->cmd, indices, instances, 0, 0);
}

/* `arguments` holds VkDrawIndirectCommand records; stride 0 means tightly packed. */
void draw_indirect(Pass *p, DataSpan push, Range arguments, uint32_t max_draws, uint32_t stride)
{
    pass_draw(p, push);
    vkCmdDrawIndirect(p->cmd, buffer_slot(p->g, arguments.buffer)->buffer, arguments.offset, max_draws, stride ? stride : sizeof(VkDrawIndirectCommand));
}

void draw_indirect_count(Pass *p, DataSpan push, Range arguments, Range counter, uint32_t max_draws, uint32_t stride)
{
    MU_ASSERT(p->g->info.draw_indirect_count);
    pass_draw(p, push);
    vkCmdDrawIndirectCount(p->cmd, buffer_slot(p->g, arguments.buffer)->buffer, arguments.offset, buffer_slot(p->g, counter.buffer)->buffer, counter.offset,
                           max_draws, stride ? stride : sizeof(VkDrawIndirectCommand));
}

void dispatch(Pass *p, DataSpan push, uint32_t x, uint32_t y, uint32_t z)
{
    pass_push(p, push);
    vkCmdDispatch(p->cmd, x, y, z);
    p->stat->dispatches++;
    p->g->recording->dispatches++;
}

void dispatch_indirect(Pass *p, DataSpan push, Range arguments)
{
    pass_push(p, push);
    vkCmdDispatchIndirect(p->cmd, buffer_slot(p->g, arguments.buffer)->buffer, arguments.offset);
    p->stat->dispatches++;
    p->g->recording->dispatches++;
}

VkCommandBuffer pass_raw_cmd(Pass *p)
{
    return p->cmd;
}

/* ============================================================================
 * 12. UPLOAD AND READBACK
 * ==========================================================================*/

Upload *upload_begin(Gpu *g)
{
    MU_ASSERT(!g->upload.open && !g->pass.open);
    g->upload = (Upload){ .g = g, .open = true };
    return &g->upload;
}

void upload_buffer(Upload *u, Range dst, DataSpan src)
{
    Gpu             *g       = u->g;
    BufferSlot      *b       = buffer_slot(g, dst.buffer);
    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = STAGES_SHADER | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
    };
    VkDependencyInfo dependency = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier };
    VkBufferCopy     region     = { .dstOffset = dst.offset, .size = src.size };
    VkCommandBuffer  cmd;
    Range            staging;

    MU_ASSERT(u->open && src.size <= dst.size && dst.offset + src.size <= b->size);
    g->recording->upload_bytes += src.size;

    /* host-visible memory is written in place: no command, no staging copy */
    if (b->mapped)
    {
        memcpy((uint8_t *)b->mapped + dst.offset, src.data, src.size);
        return;
    }

    /* Small and 4-byte aligned: the bytes ride inside the command buffer.
     * Anything else goes through the staging ring. The staging allocation comes
     * first because it may submit the open batch and start a new one. */
    if (src.size > INLINE_UPLOAD_MAX || ((src.size | dst.offset) & 3))
    {
        staging = arena_alloc(g, ARENA_STAGING, src.size, 16);
        memcpy((uint8_t *)buffer_slot(g, staging.buffer)->mapped + staging.offset, src.data, src.size);
        region.srcOffset = staging.offset;
    }
    cmd = cmd_open(g);

    /* once per batch: whatever shaders or earlier copies did to buffers is
     * finished before these copies run */
    if (!u->wrote_buffers)
        vkCmdPipelineBarrier2(cmd, &dependency);
    u->wrote_buffers = true;

    if (src.size > INLINE_UPLOAD_MAX || ((src.size | dst.offset) & 3))
        vkCmdCopyBuffer(cmd, buffer_slot(g, g->arena_buffers[ARENA_STAGING])->buffer, b->buffer, 1, &region);
    else
        vkCmdUpdateBuffer(cmd, b->buffer, dst.offset, src.size, src.data);
}

/* `src` holds dst.mip_count mip levels back to back, each one all of its layers. */
void upload_image(Upload *u, ImageSub dst, DataSpan src)
{
    Gpu              *g         = u->g;
    ImageSlot        *s         = image_slot(g, dst.image);
    uint32_t          mip_count = MU_MAX(dst.mip_count, 1);
    uint32_t          layers    = MU_MAX(dst.layer_count, 1);
    uint64_t          offset    = 0;
    VkBufferImageCopy regions[MAX_MIPS];
    VkCommandBuffer   cmd;
    Range             staging;
    uint32_t          i, mip, width, height, depth;

    MU_ASSERT(u->open && mip_count <= MAX_MIPS && s->samples == 1);
    staging = arena_alloc(g, ARENA_STAGING, src.size, 16);
    memcpy((uint8_t *)buffer_slot(g, staging.buffer)->mapped + staging.offset, src.data, src.size);
    cmd = cmd_open(g); /* after the staging allocation, which may have submitted the batch */

    for (i = 0; i < mip_count; ++i)
    {
        mip        = dst.mip + i;
        width      = MU_MAX(s->width >> mip, 1u);
        height     = MU_MAX(s->height >> mip, 1u);
        depth      = MU_MAX(s->depth >> mip, 1u);
        regions[i] = (VkBufferImageCopy){
            .bufferOffset     = staging.offset + offset,
            /* depth and stencil are separate planes to a copy */
            .imageSubresource = { dst.stencil_only ? VK_IMAGE_ASPECT_STENCIL_BIT : s->format.aspect & ~(VkImageAspectFlags)VK_IMAGE_ASPECT_STENCIL_BIT,
                                  mip, dst.layer, layers },
            .imageExtent      = { width, height, depth },
        };
        offset += (uint64_t)width * height * depth * layers * s->format.bytes;
    }
    MU_ASSERT(offset <= src.size);
    g->recording->upload_bytes += offset;

    sync_image(g, (ImageSub){ .image = dst.image, .mip = dst.mip, .mip_count = (uint16_t)mip_count }, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    barriers_flush(g, cmd);
    vkCmdCopyBufferToImage(cmd, buffer_slot(g, staging.buffer)->buffer, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_count, regions);

    /* remember it for upload_end; a full list settles what it has first */
    for (i = 0; i < u->image_count && u->images[i].index != dst.image.index; ++i)
    {
    }
    if (i == MAX_UPLOAD_IMAGES)
    {
        upload_end(g, u);
        u->open = true;
        i       = 0;
    }
    u->images[i]   = dst.image;
    u->image_count = MU_MAX(u->image_count, i + 1);
}

void upload_end(Gpu *g, Upload *u)
{
    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = STAGES_SHADER | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT
                         | VK_ACCESS_2_TRANSFER_WRITE_BIT,
    };
    VkDependencyInfo dependency = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier };
    VkCommandBuffer  cmd;
    ImageSlot       *s;
    uint32_t         i, mip;
    bool             is_depth;

    MU_ASSERT(u->open);
    u->open = false;

    /* Uploaded data is settled here, once, so that nothing has to declare it
     * later: textures rest in their sampled layout, readable from every shader
     * stage, and buffer bytes are visible to every stage that can read one. */
    if (u->image_count || u->wrote_buffers)
    {
        cmd = cmd_open(g);
        for (i = 0; i < u->image_count; ++i)
        {
            s        = image_slot(g, u->images[i]);
            is_depth = s->format.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
            for (mip = 0; mip < s->mips; ++mip)
                if ((s->usage & IMG_SAMPLED) && s->sync[mip].layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
                    sync_image(g, (ImageSub){ .image = u->images[i], .mip = (uint16_t)mip, .mip_count = 1 },
                              is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, STAGES_SHADER,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
        }
        barriers_flush(g, cmd);
        if (u->wrote_buffers)
            vkCmdPipelineBarrier2(cmd, &dependency);
    }
    u->image_count   = 0;
    u->wrote_buffers = false;
    flush(g); /* outside a frame the batch goes out now; inside one it rides the frame's submit */
}

/* Both readbacks copy into a private host buffer inside the open command
 * buffer. The bytes reach `dst` in readback_ready(), once the timeline has
 * passed the submit that carried the copy. */
static Readback *readback_open(Gpu *g, DataSpan dst)
{
    Readback *r = NULL;
    uint32_t  i;

    for (i = MAX_READBACKS; i-- > 0;)
        if (!g->readbacks[i].used)
            r = &g->readbacks[i];
    if (!r)
        gpu_fatal("all %d readbacks are in use; release the finished ones", MAX_READBACKS);
    *r = (Readback){
        .staging = buffer_create(g, &(BufferDesc){ .name = "readback", .size = dst.size, .host_visible = true }),
        .dst     = (void *)dst.data,
        .size    = dst.size,
        .value   = g->submitted + 1,
        .used    = true,
    };
    return r;
}

Readback *readback_buffer(Gpu *g, Range src, DataSpan dst)
{
    Readback       *r      = readback_open(g, dst);
    VkCommandBuffer cmd    = cmd_open(g);
    VkBufferCopy    region = { .srcOffset = src.offset, .size = MU_MIN(src.size, (uint64_t)dst.size) };

    MU_ASSERT(!g->pass.open);
    sync_buffer(g, src.buffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
    barriers_flush(g, cmd);
    vkCmdCopyBuffer(cmd, buffer_slot(g, src.buffer)->buffer, buffer_slot(g, r->staging)->buffer, 1, &region);
    flush(g);
    return r;
}

/* One mip, in the image's own format and channel order. */
Readback *readback_image(Gpu *g, ImageSub src, DataSpan dst)
{
    ImageSlot        *s      = image_slot(g, src.image);
    Readback         *r      = readback_open(g, dst);
    VkCommandBuffer   cmd    = cmd_open(g);
    uint32_t          layers = MU_MAX(src.layer_count, 1);
    VkBufferImageCopy region = {
        .imageSubresource = { src.stencil_only ? VK_IMAGE_ASPECT_STENCIL_BIT : s->format.aspect & ~(VkImageAspectFlags)VK_IMAGE_ASPECT_STENCIL_BIT,
                              src.mip, src.layer, layers },
        .imageExtent      = { MU_MAX(s->width >> src.mip, 1u), MU_MAX(s->height >> src.mip, 1u), MU_MAX(s->depth >> src.mip, 1u) },
    };

    MU_ASSERT(!g->pass.open && s->samples == 1);
    MU_ASSERT((uint64_t)region.imageExtent.width * region.imageExtent.height * region.imageExtent.depth * layers * s->format.bytes <= dst.size);
    sync_image(g, (ImageSub){ .image = src.image, .mip = src.mip, .mip_count = 1 }, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
              VK_ACCESS_2_TRANSFER_READ_BIT, false);
    barriers_flush(g, cmd);
    vkCmdCopyImageToBuffer(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_slot(g, r->staging)->buffer, 1, &region);
    flush(g);
    return r;
}

/* With `wait`, blocks until the copy has run. That needs the copy to have been
 * submitted: for a readback requested inside a frame, call this after frame_end. */
bool readback_ready(Gpu *g, Readback *r, bool wait)
{
    MU_ASSERT(r->used);
    if (wait && !r->done)
        timeline_wait(g, r->value);
    timeline_poll(g);
    if (!r->done && g->completed >= r->value)
    {
        memcpy(r->dst, buffer_slot(g, r->staging)->mapped, r->size);
        r->done = true;
    }
    return r->done;
}

void readback_release(Gpu *g, Readback *r)
{
    MU_ASSERT(r->used);
    buffer_destroy(g, r->staging);
    *r = (Readback){ 0 };
}

/* ============================================================================
 * 13. PROFILING
 * ==========================================================================*/

/* Turns a finished frame's queries into milliseconds and publishes the frame
 * as gpu_stats(). Callers have waited for the frame, so every query it wrote
 * is available and nothing here blocks. */
static void frame_stats_resolve(Gpu *g, Frame *f)
{
    uint32_t i;

    if (!f->pending)
        return;
    f->pending = false;
    if (f->profiled)
    {
        uint64_t ticks[QUERIES_PER_FRAME] = { 0 };

        VK_CHECK(vkGetQueryPoolResults(g->device, f->timestamps, 0, f->query_count, sizeof ticks, ticks, sizeof ticks[0], VK_QUERY_RESULT_64_BIT));
        f->stats.gpu_ms = (double)(ticks[f->query_count - 1] - ticks[0]) * g->ms_per_tick;
        for (i = 0; i < f->stats.pass_count; ++i)
            f->stats.passes[i].gpu_ms = (double)(ticks[f->pass_queries[i][1]] - ticks[f->pass_queries[i][0]]) * g->ms_per_tick;
        for (i = 0; i < f->stats.scope_count; ++i)
            f->stats.scopes[i].gpu_ms = (double)(ticks[f->scope_queries[i][1]] - ticks[f->scope_queries[i][0]]) * g->ms_per_tick;
    }
    if (g->statistics)
        VK_CHECK(vkGetQueryPoolResults(g->device, f->statistics, 0, 1, sizeof g->statistics_result, g->statistics_result, sizeof g->statistics_result,
                                       VK_QUERY_RESULT_64_BIT));
    g->stats = f->stats;
}

/* The newest frame the GPU has finished: its counts and its timings belong
 * together. Call gpu_wait_idle() first to make that the last frame submitted. */
const FrameStats *gpu_stats(const Gpu *g)
{
    return &g->stats;
}

/* The application's word on what a pass does, kept in the pass's row. */
void pass_note(Pass *p, PassNote note)
{
    MU_ASSERT(p->open);
    p->stat->note = note;
}

/* One more row and, when the pass is timed, one more timestamp: the one that
 * begins a scope is also where the scope before it ends. */
void pass_scope(Pass *p, const char *name)
{
    Gpu        *g     = p->g;
    FrameStats *stats = g->recording;
    Frame      *frame = &g->frames[g->frame_index];
    uint32_t    row   = stats->scope_count;
    uint16_t    query;

    MU_ASSERT(p->open);
    /* a frame keeps MAX_PROFILE_SCOPES of them, and a pass beyond MAX_PASSES has no row for one to be part of */
    if (row == MAX_PROFILE_SCOPES || p->row == MAX_PASSES)
        return;
    stats->scopes[row] = (ScopeStat){ .pass = p->row };
    snprintf(stats->scopes[row].name, sizeof stats->scopes[row].name, "%s", name);
    stats->scope_count++;
    if (p->timed)
    {
        query                        = timestamp(frame, p->cmd, p->views);
        frame->scope_queries[row][0] = query;
        frame->scope_queries[row][1] = (uint16_t)(query + p->views); /* the next one this pass writes, here or in end_pass() */
        p->scoped                    = true;
    }
}

/* Takes effect from the next frame_begin. */
void gpu_profile(Gpu *g, bool enabled)
{
    g->profile = enabled && g->timestamps;
}

void gpu_dump_stats(const Gpu *g, const char *path)
{
    static const char *const ARENA_NAMES[3]                 = { "device", "transient", "staging" };
    static const char *const STATISTIC_NAMES[STATISTIC_COUNT] = { "input vertices", "vertex shader runs", "clipped primitives", "fragment shader runs",
                                                                  "compute shader runs" };
    static const char *const NOTE_NAMES[]                   = { "samples per pixel", "effective samples", "steps per pixel", "tile size" };
    const FrameStats        *stats                          = &g->stats;
    FILE                    *out                            = path ? fopen(path, "w") : stdout;
    uint32_t                 i, j;

    if (!out)
        gpu_fatal("cannot write stats to %s", path);
    fprintf(out, "gpu      %s (%ux%u)%s\n", g->info.name, g->info.width, g->info.height, g->eds3 ? "" : ", static blend/samples");
    fprintf(out, "frame    cpu %.3f ms, gpu %.3f ms\n", stats->cpu_ms, stats->gpu_ms);
    fprintf(out, "work     %u passes, %u draws in %u state runs, %u dispatches, %u barriers, %u layout transitions, %u descriptor writes\n", stats->pass_count,
            stats->draws, stats->state_runs, stats->dispatches, stats->barriers, stats->layout_transitions, stats->descriptor_writes);
    fprintf(out, "memory   %llu upload bytes, %llu transient bytes\n", (unsigned long long)stats->upload_bytes, (unsigned long long)stats->transient_peak);
    fprintf(out, "objects  %u pipelines, %u shaders\n", g->variant_count, g->shader_count);
    for (i = 0; i < 3; ++i)
        fprintf(out, "arena    %-9s %10llu / %llu bytes, peak %llu, %u allocations\n", ARENA_NAMES[i], (unsigned long long)g->arena_stats[i].used,
                (unsigned long long)g->arena_stats[i].reserved, (unsigned long long)g->arena_stats[i].peak, g->arena_stats[i].allocations);
    for (i = 0; i < STATISTIC_COUNT && g->statistics; ++i)
        fprintf(out, "count    %-20s %llu\n", STATISTIC_NAMES[i], (unsigned long long)g->statistics_result[i]);
    fprintf(out, "%-24s %9s %6s %10s %9s %12s\n", "pass", "gpu ms", "draws", "dispatches", "barriers", "declared MB");
    /* a pass, then its scopes: those are in the order of their passes */
    for (i = 0, j = 0; i < stats->pass_count; ++i)
    {
        fprintf(out, "%-24s %9.3f %6u %10u %9u %12.2f\n", stats->passes[i].name, stats->passes[i].gpu_ms, stats->passes[i].draws, stats->passes[i].dispatches,
                stats->passes[i].barriers, (double)stats->passes[i].declared_bytes / (1024.0 * 1024.0));
        for (; j < stats->scope_count && stats->scopes[j].pass == i; ++j)
            fprintf(out, "  %-22s %9.3f\n", stats->scopes[j].name, stats->scopes[j].gpu_ms);
    }
    /* what the application said about its passes: its word, not a measurement, and only what it did say */
    for (i = 0; i < stats->pass_count; ++i)
    {
        const PassNote *note      = &stats->passes[i].note;
        const uint32_t  numbers[] = { note->samples, note->effective_samples, note->steps_per_pixel, note->tile_size };

        if (note->workgroup[0])
            fprintf(out, "note     %-24s %-20s %ux%ux%u\n", stats->passes[i].name, "workgroup", note->workgroup[0], note->workgroup[1], note->workgroup[2]);
        for (j = 0; j < MU_ARRAY_COUNT(NOTE_NAMES); ++j)
            if (numbers[j])
                fprintf(out, "note     %-24s %-20s %u\n", stats->passes[i].name, NOTE_NAMES[j], numbers[j]);
    }
    if (path)
        fclose(out);
}
