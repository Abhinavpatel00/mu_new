/*
 * mg.c — the single implementation of mg.h.
 *
 * One header, one implementation file. This translation unit is the whole of
 * `mg`: the collector (flat draw array, stable radix sort, coalescing) and the
 * Vulkan backend (device, one UMA heap, pass table, material sets, thin pipes,
 * one fence per frame slot). It is the only file in the project that mentions a
 * Vk* type.
 *
 *   app:            #include "mg.h"          -- no Vulkan, no platform
 *   build:          cc app.c mg.c -lvulkan  -- or -DMG_TEST for the collector
 *
 * The split inside the file is the split the API is designed around:
 *
 *   PART 1  the collector. Pure data: 24-byte records, a 4 x 8-bit stable
 *           radix sort, a coalescing walk. No Vulkan, no allocation, no
 *           platform. Compiles and runs anywhere (that is what -DMG_TEST does).
 *
 *   PART 2  the backend. Everything that knows what a descriptor set, a
 *           render pass or a swapchain is: Android surface, pre-rotation, one
 *           mapped heap, transient attachments, one submit and one present per
 *           frame.
 *
 * Targets Vulkan 1.1 core with no optional features beyond swapchain, so the
 * same binary runs on the whole Android Baseline 2021 fleet.
 *
 * Implemented: everything mg.h declares EXCEPT the ones that need a memory
 * manager or an in-frame copy path, which this backend deliberately leaves out
 * (they are stage 3 of the port plan in MOBILE_API_DESIGN.md section 12):
 *
 *     mg_malloc / mg_free            -- no general allocator; the device owns
 *                                       one mapped heap, bumped internally
 *     mg_upload / mg_readback        -- no mid-frame copies; textures upload at
 *                                       creation, nothing reads back
 *     mg_destroy_tex / _mesh / _mat  -- objects live until mg_destroy()
 *     mg_draw_indirect               -- the coarse GPU-driven escape, section 10
 *     mg_dump                        -- no stats file; mg_stats() is the read API
 *
 * Calling any of them is a link error today, not a silent no-op: a missing
 * symbol is the loudest failure this file can produce.
 */

#define _POSIX_C_SOURCE 200809L

#include "mg.h"

#include <string.h>

// ============================================================
// PART 1 -- THE COLLECTOR
// ============================================================

// ============================================================
// 1. THE SORT KEY
// ============================================================
//
// pipe: 12 bits, mat: 12 bits, bucket: 8 bits — exactly 32, so the key is one
// word and the sort is four 8-bit digit passes. `order` is deliberately not in
// the key: stability over the record array is what preserves submission order
// inside a run, which is the guarantee alpha blending needs and the reason we
// can sort everything at all.

uint32_t mg_coll_key(const MgDraw *d)
{
    // Ordered draws keep submission order and land after everything else:
    // they coalesce with each other but never move relative to one another.
    if (d->flags & MG_DRAW_ORDERED) return 0xFF000000u | (d->order & 0x00FFFFFFu);
    return ((d->pipe & 0xFFFu) << 20)
         | ((d->mat  & 0xFFFu) << 8)
         |  (d->state & 0x0Fu);
}

// ============================================================
// 2. APPEND
// ============================================================

void mg_coll_clear(MgDrawList *l)
{
    l->count = 0;
    l->overflow = 0;
}

void mg_coll_draw(MgDrawList *l, const MgDraw *d)
{
    if (l->count >= MG_MAX_DRAWS) { l->overflow++; return; }
    l->draw[l->count] = *d;
    l->draw[l->count].order = l->count;
    l->count++;
}

// ============================================================
// 3. STABLE LSD RADIX SORT, 4 x 8 BITS
// ============================================================
//
// 10,000 records in ~120 us on a laptop-class core. No comparisons, no
// branches on data, no allocation, and — after four passes — the result is
// back in `draw[]`, so no copy is needed at the end.

void mg_coll_sort_range(MgDrawList *l, uint32_t first, uint32_t count)
{
    const uint32_t n = count;
    if (n < 2) return;

    MgDraw *src = &l->draw[first];
    MgDraw *dst = l->scratch;

    for (uint32_t pass = 0; pass < 4; pass++) {
        const uint32_t shift = pass * 8u;

        uint32_t count[256] = { 0 };
        for (uint32_t i = 0; i < n; i++) count[(mg_coll_key(&src[i]) >> shift) & 0xFFu]++;

        uint32_t sum = 0;
        for (uint32_t b = 0; b < 256; b++) { const uint32_t c = count[b]; count[b] = sum; sum += c; }

        for (uint32_t i = 0; i < n; i++)
            dst[count[(mg_coll_key(&src[i]) >> shift) & 0xFFu]++] = src[i];

        MgDraw *t = src; src = dst; dst = t;   // 4 passes: ends back in draw[]
    }
}

void mg_coll_sort(MgDrawList *l)
{
    mg_coll_sort_range(l, 0, l->count);
}

// ============================================================
// 4. COALESCING
// ============================================================
//
// Walks whatever order the array is in and counts what the driver would have
// to be told. Called before the sort by the test, and after it by the backend
// to emit binds. The push count is always the record count: on mobile the push
// is the cheap part (8 B into the command stream), the binds are not.

MgCollStats mg_coll_coalesce(const MgDrawList *l)
{
    MgCollStats s = { 0 };
    s.records = l->count;
    s.push_count = l->count;
    if (l->count == 0) return s;

    s.runs = 1;
    s.pipe_binds = 1;
    s.mat_binds = 1;

    const MgDraw *prev = &l->draw[0];
    for (uint32_t i = 1; i < l->count; i++) {
        const MgDraw *d = &l->draw[i];
        const bool new_pipe = d->pipe  != prev->pipe;
        const bool new_mat  = d->mat   != prev->mat;
        const bool new_buck = d->state != prev->state;

        s.pipe_binds += new_pipe;
        s.mat_binds  += new_mat;
        s.bucket_changes += new_buck;
        s.runs += (new_pipe | new_mat | new_buck);
        prev = d;
    }
    return s;
}

// ============================================================
// 5. TEST — the numbers quoted in MOBILE_API_DESIGN.md
// ============================================================

#ifndef MG_TEST

// ============================================================
// PART 2 -- THE VULKAN BACKEND
// ============================================================

#include <android/log.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "mg.h"
#include "shaders/spirv.h"

// VK_EXT_layer_settings lets the app hand the validation layer its settings at
// vkCreateInstance time. The NDK's headers (r26: vk 1.3.237) predate it, and
// the structs are three plain fields, so declare them locally rather than
// requiring a newer NDK. Values from the Vulkan registry (vk.xml /
// vulkan_core.h).
#ifndef VK_EXT_layer_settings
#define VK_EXT_layer_settings 1
#define VK_EXT_LAYER_SETTINGS_EXTENSION_NAME "VK_EXT_layer_settings"
#define VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT ((VkStructureType)1000496000)
typedef enum VkLayerSettingTypeEXT {
    VK_LAYER_SETTING_TYPE_BOOL32_EXT  = 0,
    VK_LAYER_SETTING_TYPE_INT32_EXT   = 1,
    VK_LAYER_SETTING_TYPE_INT64_EXT   = 2,
    VK_LAYER_SETTING_TYPE_UINT32_EXT  = 3,
    VK_LAYER_SETTING_TYPE_UINT64_EXT  = 4,
    VK_LAYER_SETTING_TYPE_FLOAT32_EXT = 5,
    VK_LAYER_SETTING_TYPE_FLOAT64_EXT = 6,
    VK_LAYER_SETTING_TYPE_STRING_EXT  = 7,
    VK_LAYER_SETTING_TYPE_MAX_ENUM_EXT = 0x7FFFFFFF,
} VkLayerSettingTypeEXT;
typedef struct VkLayerSettingEXT {
    const char           *pLayerName;
    const char           *pSettingName;
    VkLayerSettingTypeEXT type;
    uint32_t              valueCount;
    const void           *pValues;
} VkLayerSettingEXT;
typedef struct VkLayerSettingsCreateInfoEXT {
    VkStructureType             sType;
    const void                 *pNext;
    uint32_t                    settingCount;
    const VkLayerSettingEXT    *pSettings;
} VkLayerSettingsCreateInfoEXT;
#endif

#define TAG "mg"

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__))

static const char *vk_result_name(long r)
{
    switch ((VkResult)r) {
    case VK_ERROR_OUT_OF_HOST_MEMORY:     return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:   return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:  return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:            return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:      return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:      return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:  return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:    return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:    return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:       return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:   return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR:       return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR:               return "VK_SUBOPTIMAL_KHR";
    default:                              return "see vulkan_core.h";
    }
}

/* Every one of these is a bug or an unsupported device, never a runtime
 * condition: log it loudly, with the call that failed, then die. The demo's
 * crash handler reports the step it was on. */
static void mg_fatal(const char *what, long code)
{
    __android_log_print(ANDROID_LOG_ERROR, TAG, "FATAL: %s -> %ld (%s)", what, code,
                        vk_result_name(code));
    abort();
}

#define VKOK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) mg_fatal(#x, (long)r_); } while (0)
#define VKTRUE(x, ...) do { if (!(x)) mg_fatal(__VA_ARGS__, 0); } while (0)

#define ARENA_BYTES   (8u << 20)
#define ARENA_ALIGN   4096u          /* >= bufferImageGranularity on every device */
#define PARAMS_BYTES  (MG_CONST_SLOTS * MG_CONST_SLOT_BYTES)
#define UBO_BYTES     256u
#define MAX_SWAP      8u

// ============================================================
// 1. RECORDS
// ============================================================

typedef struct PassVk {
    MgPassDesc  d;
    VkRenderPass rp;
    // attachments this pass owns (0 = msaa colour, 1 = resolve, 2 = depth)
    VkImage       img[4];
    VkImageView   view[4];
    VkDeviceMemory mem[4];
    uint32_t      n_img;
    VkFramebuffer fb[MAX_SWAP];
    uint32_t      fb_count;
    uint32_t      out_tex;    // stable texture handle for this pass's output
    uint32_t      depth_att;  // attachment index of the depth image, if any
    bool          has_depth;
    bool          built;
} PassVk;

typedef struct PipeVk {
    MgPipeDesc d;
    VkPipeline pipe[MG_MAX_STATES];   // [pipe][state], pre-warmed
    bool       have[MG_MAX_STATES];
} PipeVk;

typedef struct MatVk {
    MgMatDesc     d;
    VkDescriptorSet set;
} MatVk;

typedef struct MeshVk {
    uint32_t first_index;
    uint32_t index_count;
    int32_t  vertex_offset;
    uint32_t base_vertex;             // where its vertices were written
} MeshVk;

typedef struct TexVk {
    VkImage     img;
    VkImageView view;
    VkDeviceMemory mem;
    VkDeviceSize mem_off;   // offset inside mem (small images share a pool block)
    bool        owns_mem;   // false: mem is a pool block, freed once per pool
    MgTexDesc   d;
    bool        owned;    // false: the image belongs to a pass, not to us
} TexVk;

struct MgDevice {
    VkInstance       inst;
    VkPhysicalDevice phys;
    VkDevice         dev;
    VkQueue          queue;
    uint32_t         qfam;
    VkPhysicalDeviceProperties props;
    MgCaps           caps;

    // surface + swapchain
    VkSurfaceKHR   surf;
    ANativeWindow *win;
    VkSwapchainKHR swap;
    VkFormat       swap_fmt;
    VkExtent2D     swap_ext;
    uint32_t       swap_n;
    VkImage        swap_img[MAX_SWAP];
    VkImageView    swap_view[MAX_SWAP];
    bool           swap_srgb;
    bool           have_surface;

    // one heap, mapped once: all geometry, constants and per-frame data live here
    VkDeviceMemory heap_mem;
    void          *heap_map;
    VkDeviceSize   heap_cap, heap_used;
    VkBuffer       vbo_pos, vbo_attr, ibo_arena, params, ubo;
    VkDeviceSize   vbo_pos_off, vbo_attr_off, ibo_off, params_off, ubo_off;
    uint32_t       pos_verts, pos_cap, idx_cap, idx_used;

    // descriptors
    VkDescriptorSetLayout set0_layout, set1_layout;
    VkDescriptorPool      dpool;
    VkDescriptorSet       set0[MG_FRAMES_IN_FLIGHT];
    VkPipelineLayout      playout;
    VkSampler             sampler_linear, sampler_point;

    // frame machinery
    VkCommandPool   cmd_pool;
    VkCommandBuffer cmd[MG_FRAMES_IN_FLIGHT];
    VkSemaphore     sem_acquire[MG_FRAMES_IN_FLIGHT];
    // One present semaphore PER SWAPCHAIN IMAGE, not per frame slot: the
    // presentation engine holds the signal until that image is acquired again,
    // and images come back in an arbitrary order (VUID-vkQueueSubmit-
    // pSignalSemaphores-00067).
    VkSemaphore     sem_present[MAX_SWAP];
    VkFence         fence[MG_FRAMES_IN_FLIGHT];
    uint32_t        frame_slot, frame_serial, acquired_img;
    bool            slot_ready[MG_FRAMES_IN_FLIGHT];

    // objects
    PassVk passes[MG_MAX_PASSES];  uint32_t n_pass;
    PipeVk pipes[MG_MAX_PIPES];    uint32_t n_pipe;
    MatVk  mats[MG_MAX_MATERIALS]; uint32_t n_mat;
    MeshVk meshes[MG_MAX_MESHES];  uint32_t n_mesh;
    TexVk  texs[MG_MAX_MATERIALS]; uint32_t n_tex;
    MgStateDesc states[MG_MAX_STATES]; uint32_t n_state;
    TexVk  dummy_white;
    // Small device-local images share one allocation: one VkDeviceMemory per
    // 1x1 texture is the "sub-allocate small resources" best-practice warning.
    struct { VkDeviceMemory mem; VkDeviceSize size, used; } img_pool[4];
    uint32_t img_pool_n;
    bool   have_store_none;
    bool   have_validation;          // VK_LAYER_KHRONOS_validation is loaded
    bool   have_debug_utils;
    VkDebugUtilsMessengerEXT dbg;    // VK_EXT_debug_utils sink, if any
    VkSurfaceTransformFlagBitsKHR swap_pre;
    uint32_t rotation_deg;

    // frame state
    MgDrawList *coll;
    MgStats     stats;
    uint32_t    params_used;
    bool        recording, in_pass;
    uint32_t    ord;                 // render passes opened this frame
    MgPass      ord_pass[MG_MAX_PASSES];
    uint32_t    ord_first[MG_MAX_PASSES];
    uint32_t    ord_count[MG_MAX_PASSES];
    uint32_t    cur_pass_ord;
    double      t_frame0;
};

struct MgFrame { MgDevice *d; };

static MgFrame g_frame;

// ============================================================
// 2. FORMAT AND VALUE TABLES
// ============================================================

static VkFormat fmt_of(MgFmt f)
{
    static const VkFormat t[MG_FMT_COUNT] = {
        [MG_FMT_NONE]      = VK_FORMAT_UNDEFINED,
        [MG_RGBA8]         = VK_FORMAT_R8G8B8A8_UNORM,
        [MG_RGBA8_SRGB]    = VK_FORMAT_R8G8B8A8_SRGB,
        [MG_BGRA8]         = VK_FORMAT_B8G8R8A8_UNORM,
        [MG_R8]            = VK_FORMAT_R8_UNORM,
        [MG_R8G8]          = VK_FORMAT_R8G8_UNORM,
        [MG_RGBA8_SNORM]   = VK_FORMAT_R8G8B8A8_SNORM,
        [MG_R11G11B10]     = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        [MG_RGBA16F]       = VK_FORMAT_R16G16B16A16_SFLOAT,
        [MG_R16F]          = VK_FORMAT_R16_SFLOAT,
        [MG_R32F]          = VK_FORMAT_R32_SFLOAT,
        [MG_RGB10A2]       = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        [MG_A2B10G10R10]   = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        [MG_D16]           = VK_FORMAT_D16_UNORM,
        [MG_D24S8]         = VK_FORMAT_D24_UNORM_S8_UINT,
        [MG_D32F]          = VK_FORMAT_D32_SFLOAT,
        [MG_ASTC_4x4]      = VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
        [MG_ASTC_6x6]      = VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
        [MG_ETC2_RGB]      = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
        [MG_ETC2_RGBA]     = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
    };
    return t[f];
}

static VkAttachmentLoadOp load_of(MgLoad l)
{
    switch (l) {
    case MG_LOAD:      return VK_ATTACHMENT_LOAD_OP_LOAD;
    case MG_CLEAR:     return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case MG_DONT_CARE: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    default:           return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
}

// DONT_CARE and NONE are different promises. DONT_CARE says the tile is garbage
// and may be thrown away; NONE says nothing wrote to it, so there is nothing to
// write back, but do not treat the tile as invalidated. The second is what a
// depth buffer that is only tested against wants (Arm, "Understanding Render
// Passes"). NONE needs Vulkan 1.3 or VK_EXT_load_store_op_none; fall back.
static VkAttachmentStoreOp store_of(MgDevice *d, MgStore s)
{
    if (s == MG_STORE) return VK_ATTACHMENT_STORE_OP_STORE;
    if (s == MG_STORE_NONE) return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (s == MG_STORE_READONLY && d->caps.store_op_none)
        return VK_ATTACHMENT_STORE_OP_NONE;
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

static VkCompareOp cmp_of(MgCmp c)
{
    switch (c) {
    case MG_CMP_LESS:    return VK_COMPARE_OP_LESS;
    case MG_CMP_GREATER: return VK_COMPARE_OP_GREATER;
    default:             return VK_COMPARE_OP_GREATER_OR_EQUAL;
    }
}

static VkCullModeFlags cull_of(MgCull c)
{
    switch (c) {
    case MG_CULL_BACK:  return VK_CULL_MODE_BACK_BIT;
    case MG_CULL_FRONT: return VK_CULL_MODE_FRONT_BIT;
    default:            return VK_CULL_MODE_NONE;
    }
}

// ============================================================
// 3. DEVICE
// ============================================================

static VkShaderModule make_shader(MgDevice *d, const uint32_t *code, uint32_t words)
{
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = words * 4u,
        .pCode = code,
    };
    VkShaderModule m;
    VKOK(vkCreateShaderModule(d->dev, &ci, NULL, &m));
    return m;
}

static VkDeviceSize heap_alloc(MgDevice *d, VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer *out)
{
    const VkDeviceSize off = (d->heap_used + ARENA_ALIGN - 1) & ~(VkDeviceSize)(ARENA_ALIGN - 1);
    VKTRUE(off + bytes <= d->heap_cap, "heap exhausted");
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VKOK(vkCreateBuffer(d->dev, &bci, NULL, out));
    VKOK(vkBindBufferMemory(d->dev, *out, d->heap_mem, off));
    d->heap_used = off + bytes;
    return off;
}

static uint32_t find_mem_type(MgDevice *d, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(d->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    mg_fatal("no memory type for the mapped UMA heap", 0);
    return 0;
}

static void build_swapchain(MgDevice *d);
static void destroy_swap_deps(MgDevice *d);
static void build_pass(MgDevice *d, PassVk *p);

// The validation layer's messages, forwarded to logcat under our own tag so
// `adb logcat -s mg` shows them in order with the API calls that caused them.
// The layer also prints to the VALIDATION tag on its own; this is the copy that
// carries severity counters into MgStats.
static VKAPI_ATTR VkBool32 VKAPI_CALL mg_debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                                  VkDebugUtilsMessageTypeFlagsEXT type,
                                                  const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                  void *user)
{
    MgDevice *d = user;
    const bool err  = (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const bool warn = (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
    (void)type;
    if (err)       d->stats.val_errors++;
    else if (warn) d->stats.val_warnings++;
    __android_log_print(err ? ANDROID_LOG_ERROR : warn ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
                        TAG, "[validation %s] %s", err ? "ERROR" : warn ? "WARN" : "INFO",
                        data->pMessage ? data->pMessage : "?");
    return VK_FALSE;   // never abort inside the layer
}

static void create_instance(MgDevice *d, const MgDesc *desc)
{
    const char *ext_inst[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };

    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *ep = calloc(n ? n : 1, sizeof *ep);
    vkEnumerateInstanceExtensionProperties(NULL, &n, ep);
    bool has_surf = false, has_android = false, has_none = false, has_dbg_ext = false;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(ep[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME)) has_surf = true;
        if (!strcmp(ep[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) has_android = true;
        if (!strcmp(ep[i].extensionName, "VK_EXT_load_store_op_none")) has_none = true;
        if (!strcmp(ep[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) has_dbg_ext = true;
    }
    free(ep);
    VKTRUE(has_surf && has_android, "no KHR_surface / KHR_android_surface");

    // The layer is only requested when the app asked for it AND the loader can
    // see it. Asking for a layer that is not installed fails vkCreateInstance
    // with VK_ERROR_LAYER_NOT_PRESENT, which on this path means a black screen
    // and no log -- so probe first and say so.
    const char *layers[1];
    uint32_t n_layer = 0;
    if (desc->validation) {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, NULL);
        VkLayerProperties *lp = calloc(ln ? ln : 1, sizeof *lp);
        vkEnumerateInstanceLayerProperties(&ln, lp);
        for (uint32_t i = 0; i < ln; i++) {
            if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) {
                layers[0] = "VK_LAYER_KHRONOS_validation";
                n_layer = 1;
                LOGI("validation: %s, spec %u.%u.%u, impl %u", lp[i].layerName,
                     VK_VERSION_MAJOR(lp[i].specVersion), VK_VERSION_MINOR(lp[i].specVersion),
                     VK_VERSION_PATCH(lp[i].specVersion), lp[i].implementationVersion);
                break;
            }
        }
        free(lp);
        if (!n_layer)
            LOGW("validation: requested but VK_LAYER_KHRONOS_validation is not visible to the "
                 "loader -- see demo/README.md (adb push + run-as + gpu_debug_layers)");
    }
    d->have_validation = n_layer != 0;
    // The layer supplies VK_EXT_debug_utils; the loader/driver may not list it
    // before the layer is enabled, so ask for it whenever the layer is on.
    d->have_debug_utils = d->have_validation || has_dbg_ext;

    // ---- the layer's own settings: sync and best practices --------------------
    // These are BOOL32 settings on VK_LAYER_KHRONOS_validation (the keys in its
    // VkLayer_khronos_validation.json). They are passed through
    // VK_EXT_layer_settings, which the layer implements, so the app does not
    // depend on a device property or a vk_layer_settings.txt file.
    const char *kLayer = "VK_LAYER_KHRONOS_validation";
    VkBool32 on = VK_TRUE;
    VkLayerSettingEXT lset[2];
    uint32_t n_lset = 0;
    bool want_modes = d->have_validation &&
                      (desc->validation & (MG_VALIDATE_SYNC | MG_VALIDATE_BEST_PRACTICES));
    bool have_layer_settings = false;
    if (want_modes) {
        uint32_t ln = 0;
        bool found = false;
        vkEnumerateInstanceExtensionProperties(kLayer, &ln, NULL);
        VkExtensionProperties *le = calloc(ln ? ln : 1, sizeof *le);
        vkEnumerateInstanceExtensionProperties(kLayer, &ln, le);
        for (uint32_t i = 0; i < ln; i++)
            if (!strcmp(le[i].extensionName, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) found = true;
        free(le);
        have_layer_settings = found;
        if (have_layer_settings) {
            if (desc->validation & MG_VALIDATE_SYNC)
                lset[n_lset++] = (VkLayerSettingEXT){ kLayer, "validate_sync",
                                                      VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on };
            if (desc->validation & MG_VALIDATE_BEST_PRACTICES)
                lset[n_lset++] = (VkLayerSettingEXT){ kLayer, "validate_best_practices",
                                                      VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on };
        } else {
            LOGW("validation: the layer does not advertise %s; sync/best-practices not enabled. "
                 "Fallback: adb shell setprop debug.vulkan.khronos_validation.validate_sync 1",
                 VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        }
    }
    VkLayerSettingsCreateInfoEXT lsi = {
        .sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
        .pNext = NULL, .settingCount = n_lset, .pSettings = n_lset ? lset : NULL,
    };
    if (n_lset)
        LOGI("validation: modes core=%d sync=%d best_practices=%d",
             (desc->validation & MG_VALIDATE_CORE) != 0,
             (desc->validation & MG_VALIDATE_SYNC) != 0,
             (desc->validation & MG_VALIDATE_BEST_PRACTICES) != 0);

    const char *ext_inst2[5];
    uint32_t n_ext = 0;
    ext_inst2[n_ext++] = ext_inst[0];
    ext_inst2[n_ext++] = ext_inst[1];
    if (has_none)            ext_inst2[n_ext++] = "VK_EXT_load_store_op_none";
    if (d->have_debug_utils) ext_inst2[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (n_lset)              ext_inst2[n_ext++] = VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;

    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = desc->name ? desc->name : "mg",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = n_lset ? &lsi : NULL,
        .pApplicationInfo = &ai,
        .enabledLayerCount = n_layer,
        .ppEnabledLayerNames = n_layer ? layers : NULL,
        .enabledExtensionCount = n_ext,
        .ppEnabledExtensionNames = ext_inst2,
    };
    d->have_store_none = has_none;
    VkResult r = vkCreateInstance(&ci, NULL, &d->inst);

    // Enumeration is not proof of loadability: a layer can be listed and still
    // fail to load (a forced `debug.vulkan.layers`, a stale .so, a missing
    // dependency), and then vkCreateInstance returns VK_ERROR_LAYER_NOT_PRESENT.
    // Validation is a debugging aid -- it must never be the reason the app does
    // not start, so drop it and try again, loudly.
    if (r != VK_SUCCESS && (n_layer || d->have_debug_utils)) {
        LOGW("validation: instance creation failed (%s); retrying without the layer and "
             "without VK_EXT_debug_utils", vk_result_name(r));
        d->have_validation = false;
        d->have_debug_utils = has_dbg_ext;
        n_layer = 0;
        n_ext = 0;
        ext_inst2[n_ext++] = ext_inst[0];
        ext_inst2[n_ext++] = ext_inst[1];
        if (has_none)            ext_inst2[n_ext++] = "VK_EXT_load_store_op_none";
        if (d->have_debug_utils) ext_inst2[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        ci.enabledLayerCount = 0;
        ci.ppEnabledLayerNames = NULL;
        ci.enabledExtensionCount = n_ext;
        ci.pNext = NULL;          // the settings name a layer that is no longer enabled
        r = vkCreateInstance(&ci, NULL, &d->inst);
    }
    VKOK(r);

    if (d->have_validation && d->have_debug_utils) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(d->inst, "vkCreateDebugUtilsMessengerEXT");
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT mi = {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = mg_debug_cb,
                .pUserData = d,
            };
            if (create(d->inst, &mi, NULL, &d->dbg) != VK_SUCCESS) d->dbg = VK_NULL_HANDLE;
        }
        LOGI("validation: errors and warnings now reach logcat as '[validation ERROR] ...'");
    }
}

static void create_device(MgDevice *d, const MgDesc *desc)
{
    uint32_t n = 0;
    VKOK(vkEnumeratePhysicalDevices(d->inst, &n, NULL));
    VKTRUE(n > 0, "no Vulkan device");
    VkPhysicalDevice pds[8];
    if (n > 8) n = 8;
    VKOK(vkEnumeratePhysicalDevices(d->inst, &n, pds));
    d->phys = pds[0];
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
            strstr(p.deviceName, "Adreno") || strstr(p.deviceName, "Mali")) {
            d->phys = pds[i];
            break;
        }
    }
    vkGetPhysicalDeviceProperties(d->phys, &d->props);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &qn, qf);
    d->qfam = ~0u;
    for (uint32_t i = 0; i < qn; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { d->qfam = i; break; }
    free(qf);
    VKTRUE(d->qfam != ~0u, "no graphics queue");

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = d->qfam,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    const char *ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = ext,
    };
    VKOK(vkCreateDevice(d->phys, &dci, NULL, &d->dev));
    vkGetDeviceQueue(d->dev, d->qfam, 0, &d->queue);

    // ---- what the api may branch on (once, at setup) ----
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(d->phys, &f);
    d->caps.color_attachments = d->props.limits.maxColorAttachments;
    d->caps.push_bytes = d->props.limits.maxPushConstantsSize;
    d->caps.sampled_images = d->props.limits.maxPerStageDescriptorSampledImages;
    d->caps.workgroup_invocations = d->props.limits.maxComputeWorkGroupInvocations;
    d->caps.workgroup_shared = d->props.limits.maxComputeSharedMemorySize;
    d->caps.subgroup_size = 32;                 /* reported by adb in the log */
    d->caps.indirect_max = d->props.limits.maxDrawIndirectCount;
    d->caps.indirect_single = true;
    d->caps.store_op_none = d->have_store_none || d->props.apiVersion >= VK_API_VERSION_1_3;
    d->caps.descriptor_buffer = false;
    d->caps.dynamic_rendering = false;
    d->caps.subpass_input = true;
    d->caps.astc = f.textureCompressionASTC_LDR;
    d->caps.texture_slots = MG_TEXTURE_SLOTS;
    (void)desc;
}

static void create_heap(MgDevice *d, const MgDesc *desc)
{
    const VkDeviceSize cap = desc->arena_bytes ? desc->arena_bytes : ARENA_BYTES;
    VkBufferCreateInfo probe = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = cap,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer b;
    VKOK(vkCreateBuffer(d->dev, &probe, NULL, &b));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d->dev, b, &req);
    vkDestroyBuffer(d->dev, b, NULL);

    // One allocation, one mapping, for everything the CPU writes.
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = cap,
        .memoryTypeIndex = find_mem_type(d, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VKOK(vkAllocateMemory(d->dev, &ai, NULL, &d->heap_mem));
    VKOK(vkMapMemory(d->dev, d->heap_mem, 0, VK_WHOLE_SIZE, 0, &d->heap_map));
    d->heap_cap = cap;

    d->vbo_pos_off  = heap_alloc(d, 1u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &d->vbo_pos);
    d->vbo_attr_off = heap_alloc(d, 1u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &d->vbo_attr);
    d->ibo_off      = heap_alloc(d, 1u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  &d->ibo_arena);
    d->params_off   = heap_alloc(d, MG_FRAMES_IN_FLIGHT * PARAMS_BYTES,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &d->params);
    d->ubo_off      = heap_alloc(d, MG_FRAMES_IN_FLIGHT * UBO_BYTES,
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &d->ubo);
    d->pos_cap = (1u << 20) / 12u;
    d->idx_cap = (1u << 20) / 2u;
    LOGI("heap: %.1f MB mapped, %.1f KB used", cap / 1048576.0, d->heap_used / 1024.0);
}

static void create_descriptors(MgDevice *d)
{
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    VKOK(vkCreateSampler(d->dev, &sci, NULL, &d->sampler_linear));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    VKOK(vkCreateSampler(d->dev, &sci, NULL, &d->sampler_point));

    // set 0: the pass-wide state. One UBO for the frame, one storage buffer
    // for the parameter ring. No dynamic offsets: the per-frame slot is baked
    // into the descriptor at write time, so a draw binds nothing.
    VkDescriptorSetLayoutBinding b0[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo l0 = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = b0,
    };
    VKOK(vkCreateDescriptorSetLayout(d->dev, &l0, NULL, &d->set0_layout));

    // set 1: the material. Four combined image samplers, exactly, always.
    VkDescriptorSetLayoutBinding b1 = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = MG_TEXTURE_SLOTS,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo l1 = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b1,
    };
    VKOK(vkCreateDescriptorSetLayout(d->dev, &l1, NULL, &d->set1_layout));

    VkDescriptorSetLayout sets[2] = { d->set0_layout, d->set1_layout };
    VkPipelineLayoutCreateInfo pl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2, .pSetLayouts = sets,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &(VkPushConstantRange){
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0, .size = MG_PUSH_BYTES,
        },
    };
    VKOK(vkCreatePipelineLayout(d->dev, &pl, NULL, &d->playout));

    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MG_FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MG_FRAMES_IN_FLIGHT },
    };
    VkDescriptorPoolSize mat_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      MG_MAX_MATERIALS * MG_TEXTURE_SLOTS };
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MG_FRAMES_IN_FLIGHT + MG_MAX_MATERIALS,
        .poolSizeCount = 3,
        .pPoolSizes = (VkDescriptorPoolSize[]){ sizes[0], sizes[1], mat_size },
    };
    VKOK(vkCreateDescriptorPool(d->dev, &pci, NULL, &d->dpool));
}

static void write_frame_sets(MgDevice *d)
{
    for (uint32_t i = 0; i < MG_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = d->dpool,
            .descriptorSetCount = 1,
            .pSetLayouts = &d->set0_layout,
        };
        VKOK(vkAllocateDescriptorSets(d->dev, &ai, &d->set0[i]));
        // The offsets here are relative to the BUFFER, not to the heap. The
        // buffers are already bound at ubo_off/params_off inside heap_mem, so
        // adding the heap offset here addresses past the end of a 512-byte
        // buffer: the descriptor is out of range and the shader reads zeros.
        // A zero viewproj collapses every vertex to w = 0, i.e. no fragments.
        VkDescriptorBufferInfo ubo = { d->ubo, i * UBO_BYTES, UBO_BYTES };
        VkDescriptorBufferInfo par = { d->params, i * PARAMS_BYTES, PARAMS_BYTES };
        VkWriteDescriptorSet w[2] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set0[i],
              .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              .pBufferInfo = &ubo },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set0[i],
              .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              .pBufferInfo = &par },
        };
        vkUpdateDescriptorSets(d->dev, 2, w, 0, NULL);
    }
}

static void create_sync(MgDevice *d)
{
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        // RESET_COMMAND_BUFFER_BIT is not optional here: mg_frame() resets the
        // slot's command buffer every frame, and without the flag that is
        // VUID-vkResetCommandBuffer-commandBuffer-00046 (undefined behaviour,
        // silently fine on several drivers).
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = d->qfam,
    };
    VKOK(vkCreateCommandPool(d->dev, &pci, NULL, &d->cmd_pool));
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = d->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MG_FRAMES_IN_FLIGHT,
    };
    VKOK(vkAllocateCommandBuffers(d->dev, &ai, d->cmd));

    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                             .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    for (uint32_t i = 0; i < MG_FRAMES_IN_FLIGHT; i++) {
        VKOK(vkCreateSemaphore(d->dev, &si, NULL, &d->sem_acquire[i]));
        VKOK(vkCreateFence(d->dev, &fi, NULL, &d->fence[i]));
    }
    for (uint32_t i = 0; i < MAX_SWAP; i++)
        VKOK(vkCreateSemaphore(d->dev, &si, NULL, &d->sem_present[i]));
}

MgDevice *mg_create(const MgDesc *desc)
{
    MgDevice *d = calloc(1, sizeof *d);
    d->coll = calloc(1, sizeof(MgDrawList));
    create_instance(d, desc);
    create_device(d, desc);
    create_heap(d, desc);
    create_descriptors(d);
    write_frame_sets(d);
    create_sync(d);
    mg_coll_clear(d->coll);

    // A 1x1 white texture so a material slot may be left empty.
    const uint32_t white = 0xffffffffu;
    MgTexDesc td = { .width = 1, .height = 1, .layers = 1, .mips = 1, .format = MG_RGBA8 };
    MgTex t = mg_texture(d, &td, &white, 4);
    d->dummy_white = d->texs[t.index - 1];   // mg_texture stores at index-1
    d->n_tex = 0;   // the dummy is not addressable

    LOGI("device '%s', %08x, vulkan %u.%u.%u", d->props.deviceName, d->props.deviceID,
         VK_VERSION_MAJOR(d->props.apiVersion), VK_VERSION_MINOR(d->props.apiVersion),
         VK_VERSION_PATCH(d->props.apiVersion));
    LOGI("caps: store_op_none=%d, preRotate=%d, rotation=%u deg",
         (int)d->caps.store_op_none, (int)d->caps.pre_rotate, d->rotation_deg);
    LOGI("caps: push=%u B, sampled=%u, colour=%u, indirectMax=%u, astc=%d, invocations=%u",
         d->caps.push_bytes, d->caps.sampled_images, d->caps.color_attachments,
         d->caps.indirect_max, (int)d->caps.astc, d->caps.workgroup_invocations);
    return d;
}

MgCaps mg_caps(const MgDevice *d) { return d->caps; }

// ============================================================
// 4. TEXTURES
// ============================================================

static uint32_t mem_type_for(MgDevice *d, VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(d->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return ~0u;
}

static void one_shot(MgDevice *d, void (*fn)(MgDevice *, VkCommandBuffer, void *), void *user)
{
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = d->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cb;
    VKOK(vkAllocateCommandBuffers(d->dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKOK(vkBeginCommandBuffer(cb, &bi));
    fn(d, cb, user);
    VKOK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    VKOK(vkQueueSubmit(d->queue, 1, &si, VK_NULL_HANDLE));
    VKOK(vkQueueWaitIdle(d->queue));
    vkFreeCommandBuffers(d->dev, d->cmd_pool, 1, &cb);
}

typedef struct UploadCtx {
    TexVk *t; const void *data; uint64_t bytes; VkBuffer stage;
} UploadCtx;

static void upload_tex_cb(MgDevice *d, VkCommandBuffer cb, void *user)
{
    (void)d;
    UploadCtx *u = user;
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = u->t->img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy c = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { u->t->d.width, u->t->d.height, 1 },
    };
    vkCmdCopyBufferToImage(cb, u->stage, u->t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    VkImageMemoryBarrier b2 = b;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b2);
}

// Bump-allocate a small image from a shared device-local block, falling back to
// a dedicated allocation when the pool cannot serve the request.
static bool pool_image_alloc(MgDevice *d, const VkMemoryRequirements *req,
                            VkDeviceMemory *out_mem, VkDeviceSize *out_off)
{
    const VkDeviceSize align = req->alignment > ARENA_ALIGN ? req->alignment : ARENA_ALIGN;
    for (uint32_t i = 0; i < d->img_pool_n; i++) {
        const VkDeviceSize o = (d->img_pool[i].used + align - 1) & ~(align - 1);
        if (o + req->size <= d->img_pool[i].size) {
            *out_mem = d->img_pool[i].mem;
            *out_off = o;
            d->img_pool[i].used = o + req->size;
            return true;
        }
    }
    if (d->img_pool_n >= 4) return false;
    const VkDeviceSize sz = req->size > (2u << 20) ? req->size : (2u << 20);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = sz,
        .memoryTypeIndex = mem_type_for(d, *req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    if (ai.memoryTypeIndex == ~0u) return false;
    VkDeviceMemory m;
    VKOK(vkAllocateMemory(d->dev, &ai, NULL, &m));
    d->img_pool[d->img_pool_n].mem  = m;
    d->img_pool[d->img_pool_n].size = sz;
    d->img_pool[d->img_pool_n].used = req->size;
    d->img_pool_n++;
    *out_mem = m;
    *out_off = 0;
    return true;
}

MgTex mg_texture(MgDevice *d, const MgTexDesc *desc, const void *data, uint64_t bytes)
{
    VKTRUE(d->n_tex < MG_MAX_MATERIALS, "too many textures");
    TexVk *t = &d->texs[d->n_tex];
    memset(t, 0, sizeof *t);
    t->d = *desc;

    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt_of(desc->format),
        .extent = { desc->width, desc->height, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
               | ((desc->flags & MG_TEX_RENDER)  ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0)
               | ((desc->flags & MG_TEX_STORAGE) ? VK_IMAGE_USAGE_STORAGE_BIT        : 0),
    };
    // Arm: an image is AFBC-compressible on TILING_OPTIMAL only if it does NOT
    // have STORAGE_BIT (nor TRANSIENT/FSR/ALIAS, nor MUTABLE_FORMAT pre-Valhall).
    // Half of the framebuffer bandwidth is not worth a convenience flag.
    VKTRUE(!((desc->flags & MG_TEX_RENDER) && (desc->flags & MG_TEX_STORAGE)),
           "MG_TEX_RENDER|MG_TEX_STORAGE: a render target is never a storage image (AFBC)");
    VKOK(vkCreateImage(d->dev, &ci, NULL, &t->img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d->dev, t->img, &req);
    if (!pool_image_alloc(d, &req, &t->mem, &t->mem_off)) {
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = mem_type_for(d, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        VKTRUE(ai.memoryTypeIndex != ~0u, "no device-local memory for a texture");
        VKOK(vkAllocateMemory(d->dev, &ai, NULL, &t->mem));
        t->mem_off = 0;
        t->owns_mem = true;
    }
    VKOK(vkBindImageMemory(d->dev, t->img, t->mem, t->mem_off));

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = ci.format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VKOK(vkCreateImageView(d->dev, &vci, NULL, &t->view));

    if (data && bytes) {
        // The staging buffer comes from the mapped heap, not its own allocation:
        // a 4-byte VkDeviceMemory is exactly what best-practices validation
        // warns about, and the heap is already host-visible and mapped.
        VkBuffer stage;
        const VkDeviceSize stage_off = heap_alloc(d, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage);
        memcpy((uint8_t *)d->heap_map + stage_off, data, bytes);

        UploadCtx u = { t, data, bytes, stage };
        one_shot(d, upload_tex_cb, &u);
        // No destroy: the heap is freed once, in mg_destroy(). Uploads happen at
        // frame boundaries, and this backend uploads only at startup.
    }

    t->owned = true;
    MgTex h = { .index = ++d->n_tex, .gen = 1 };
    return h;
}

// Rewritable: after a surface rebuild the pass outputs are new images, so every
// material is pointed at the new views. The handle never changes.
static void write_material_set(MgDevice *d, VkDescriptorSet set, const MgMatDesc *m)
{
    VkDescriptorImageInfo ii[MG_TEXTURE_SLOTS];
    for (uint32_t i = 0; i < MG_TEXTURE_SLOTS; i++) {
        const TexVk *t = m->tex[i].index ? &d->texs[m->tex[i].index - 1] : &d->dummy_white;
        ii[i].imageView = t->view;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii[i].sampler = (m->samplers[i] & 1) ? d->sampler_point : d->sampler_linear;
    }
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .descriptorCount = MG_TEXTURE_SLOTS,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = ii,
    };
    vkUpdateDescriptorSets(d->dev, 1, &w, 0, NULL);
}

static VkDescriptorSet alloc_material_set(MgDevice *d)
{
    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = d->dpool, .descriptorSetCount = 1, .pSetLayouts = &d->set1_layout,
    };
    VkDescriptorSet set;
    VKOK(vkAllocateDescriptorSets(d->dev, &ai, &set));
    return set;
}

// A pass output gets a STABLE slot in the texture table, so a material handle
// keeps working across surface recreation.
static uint32_t tex_register(MgDevice *d, uint32_t reuse, VkImage img, VkImageView view, MgTexDesc td)
{
    const uint32_t idx = reuse ? reuse : ++d->n_tex;
    TexVk *t = &d->texs[idx - 1];
    VkDeviceMemory owned_mem = t->owned ? t->mem : VK_NULL_HANDLE;
    memset(t, 0, sizeof *t);
    t->img = img; t->view = view; t->d = td; t->owned = false; t->mem = owned_mem;
    return idx;
}

// ============================================================
// 5. PASSES  (built once; the frame never describes a pass again)
// ============================================================

// A depth/stencil image view must name the depth aspect. COLOR_BIT on a D24S8
// image is a spec violation (VUID-VkImageViewCreateInfo-format); a driver that
// honours the view's aspect then cannot use it as a depth attachment.
static VkImageAspectFlags aspect_of(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// Can this attachment live entirely in tile memory? A transient image is
// required to be backed by lazily-allocated memory and may not be sampled,
// stored, copied or read back -- exactly the contract of "cleared or drawn
// over, never stored". Both conditions are probed rather than assumed.
static bool lazy_supported(MgDevice *d, VkFormat fmt, VkImageUsageFlags usage)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(d->phys, &mp);
    bool any_lazy = false;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) any_lazy = true;
    if (!any_lazy) return false;

    VkImageFormatProperties fp;
    return vkGetPhysicalDeviceImageFormatProperties(
               d->phys, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
               usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, 0, &fp) == VK_SUCCESS;
}

static void pass_image(MgDevice *d, PassVk *p, uint32_t slot, VkFormat fmt,
                       uint32_t w, uint32_t h, VkSampleCountFlagBits samples,
                       VkImageUsageFlags usage, bool transient)
{
    bool lazy = false;
    if (transient && lazy_supported(d, fmt, usage)) {
        usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        lazy = true;
    }
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VKOK(vkCreateImage(d->dev, &ci, NULL, &p->img[slot]));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d->dev, p->img[slot], &req);
    uint32_t mt = ~0u;
    if (lazy) mt = mem_type_for(d, req, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    if (mt == ~0u) mt = mem_type_for(d, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKTRUE(mt != ~0u, "no memory type for a pass attachment");
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    VKOK(vkAllocateMemory(d->dev, &ai, NULL, &p->mem[slot]));
    VKOK(vkBindImageMemory(d->dev, p->img[slot], p->mem[slot], 0));

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = p->img[slot],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = { aspect_of(fmt), 0, 1, 0, 1 },
    };
    vkDestroyImageView(d->dev, p->view[slot], NULL);
    VKOK(vkCreateImageView(d->dev, &vci, NULL, &p->view[slot]));
}

static uint32_t msaa4(MgDevice *d)
{
    VkImageFormatProperties fp;
    VkResult r = vkGetPhysicalDeviceImageFormatProperties(
        d->phys, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        0, &fp);
    return (r == VK_SUCCESS && (fp.sampleCounts & VK_SAMPLE_COUNT_4_BIT)) ? 4 : 1;
}

static void build_pass(MgDevice *d, PassVk *p)
{
    const MgPassDesc *desc = &p->d;
    const uint32_t w = desc->width  ? desc->width  : (d->swap_ext.width  ? d->swap_ext.width  : 1080);
    const uint32_t h = desc->height ? desc->height : (d->swap_ext.height ? d->swap_ext.height : 2400);
    const bool depth = desc->depth != MG_FMT_NONE;

    uint8_t samples = desc->samples ? desc->samples : 1;
    if (samples > 1) samples = (uint8_t)msaa4(d);
    const bool resolve = desc->resolve && samples > 1;

    VkAttachmentDescription att[4];
    VkAttachmentReference col_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference res_ref = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference d_ref   = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    uint32_t n = 0;
    p->n_img = 0;

    const bool to_swap = (desc->color[0].fmt == MG_SWAPCHAIN);

    if (to_swap) {
        // final pass: draw straight into the swapchain images
        att[n] = (VkAttachmentDescription){
            .format = d->swap_fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = load_of(desc->color[0].load), .storeOp = store_of(d, desc->color[0].store),
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        };
        col_ref.attachment = n++;
    } else {
        // Resolved in-pass: the multisampled image is never read again, so it
        // is not stored (STORE was quietly writing the 4x surface to memory
        // every frame), needs no TRANSFER_SRC, and can be transient.
        // A pass whose colour is not resolved IS its output: mg_pass_output()
        // hands the application that image view, and a material samples it in
        // a later pass. It therefore has to carry SAMPLED -- the attachment
        // finishes the pass in SHADER_READ_ONLY, and an image view without
        // VK_IMAGE_USAGE_SAMPLED_BIT may not be written through that layout
        // (VUID-VkRenderPassBeginInfo-framebuffer-00601). The multisampled
        // image below is the opposite case: transient, never sampled.
        VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (!resolve) u |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        pass_image(d, p, 0, fmt_of(desc->color[0].fmt), w, h,
                   samples > 1 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT, u, resolve);
        att[n] = (VkAttachmentDescription){
            .format = fmt_of(desc->color[0].fmt),
            .samples = samples > 1 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,
            .loadOp = load_of(desc->color[0].load),
            .storeOp = resolve ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                               : store_of(d, desc->color[0].store),
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = resolve ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        col_ref.attachment = n++;
        if (resolve) {
            // the resolve target: in-pass, never vkCmdResolveImage
            pass_image(d, p, 1, fmt_of(desc->color[0].fmt), w, h, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false);
            att[n] = (VkAttachmentDescription){
                .format = fmt_of(desc->color[0].fmt), .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            res_ref.attachment = n++;
        }
    }
    if (depth) {
        VkFormat df = fmt_of(desc->depth);
        pass_image(d, p, 2, df, w, h, samples > 1 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   desc->depth_store != MG_STORE);   // never stored: tile memory only
        att[n] = (VkAttachmentDescription){
            .format = df,
            .samples = samples > 1 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,
            .loadOp = load_of(desc->depth_load), .storeOp = store_of(d, desc->depth_store),
            .stencilLoadOp = load_of(desc->depth_load),
            .stencilStoreOp = store_of(d, desc->depth_store),
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        p->depth_att = n;
        p->has_depth = true;
        d_ref.attachment = n++;
    } else {
        p->has_depth = false;
    }
    p->n_img = n;

    VkSubpassDescription sp = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &col_ref,
        .pResolveAttachments = resolve ? &res_ref : NULL,
        .pDepthStencilAttachment = depth ? &d_ref : NULL,
    };
    // The pass carries the synchronisation, so this dependency has to cover the
    // PREVIOUS frame as well: the depth and MSAA attachments were written by the
    // previous frame's pass, and the resolve target was SAMPLED by its composite.
    // srcAccessMask = 0 orders nothing -- syncval reports that as
    // "WRITE_AFTER_WRITE hazard" at the next vkCmdBeginRenderPass, which is a
    // real cross-frame hazard even when the contents are cleared anyway.
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_READ_BIT,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT },
    };
    VkRenderPassCreateInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = n, .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sp,
        .dependencyCount = to_swap ? 1 : 2, .pDependencies = dep,
    };
    VKOK(vkCreateRenderPass(d->dev, &rp, NULL, &p->rp));

    // framebuffers: one for an owned image set, one per swapchain image otherwise
    const uint32_t count = to_swap ? d->swap_n : 1;
    for (uint32_t i = 0; i < count; i++) {
        VkImageView views[4];
        uint32_t k = 0;
        if (to_swap) views[k++] = d->swap_view[i];
        else views[k++] = p->view[0];
        if (!to_swap && resolve) views[k++] = p->view[1];
        if (depth) views[k++] = p->view[2];
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->rp, .attachmentCount = k, .pAttachments = views,
            .width = to_swap ? d->swap_ext.width : w,
            .height = to_swap ? d->swap_ext.height : h,
            .layers = 1,
        };
        VKOK(vkCreateFramebuffer(d->dev, &fci, NULL, &p->fb[i]));
    }
    p->fb_count = count;
    p->built = true;

    if (!to_swap) {
        const uint32_t out_slot = resolve ? 1u : 0u;
        MgTexDesc td = { .width = w, .height = h, .layers = 1, .mips = 1,
                         .format = desc->color[0].fmt };
        p->out_tex = tex_register(d, p->out_tex, p->img[out_slot], p->view[out_slot], td);
    }
    LOGI("pass '%s': %u attachments, %dx%d, %ux MSAA, resolve=%d, depth=%d",
         desc->name ? desc->name : "?", n, w, h, samples, (int)resolve, (int)depth);
}

MgPass mg_pass(MgDevice *d, const MgPassDesc *desc)
{
    VKTRUE(d->n_pass < MG_MAX_PASSES, "too many passes");
    PassVk *p = &d->passes[d->n_pass];
    memset(p, 0, sizeof *p);
    p->d = *desc;
    MgPass h = { .index = ++d->n_pass, .gen = 1 };
    if (d->have_surface && (desc->width == 0 || desc->color[0].fmt == MG_SWAPCHAIN))
        build_pass(d, p);          /* needs the swapchain extent */
    return h;
}

static void mg_rebuild_passes(MgDevice *d)
{
    for (uint32_t i = 0; i < d->n_pass; i++) {
        PassVk *p = &d->passes[i];
        if (p->d.width == 0 || p->d.color[0].fmt == MG_SWAPCHAIN) build_pass(d, p);
    }
    for (uint32_t i = 0; i < d->n_mat; i++)
        write_material_set(d, d->mats[i].set, &d->mats[i].d);
}

// ============================================================
// 6. PIPES  (thin: shaders, formats, samples; state selects a variant)
// ============================================================

MgPipe mg_pipe(MgDevice *d, const MgPipeDesc *desc)
{
    VKTRUE(d->n_pipe < MG_MAX_PIPES, "too many pipes");
    PipeVk *p = &d->pipes[d->n_pipe];
    memset(p, 0, sizeof *p);
    p->d = *desc;
    MgPipe h = { .index = ++d->n_pipe, .gen = 1 };
    return h;
}

MgState mg_state(MgDevice *d, const MgStateDesc *desc)
{
    VKTRUE(d->n_state < MG_MAX_STATES, "too many states");
    d->states[d->n_state] = *desc;
    MgState h = { .index = ++d->n_state, .gen = 1 };
    return h;
}

MgMat mg_material(MgDevice *d, const MgMatDesc *desc)
{
    VKTRUE(d->n_mat < MG_MAX_MATERIALS, "too many materials");
    MatVk *m = &d->mats[d->n_mat];
    m->d = *desc;
    m->set = alloc_material_set(d);
    write_material_set(d, m->set, desc);
    MgMat h = { .index = ++d->n_mat, .gen = 1 };
    return h;
}

MgMesh mg_mesh(MgDevice *d, const MgMeshDesc *desc)
{
    VKTRUE(d->n_mesh < MG_MAX_MESHES, "too many meshes");
    MeshVk *m = &d->meshes[d->n_mesh];
    memset(m, 0, sizeof *m);

    if (!desc->positions) {
        // no vertex data: attribute-less mesh (fullscreen triangle).
        // Still needs an index buffer: generate [0, 1, 2, ...] so
        // vkCmdDrawIndexed has something to read instead of zeros,
        // which would collapse every vertex to the same point.
        VKTRUE(d->idx_used + desc->index_count <= d->idx_cap, "index arena full");
        const uint32_t istride = desc->indices_u32 ? 4u : 2u;
        uint8_t *ix = (uint8_t *)d->heap_map + d->ibo_off +
                      (size_t)d->idx_used * istride;
        for (uint32_t i = 0; i < desc->index_count; i++)
            if (desc->indices_u32) ((uint32_t *)ix)[i] = i;
            else                   ((uint16_t *)ix)[i] = (uint16_t)i;
        m->first_index   = d->idx_used;
        m->index_count   = desc->index_count;
        m->vertex_offset = 0;
        d->idx_used += desc->index_count;
    } else {
        const uint32_t stride_p = 12, stride_a = desc->attribute_stride;
        VKTRUE(d->pos_verts + desc->vertex_count <= d->pos_cap, "vertex arena full");
        uint8_t *p = (uint8_t *)d->heap_map + d->vbo_pos_off + (size_t)d->pos_verts * stride_p;
        memcpy(p, desc->positions, (size_t)desc->vertex_count * stride_p);
        if (desc->attributes && stride_a) {
            uint8_t *a = (uint8_t *)d->heap_map + d->vbo_attr_off + (size_t)d->pos_verts * stride_a;
            memcpy(a, desc->attributes, (size_t)desc->vertex_count * stride_a);
        }
        VKTRUE(d->idx_used + desc->index_count <= d->idx_cap, "index arena full");
        uint8_t *ix = (uint8_t *)d->heap_map + d->ibo_off +
                      (size_t)d->idx_used * (desc->indices_u32 ? 4u : 2u);
        memcpy(ix, desc->indices, (size_t)desc->index_count * (desc->indices_u32 ? 4u : 2u));

        m->first_index   = d->idx_used;
        m->index_count   = desc->index_count;
        m->vertex_offset = (int32_t)d->pos_verts;
        m->base_vertex   = d->pos_verts;
        d->pos_verts    += desc->vertex_count;
        d->idx_used += desc->index_count;
    }
    MgMesh h = { .index = ++d->n_mesh, .gen = 1 };
    return h;
}

static VkPipeline build_pipe(MgDevice *d, uint32_t pi, uint32_t si)
{
    PipeVk *p = &d->pipes[pi];
    const MgStateDesc *st = &d->states[si - 1];   // si is a state HANDLE (1-based)
    PassVk *pass = &d->passes[p->d.pass.index - 1];

    VkShaderModule vs = make_shader(d, p->d.vs.spirv, p->d.vs.words);
    VkShaderModule fs = make_shader(d, p->d.fs.spirv, p->d.fs.words);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };

    VkVertexInputBindingDescription binds[2] = {
        { .binding = 0, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
        { .binding = 1, .stride = 8,  .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
    };
    VkVertexInputAttributeDescription attrs[2] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 1, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = 0 },
    };
    const uint32_t nb = p->d.streams;

    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = nb,
        .pVertexBindingDescriptions = nb ? binds : NULL,
        .vertexAttributeDescriptionCount = nb,
        .pVertexAttributeDescriptions = nb ? attrs : NULL,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = p->d.topology == MG_TRIANGLE_STRIP ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = cull_of(st->cull),
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = p->d.samples > 1 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,
        .alphaToCoverageEnable = p->d.alpha_to_coverage,
    };
    VkPipelineDepthStencilStateCreateInfo dss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = st->depth != MG_DEPTH_NONE,
        .depthWriteEnable = st->depth == MG_DEPTH_READ_WRITE,
        .depthCompareOp = cmp_of(st->cmp),
    };
    VkPipelineColorBlendAttachmentState cba = {
        .colorWriteMask = st->write_mask ? st->write_mask
                                         : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT),
        .blendEnable = st->blend != MG_BLEND_NONE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = st->blend == MG_BLEND_ADD ? VK_BLEND_FACTOR_ONE
                                                         : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = st->blend == MG_BLEND_ADD ? VK_BLEND_OP_ADD : VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
    };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn,
    };
    VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &dss, .pColorBlendState = &cb, .pDynamicState = &ds,
        .layout = d->playout, .renderPass = pass->rp, .subpass = 0,
    };
    VkPipeline out;
    VKOK(vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &ci, NULL, &out));
    vkDestroyShaderModule(d->dev, vs, NULL);
    vkDestroyShaderModule(d->dev, fs, NULL);
    return out;
}

void mg_warm(MgDevice *d, bool blocking)
{
    (void)blocking;
    uint32_t made = 0;
    for (uint32_t pi = 0; pi < d->n_pipe; pi++) {
        PassVk *pass = &d->passes[d->pipes[pi].d.pass.index - 1];
        if (!pass->built) continue;
        // State handles are 1-based (mg_state returns ++n_state), and a draw
        // record stores the handle, so pipe[]/have[] are indexed by the handle
        // too. Warming 0..n_state-1 left the last state without a pipeline and
        // shifted every other one onto its neighbour's variant.
        for (uint32_t si = 1; si <= d->n_state; si++) {
            if (d->pipes[pi].have[si]) continue;
            d->pipes[pi].pipe[si] = build_pipe(d, pi, si);
            d->pipes[pi].have[si] = true;
            made++;
        }
    }
    LOGI("warm: %u pipeline variants, 0 created after this point", made);
}

// ============================================================
// 7. SURFACE AND SWAPCHAIN
// ============================================================

static void destroy_swap_deps(MgDevice *d)
{
    for (uint32_t i = 0; i < d->n_pass; i++) {
        PassVk *p = &d->passes[i];
        for (uint32_t f = 0; f < p->fb_count; f++) vkDestroyFramebuffer(d->dev, p->fb[f], NULL);
        p->fb_count = 0;
        vkDestroyRenderPass(d->dev, p->rp, NULL);
        p->rp = VK_NULL_HANDLE;
        for (uint32_t k = 0; k < 4; k++) {
            vkDestroyImageView(d->dev, p->view[k], NULL);
            vkDestroyImage(d->dev, p->img[k], NULL);
            vkFreeMemory(d->dev, p->mem[k], NULL);
            p->view[k] = VK_NULL_HANDLE; p->img[k] = VK_NULL_HANDLE; p->mem[k] = VK_NULL_HANDLE;
        }
        if (p->out_tex) memset(&d->texs[p->out_tex - 1], 0, sizeof(TexVk));
        p->built = false;
    }
    for (uint32_t i = 0; i < d->n_pipe; i++)
        for (uint32_t s = 0; s < MG_MAX_STATES; s++) {
            if (d->pipes[i].have[s]) vkDestroyPipeline(d->dev, d->pipes[i].pipe[s], NULL);
            d->pipes[i].have[s] = false;
        }
}

static void build_swapchain(MgDevice *d)
{
    VkSurfaceCapabilitiesKHR caps;
    VKOK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, d->surf, &caps));

    // Pre-rotation: the DPU should never rotate a frame. If the surface lets us
    // present IDENTITY, do so and hand the rotation to the vertex shader as a
    // geometry transform; only fall back to rotating in the compositor.
    // (Arm, Vulkanised 2023: "Android Vulkan capability reporting is not useful
    // -- SurfaceFlinger's GPU fallback means everything is possible, but not
    // everything is fast.")
    VkSurfaceTransformFlagBitsKHR cur = caps.currentTransform;
    d->rotation_deg = (cur == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)  ? 90u
                    : (cur == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) ? 180u
                    : (cur == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) ? 270u : 0u;
    const VkSurfaceTransformFlagBitsKHR ident = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if (caps.supportedTransforms & ident) {
        d->swap_pre = ident;
        d->caps.pre_rotate = true;
    } else {
        d->swap_pre = cur;
        d->rotation_deg = 0u;   /* the compositor rotates; geometry stays put */
        d->caps.pre_rotate = false;
    }

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surf, &n, NULL);
    VkSurfaceFormatKHR fmts[32];
    if (n > 32) n = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, d->surf, &n, fmts);

    // linear UNORM + gamma in the composite shader: predictable on every device
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < n; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            pick = fmts[i];
            break;
        }
    d->swap_fmt = pick.format;
    d->swap_srgb = false;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xffffffffu) {
        ext.width  = d->win ? (uint32_t)ANativeWindow_getWidth(d->win) : 1080;
        ext.height = d->win ? (uint32_t)ANativeWindow_getHeight(d->win) : 2400;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    d->swap_ext = ext;

    uint32_t want = caps.minImageCount + 1;
    if (want > caps.maxImageCount && caps.maxImageCount) want = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = d->surf,
        .minImageCount = want,
        .imageFormat = d->swap_fmt,
        .imageColorSpace = pick.colorSpace,
        .imageExtent = ext,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = d->swap_pre,
        .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VKOK(vkCreateSwapchainKHR(d->dev, &ci, NULL, &d->swap));

    VKOK(vkGetSwapchainImagesKHR(d->dev, d->swap, &d->swap_n, NULL));
    if (d->swap_n > MAX_SWAP) d->swap_n = MAX_SWAP;
    VKOK(vkGetSwapchainImagesKHR(d->dev, d->swap, &d->swap_n, d->swap_img));

    for (uint32_t i = 0; i < d->swap_n; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = d->swap_img[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = d->swap_fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VKOK(vkCreateImageView(d->dev, &vci, NULL, &d->swap_view[i]));
    }
    LOGI("swapchain: %ux%u, %u images, format %d", ext.width, ext.height, d->swap_n, (int)d->swap_fmt);
}

bool mg_surface(MgDevice *d, void *native_window)
{
    d->win = native_window;
    VkAndroidSurfaceCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = (ANativeWindow *)native_window,
    };
    VKOK(vkCreateAndroidSurfaceKHR(d->inst, &ci, NULL, &d->surf));

    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(d->phys, d->qfam, d->surf, &ok);
    VKTRUE(ok, "queue family cannot present to this surface");

    d->have_surface = true;
    build_swapchain(d);
    mg_rebuild_passes(d);
    mg_warm(d, true);
    return true;
}

void mg_surface_lost(MgDevice *d)
{
    if (!d->have_surface) return;
    VKOK(vkDeviceWaitIdle(d->dev));
    destroy_swap_deps(d);
    for (uint32_t i = 0; i < d->swap_n; i++) vkDestroyImageView(d->dev, d->swap_view[i], NULL);
    vkDestroySwapchainKHR(d->dev, d->swap, NULL);
    vkDestroySurfaceKHR(d->inst, d->surf, NULL);
    d->swap = VK_NULL_HANDLE;
    d->surf = VK_NULL_HANDLE;
    d->have_surface = false;
    d->win = NULL;
}

// Degrees the application must rotate its geometry by, because the swapchain was
// created with preTransform = IDENTITY. Zero when the compositor rotates instead.
uint32_t mg_display_rotation(const MgDevice *d) { return d->rotation_deg; }

void mg_resize(MgDevice *d, uint32_t w, uint32_t h)
{
    (void)w; (void)h;
    if (!d->have_surface) return;
    VKOK(vkDeviceWaitIdle(d->dev));
    destroy_swap_deps(d);
    for (uint32_t i = 0; i < d->swap_n; i++) vkDestroyImageView(d->dev, d->swap_view[i], NULL);
    vkDestroySwapchainKHR(d->dev, d->swap, NULL);
    build_swapchain(d);
    mg_rebuild_passes(d);
    mg_warm(d, true);
}

// ============================================================
// 8. FRAME
// ============================================================

uint32_t mg_const(MgFrame *f, const void *data, uint32_t bytes)
{
    MgDevice *d = f->d;
    const uint32_t off = (d->params_used + MG_CONST_SLOT_BYTES - 1) & ~(MG_CONST_SLOT_BYTES - 1u);
    VKTRUE(off + bytes <= PARAMS_BYTES, "const ring exhausted");
    memcpy((uint8_t *)d->heap_map + d->params_off +
           d->frame_slot * PARAMS_BYTES + off, data, bytes);
    d->params_used = off + ((bytes + MG_CONST_SLOT_BYTES - 1) & ~(MG_CONST_SLOT_BYTES - 1u));
    return off;
}

void mg_frame_const(MgFrame *f, const void *data, uint32_t bytes)
{
    MgDevice *d = f->d;
    VKTRUE(bytes <= UBO_BYTES, "frame constants must fit one cache line pair");
    memcpy((uint8_t *)d->heap_map + d->ubo_off + d->frame_slot * UBO_BYTES, data, bytes);
}

MgTex mg_pass_output(MgDevice *d, MgPass pass)
{
    VKTRUE(pass.index >= 1 && pass.index <= d->n_pass, "bad pass handle");
    const uint32_t idx = d->passes[pass.index - 1].out_tex;
    VKTRUE(idx != 0, "this pass writes to the swapchain: it has no readable output");
    MgTex h = { .index = idx, .gen = 1 };
    return h;
}

bool mg_frame(MgDevice *d, MgFrame **out)
{
    if (!d->have_surface) return false;
    const uint32_t slot = d->frame_slot;
    VKOK(vkWaitForFences(d->dev, 1, &d->fence[slot], VK_TRUE, UINT64_MAX));

    uint32_t img = 0;
    VkResult r = vkAcquireNextImageKHR(d->dev, d->swap, UINT64_MAX, d->sem_acquire[slot],
                                       VK_NULL_HANDLE, &img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { mg_resize(d, 0, 0); return false; }
    VKOK(r);

    VKOK(vkResetFences(d->dev, 1, &d->fence[slot]));
    VKOK(vkResetCommandBuffer(d->cmd[slot], 0));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKOK(vkBeginCommandBuffer(d->cmd[slot], &bi));

    mg_coll_clear(d->coll);
    d->params_used = 0;
    d->recording = true;
    d->in_pass = false;
    d->ord = 0;
    d->stats.draws = 0;
    d->stats.barriers = 0;
    d->acquired_img = img;
    g_frame.d = d;
    *out = &g_frame;
    return true;
}

void mg_pass_begin(MgFrame *f, MgPass pass)
{
    MgDevice *d = f->d;
    VKTRUE(pass.index >= 1 && pass.index <= d->n_pass, "bad pass handle");
    d->cur_pass_ord = d->ord;
    d->ord_pass[d->ord] = pass;
    d->ord_first[d->ord] = d->coll->count;
    d->ord++;
    d->in_pass = true;
}

void mg_pass_end(MgFrame *f)
{
    MgDevice *d = f->d;
    VKTRUE(d->in_pass, "pass_end without pass_begin");
    d->ord_count[d->cur_pass_ord] = d->coll->count - d->ord_first[d->cur_pass_ord];
    d->in_pass = false;
}

void mg_draw(MgFrame *f, const MgDrawDesc *desc)
{
    MgDevice *d = f->d;
    VKTRUE(d->in_pass, "mg_draw outside a pass");
    VKTRUE(desc->mesh.index >= 1 && desc->mesh.index <= d->n_mesh, "bad mesh");
    MgDraw r = {
        .pipe = desc->pipe.index,
        .mat  = desc->mat.index,
        .mesh = desc->mesh.index,
        .params = desc->params,
        .instances = desc->instances ? desc->instances : 1,
        .state = (uint8_t)desc->state.index,
        .flags = (uint8_t)desc->flags,
    };
    mg_coll_draw(d->coll, &r);
    d->stats.draws++;
}

void mg_draw_batch(MgFrame *f, const MgDrawDesc *draws, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) mg_draw(f, &draws[i]);
}

// record one pass's draws, already sorted
static void record_pass(MgDevice *d, uint32_t ord, uint32_t img)
{
    PassVk *p = &d->passes[d->ord_pass[ord].index - 1];
    const uint32_t first = d->ord_first[ord], count = d->ord_count[ord];
    const uint32_t slot = d->frame_slot;

    VkClearValue clears[4];
    for (uint32_t i = 0; i < p->n_img; i++)
        clears[i] = (VkClearValue){ .color = { .float32 = { 0.04f, 0.05f, 0.07f, 1.0f } } };
    if (p->has_depth) clears[p->depth_att] = (VkClearValue){ .depthStencil = { 1.0f, 0 } };
    // clearValueCount is positional, so it must reach the LAST attachment that
    // is actually cleared -- and be 0 when nothing is (a pass that only draws
    // into the swapchain passes no clear values at all).
    uint32_t n_clear = 0;
    if (p->d.color[0].load == MG_CLEAR) n_clear = 1;
    if (p->has_depth && p->d.depth_load == MG_CLEAR && p->depth_att + 1u > n_clear)
        n_clear = p->depth_att + 1u;
    VkRenderPassBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = p->rp,
        .framebuffer = p->fb[p->fb_count > 1 ? img : 0],
        .renderArea = { { 0, 0 }, { d->swap_ext.width, d->swap_ext.height } },
        .clearValueCount = n_clear, .pClearValues = n_clear ? clears : NULL,
    };
    vkCmdBeginRenderPass(d->cmd[slot], &bi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = { 0, 0, (float)d->swap_ext.width, (float)d->swap_ext.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { d->swap_ext.width, d->swap_ext.height } };
    vkCmdSetViewport(d->cmd[slot], 0, 1, &vp);
    vkCmdSetScissor(d->cmd[slot], 0, 1, &sc);

    VkDescriptorSet s0 = d->set0[slot];
    vkCmdBindDescriptorSets(d->cmd[slot], VK_PIPELINE_BIND_POINT_GRAPHICS, d->playout,
                            0, 1, &s0, 0, NULL);
    // one vertex arena, one index arena, bound once per pass
    VkBuffer vbs[2] = { d->vbo_pos, d->vbo_attr };
    VkDeviceSize offs[2] = { 0, 0 };
    vkCmdBindVertexBuffers(d->cmd[slot], 0, 2, vbs, offs);
    vkCmdBindIndexBuffer(d->cmd[slot], d->ibo_arena, 0, VK_INDEX_TYPE_UINT16);

    uint32_t cur_pipe = 0, cur_mat = 0;
    for (uint32_t i = first; i < first + count; i++) {
        const MgDraw *r = &d->coll->draw[i];
        if (r->pipe != cur_pipe || i == first) {
            if (d->pipes[r->pipe - 1].have[r->state])
                vkCmdBindPipeline(d->cmd[slot], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  d->pipes[r->pipe - 1].pipe[r->state]);
            cur_pipe = r->pipe;
            d->stats.pipe_binds++;
        }
        if (r->mat != cur_mat || i == first) {
            VkDescriptorSet s1 = d->mats[r->mat - 1].set;
            vkCmdBindDescriptorSets(d->cmd[slot], VK_PIPELINE_BIND_POINT_GRAPHICS, d->playout,
                                    1, 1, &s1, 0, NULL);
            cur_mat = r->mat;
            d->stats.mat_binds++;
        }
        MgPush push = { .const_off = r->params, .flags = r->state };
        vkCmdPushConstants(d->cmd[slot], d->playout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, MG_PUSH_BYTES, &push);
        const MeshVk *m = &d->meshes[r->mesh - 1];
        vkCmdDrawIndexed(d->cmd[slot], m->index_count, r->instances,
                         m->first_index, m->vertex_offset, 0);
        d->stats.push_count++;
    }

    vkCmdEndRenderPass(d->cmd[slot]);
}

void mg_frame_end(MgDevice *d, MgFrame *f)
{
    (void)f;
    const uint32_t slot = d->frame_slot;
    VKTRUE(d->ord > 0, "a frame with no passes");

    // sort each pass's records, then record: the frame is a data structure
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    for (uint32_t o = 0; o < d->ord; o++)
        mg_coll_sort_range(d->coll, d->ord_first[o], d->ord_count[o]);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    d->stats.sort_us = (uint32_t)((ts1.tv_sec - ts0.tv_sec) * 1000000 +
                                  (ts1.tv_nsec - ts0.tv_nsec) / 1000);
    d->stats.passes = d->ord;

    d->stats.pipe_binds = 0;
    d->stats.mat_binds = 0;
    d->stats.push_count = 0;
    for (uint32_t o = 0; o < d->ord; o++) record_pass(d, o, d->acquired_img);

    VKOK(vkEndCommandBuffer(d->cmd[slot]));

    // The present semaphore belongs to the acquired IMAGE, not the frame slot.
    VkSemaphore *sem_pres = &d->sem_present[d->acquired_img];
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &d->sem_acquire[slot], .pWaitDstStageMask = &wait,
        .commandBufferCount = 1, .pCommandBuffers = &d->cmd[slot],
        .signalSemaphoreCount = 1, .pSignalSemaphores = sem_pres,
    };
    VKOK(vkQueueSubmit(d->queue, 1, &si, d->fence[slot]));

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = sem_pres,
        .swapchainCount = 1, .pSwapchains = &d->swap, .pImageIndices = &d->acquired_img,
    };
    VkResult r = vkQueuePresentKHR(d->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) mg_resize(d, 0, 0);
    else VKOK(r);

    d->recording = false;
    d->stats.arena_used = d->params_used;
    d->frame_slot = (d->frame_slot + 1) % MG_FRAMES_IN_FLIGHT;
    d->frame_serial++;
}

const MgStats *mg_stats(const MgDevice *d) { return &d->stats; }

// ============================================================
// 9. TEARDOWN
// ============================================================

void mg_destroy(MgDevice *d)
{
    if (!d) return;
    VKOK(vkDeviceWaitIdle(d->dev));
    mg_surface_lost(d);
    for (uint32_t i = 0; i < d->n_tex; i++) {
        if (!d->texs[i].owned) continue;
        vkDestroyImageView(d->dev, d->texs[i].view, NULL);
        vkDestroyImage(d->dev, d->texs[i].img, NULL);
        if (d->texs[i].owns_mem) vkFreeMemory(d->dev, d->texs[i].mem, NULL);
    }
    vkDestroyImageView(d->dev, d->dummy_white.view, NULL);
    vkDestroyImage(d->dev, d->dummy_white.img, NULL);
    if (d->dummy_white.owns_mem) vkFreeMemory(d->dev, d->dummy_white.mem, NULL);
    for (uint32_t i = 0; i < d->img_pool_n; i++) vkFreeMemory(d->dev, d->img_pool[i].mem, NULL);
    for (uint32_t i = 0; i < MG_FRAMES_IN_FLIGHT; i++) {
        vkDestroyFence(d->dev, d->fence[i], NULL);
        vkDestroySemaphore(d->dev, d->sem_acquire[i], NULL);
    }
    for (uint32_t i = 0; i < MAX_SWAP; i++)
        vkDestroySemaphore(d->dev, d->sem_present[i], NULL);
    if (d->dbg) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(d->inst, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(d->inst, d->dbg, NULL);
    }
    vkDestroyCommandPool(d->dev, d->cmd_pool, NULL);
    vkDestroyDescriptorPool(d->dev, d->dpool, NULL);
    vkDestroyPipelineLayout(d->dev, d->playout, NULL);
    vkDestroyDescriptorSetLayout(d->dev, d->set0_layout, NULL);
    vkDestroyDescriptorSetLayout(d->dev, d->set1_layout, NULL);
    vkDestroySampler(d->dev, d->sampler_linear, NULL);
    vkDestroySampler(d->dev, d->sampler_point, NULL);
    vkUnmapMemory(d->dev, d->heap_mem);
    vkFreeMemory(d->dev, d->heap_mem, NULL);
    vkDestroyDevice(d->dev, NULL);
    vkDestroyInstance(d->inst, NULL);
    free(d->coll);
    free(d);
}

#endif // !MG_TEST

#ifdef MG_TEST

#include <stdio.h>
#include <time.h>

static uint32_t rng_state = 0x1234567u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}

static void row(const char *label, MgCollStats s)
{
    printf("%-12s %10u %14u %14u %9u\n",
           label, s.pipe_binds, s.mat_binds, s.push_count, s.records);
}

int main(void)
{
    static MgDrawList list;
    static MgDrawList pristine;
    static MgDrawList sorted_copy;

    const uint32_t N = 10000;
    const uint32_t PIPES = 8, MATS = 64, BUCKETS = 4;

    // An engine's natural traversal order: no grouping at all. This is the
    // realistic "unsorted" case, not a worst case built to flatter the sort.
    for (uint32_t i = 0; i < N; i++) {
        MgDraw d = { 0 };
        d.pipe      = 1 + rng() % PIPES;
        d.mat       = 1 + rng() % MATS;
        d.state     = (uint8_t)(1 + rng() % BUCKETS);
        d.mesh      = 1 + rng() % 4096;
        d.params    = i;
        d.instances = 1;
        mg_coll_draw(&list, &d);
    }

    const MgCollStats unsorted = mg_coll_coalesce(&list);

    pristine = list;
    const double t0 = now_us();
    mg_coll_sort(&list);
    const double sort_us = now_us() - t0;
    sorted_copy = list;

    // Best of 20, with a restore each time, so the number is the sort and not
    // the page faults of the first touch.
    double best = 1e9;
    for (int it = 0; it < 20; it++) {
        list = pristine;
        const double a = now_us();
        mg_coll_sort(&list);
        const double e = now_us() - a;
        if (e < best) best = e;
    }

    const MgCollStats sorted = mg_coll_coalesce(&sorted_copy);

    printf("%u draws, %u pipes, %u materials, %u state buckets\n\n", N, PIPES, MATS, BUCKETS);
    printf("%-12s %10s %14s %14s %9s\n",
           "", "pipe binds", "material binds", "push changes", "records");
    row("unsorted", unsorted);
    row("sorted", sorted);
    printf("\n");

    printf("radix sort (4 x 8 bit, stable, 24 B records)\n");
    printf("  first call (cold pages): %8.1f us\n", sort_us);
    printf("  best of 20:              %8.1f us  (%.1f ns/draw)\n", best, best * 1000.0 / N);

    const uint64_t per_draw = 8 + 4;    // 8 B push + 4 B draw cmd
    printf("\ncommand stream: %u draws x %llu B = %.1f KB (%.1f B/draw, %.1f%% of the records themselves)\n",
           N, (unsigned long long)per_draw, N * (double)per_draw / 1024.0,
           (double)per_draw, 100.0 * (double)per_draw / 24.0);

    // ---- correctness ----
    int fail = 0;

    if (sorted_copy.count != pristine.count) { printf("FAIL count\n"); fail = 1; }
    if (sorted_copy.overflow || pristine.overflow) { printf("FAIL overflow\n"); fail = 1; }

    // Every record still present: checksum of the params field (unique per draw).
    uint64_t sum_a = 0, sum_b = 0, xor_a = 0, xor_b = 0;
    for (uint32_t i = 0; i < N; i++) {
        sum_a += pristine.draw[i].params; sum_b += sorted_copy.draw[i].params;
        xor_a ^= pristine.draw[i].params; xor_b ^= sorted_copy.draw[i].params;
    }
    if (sum_a != sum_b || xor_a != xor_b) { printf("FAIL multiset (sum %llu/%llu xor %llu/%llu)\n",
                                                 (unsigned long long)sum_a, (unsigned long long)sum_b,
                                                 (unsigned long long)xor_a, (unsigned long long)xor_b); fail = 1; }

    // The multiset check is necessary but not sufficient: make sure the sort
    // did not just permute. Each record must keep its pipe/mat/mesh triple.
    for (uint32_t i = 0; i < N; i++) {
        const MgDraw *d = &sorted_copy.draw[i];
        const uint32_t o = d->order;
        if (pristine.draw[o].mesh != d->mesh || pristine.draw[o].mat != d->mat ||
            pristine.draw[o].pipe != d->pipe || pristine.draw[o].params != d->params) {
            printf("FAIL record integrity at %u\n", i); fail = 1; break;
        }
    }

    // Sorted order and stability.
    uint32_t inversions = 0, unstable = 0;
    for (uint32_t i = 1; i < N; i++) {
        const uint32_t ka = mg_coll_key(&sorted_copy.draw[i - 1]);
        const uint32_t kb = mg_coll_key(&sorted_copy.draw[i]);
        if (ka > kb) inversions++;
        if (ka == kb && sorted_copy.draw[i - 1].order > sorted_copy.draw[i].order) unstable++;
    }
    if (inversions) { printf("FAIL %u inversions\n", inversions); fail = 1; }
    if (unstable)   { printf("FAIL %u unstable pairs\n", unstable); fail = 1; }

    if (!fail)
        printf("\nkey monotone: ok   stable within a bucket: ok   records intact: ok\n");

    return fail;
}

#endif // MG_TEST
