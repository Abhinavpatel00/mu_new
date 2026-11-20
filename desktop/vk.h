
#ifndef MU_VK_H
#define MU_VK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

// ============================================================
// 1. CONFIGURATION
// ============================================================

#define MAX_FRAMES_IN_FLIGHT 3
#define MAX_COLOR_ATTACHMENTS 8
#define MAX_IMAGE_USES       16
#define MAX_BUFFER_USES      16
#define MAX_PASSES           64
#define MAX_PROFILE_SCOPES   64
#define MAX_PUSH_CONSTANTS   256
#define MAX_BINDLESS_TEXTURES 65536
#define MAX_BINDLESS_SAMPLERS 256
#define MAX_BINDLESS_STORAGE 16384
#define MAX_MIPS             13
#define MAX_PIPELINES        256

#define PUSH_ALIGN 16

#ifndef ALIGNAS
    #define ALIGNAS(x) __attribute__((aligned(x)))
#endif

// Attach an arbitrary object to a command.
typedef struct DataSpan {
    const void *data;
    uint32_t size;
} DataSpan;

#define BYTE_SPAN(x) ((DataSpan){ &(x), (uint32_t)sizeof(x) })

// ============================================================
// 2. HANDLES
// ============================================================

typedef struct Gpu       Gpu;
typedef struct Pass      Pass;
typedef struct Upload    Upload;
typedef struct Readback  Readback;

typedef struct Buffer {    uint32_t index, gen; } Buffer;

typedef struct Image {    uint32_t index, gen;} Image;

typedef struct Sampler {    uint32_t index, gen;} Sampler;

typedef struct Pipe {    uint32_t index, gen;} Pipe;

#define HANDLE_NIL(T) ((T){0, 0})

#define BUFFER_NIL  HANDLE_NIL(Buffer)
#define IMAGE_NIL   HANDLE_NIL(Image)
#define SAMPLER_NIL HANDLE_NIL(Sampler)
#define PIPE_NIL    HANDLE_NIL(Pipe)

#define HANDLE_VALID(h) ((h).index != 0)

// ============================================================
// 3. TYPES
// ============================================================

typedef enum Format {
    FMT_NONE = 0,

    FMT_RGBA8_UNORM,
    FMT_RGBA8_SRGB,
    FMT_BGRA8_UNORM,
    FMT_BGRA8_SRGB,

    FMT_RGBA16F,
    FMT_RGBA32F,
    FMT_RG16F,
    FMT_RG8_UNORM,
    FMT_R32F,
    FMT_R16F,
    FMT_R8_UNORM,

    FMT_RGB10A2_UNORM,
    FMT_R11G11B10F,

    FMT_D16_UNORM,
    FMT_D24S8,
    FMT_D32F,

    FMT_R32_UINT,
    FMT_RGBA8_UINT,
    FMT_RGBA8_SNORM,

    FMT_COUNT
} Format;

typedef enum Access {
    ACC_READ,
    ACC_WRITE,
    ACC_RW
} Access;

typedef enum Above {
    DISCARD,
    KEEP
} Above;

typedef enum Load {
    LOAD,
    CLEAR,
    DONT_CARE
} Load;

typedef enum Store {
    STORE,
    STORE_DISCARD
} Store;

typedef enum Filter {
    FILT_NEAREST,
    FILT_LINEAR
} Filter;

typedef enum Address {
    ADDR_REPEAT,
    ADDR_CLAMP,
    ADDR_MIRROR,
    ADDR_CLAMP_BORDER
} Address;

typedef enum Compare {
    CMP_NONE,
    CMP_NEVER,
    CMP_LESS,
    CMP_EQUAL,
    CMP_LEQUAL,
    CMP_GREATER,
    CMP_GEQUAL,
    CMP_ALWAYS
} Compare;

typedef enum Blend {
    BLEND_OPAQUE,
    BLEND_ALPHA,
    BLEND_PREMUL,
    BLEND_ADD
} Blend;

typedef enum Cull {
    CULL_NONE,
    CULL_FRONT,
    CULL_BACK
} Cull;

typedef enum Topology {
    TOPO_TRI,
    TOPO_TRI_STRIP,
    TOPO_LINE,
    TOPO_POINT
} Topology;

typedef enum SampleCount {
    SC_1 = 1,
    SC_2 = 2,
    SC_4 = 4,
    SC_8 = 8
} SampleCount;

typedef enum PassKind {
    PASS_GRAPHICS,
    PASS_COMPUTE
} PassKind;

typedef enum ImageType {
    IMAGE_SAMPLED,
    IMAGE_STORAGE
} ImageType;

typedef enum ShaderStage {
    STAGE_VERTEX,
    STAGE_FRAGMENT,
    STAGE_COMPUTE,
    STAGE_TASK,
    STAGE_MESH
} ShaderStage;

// ============================================================
// 4. RANGES
// ============================================================

// A byte range inside a buffer.
typedef struct Range {
    Buffer buffer;
    uint64_t offset;
    uint64_t size;
} Range;

#define RANGE(b, n) \
    ((Range){ (b), 0, (uint64_t)(n) })

#define RANGE_AT(b, off, n) \
    ((Range){ (b), (off), (n) })

#define RANGE_NIL ((Range){ BUFFER_NIL, 0, 0 })

// An image subresource range.
typedef struct ImageSub {
    Image image;
    uint16_t mip, mip_count;
    uint16_t layer, layer_count;
    bool stencil_only;
} ImageSub;

#define SUB(img) \
    ((ImageSub){ .image = (img), .mip_count = 1, .layer_count = 1 })

#define SUB_MIP(img, m, n) \
    ((ImageSub){ .image = (img), .mip = (m), .mip_count = (n), .layer_count = 1 })

#define SUB_LAYERS(img, l, n) \
    ((ImageSub){ .image = (img), .layer = (l), .layer_count = (n), .mip_count = 1 })

// ============================================================
// 5. GPU
// ============================================================

typedef struct GpuDesc {
    const char *name;
    uint32_t width, height;
    bool     enable_validation;
    bool     enable_gpu_based_validation;
    bool              enable_debug_printf; /* Enable VK_KHR_shader_non_semantic_info for shader
                                              debug printf */
    uint32_t bindless_sampled_image_count;
    uint32_t bindless_sampler_count;
    uint32_t bindless_storage_image_count;
    bool     enable_pipeline_stats;
    bool     enable_graphics_profiler;

    VkDeviceSize size_of_cpu_pool;

    VkDeviceSize size_of_gpu_pool;

    VkDeviceSize size_of_staging_pool;


    bool vsync;
} GpuDesc;

typedef struct GpuInfo {
    char name[64];

    uint32_t width, height;
    uint32_t subgroup_size;

    uint64_t device_memory;
    uint64_t visible_device_memory;

    uint32_t max_compute_workgroup_invocations;
    uint32_t max_compute_shared_memory_size;

    uint32_t min_storage_buffer_offset_alignment;
    uint64_t non_coherent_atom_size;

    uint32_t max_push_constants_size;
    uint32_t max_sampled_images;
    uint32_t max_storage_images;
    uint32_t max_samplers;

    // The SampleCount values an attachment can have, or-ed together:
    // (sample_counts & SC_8) != 0. Every device has SC_1 and SC_4.
    // Integer formats may offer fewer.
    uint32_t sample_counts;

    bool buffer_device_address;
    bool descriptor_indexing;
    bool synchronization2;
    bool dynamic_rendering;
    bool draw_indirect_count;
} GpuInfo;

Gpu *gpu_create(const GpuDesc *desc);
void gpu_destroy(Gpu *g);

GpuInfo gpu_info(const Gpu *g);

VkInstance gpu_raw_instance(const Gpu *g);
bool gpu_set_surface(Gpu *g, VkSurfaceKHR surface);

// The window's size in pixels, given whenever it may have changed. Where a
// surface has no size of its own (Wayland) the frame image takes this one.
// Where it has (X11, Win32) the surface's size is used and this does nothing.
void gpu_resize(Gpu *g, uint32_t width, uint32_t height);

const char *gpu_error(const Gpu *g);
void gpu_wait_idle(Gpu *g);

// ============================================================
// 6. RESOURCES
// ============================================================

typedef enum BufferUsage {
    BUF_DEVICE   = 1u << 0,
    BUF_INDIRECT = 1u << 1,
    BUF_VERTEX   = 1u << 2,
    BUF_INDEX    = 1u << 3,
    BUF_XFER_SRC = 1u << 4,
    BUF_XFER_DST = 1u << 5
} BufferUsage;

typedef enum ImageUsage {
    IMG_SAMPLED  = 1u << 0,
    IMG_STORAGE  = 1u << 1,
    IMG_ATTACH   = 1u << 2,
    IMG_XFER_SRC = 1u << 3,
    IMG_XFER_DST = 1u << 4
} ImageUsage;

typedef struct BufferDesc {
    const char *name;
    uint64_t size;
    BufferUsage usage;
    bool host_visible;
} BufferDesc;

typedef struct ImageDesc {
    const char *name;

    uint32_t width, height;
    uint32_t depth;
    uint32_t layers;
    uint32_t mips;

    Format format;
    uint32_t usage;
    SampleCount samples;

    bool transient;

    // An image as large as the frame: leave width and height 0. frame_shift
    // halves that size so many times (1 is half the frame), down to one pixel,
    // and of `mips` the image has the ones its size has. When the frame
    // changes size the backend makes the image again under the same handle:
    // its shader indices stay as they are, what it held is gone, and so are
    // the indices of a mip the new size does not have. That happens inside
    // frame_begin() and gpu_set_surface(), never between frame_begin() and
    // frame_end().
    uint8_t frame_shift;
} ImageDesc;

typedef struct SamplerDesc {
    Filter min_filter, mag_filter, mip_filter;
    Address addr_u, addr_v, addr_w;

    float max_lod;
    float min_lod;
    float lod_bias;
    float max_aniso;

    Compare compare;
} SamplerDesc;

// General creation.
Buffer buffer_create(Gpu *g, const BufferDesc *desc);
Image image_create(Gpu *g, const ImageDesc *desc);
Sampler sampler_create(Gpu *g, const SamplerDesc *desc);

// Common creation helpers.
Buffer buffer_device(Gpu *g, uint64_t size);
Buffer buffer_host(Gpu *g, uint64_t size);

Image image_color(Gpu *g, uint32_t w, uint32_t h, Format fmt);
Image image_storage(Gpu *g, uint32_t w, uint32_t h, Format fmt);
Image image_depth(Gpu *g, uint32_t w, uint32_t h);

Image image_from_bytes(
    Gpu *g,
    uint32_t w,
    uint32_t h,
    Format fmt,
    const void *data,
    uint64_t size
);

// Resource destruction is deferred internally using the GPU timeline.
void buffer_destroy(Gpu *g, Buffer b);
void image_destroy(Gpu *g, Image img);
void sampler_destroy(Gpu *g, Sampler s);

// ============================================================
// 7. BINDLESS
// ============================================================

// Shader-visible descriptor indices.
// Sampled and storage images are separate descriptor arrays.
uint32_t image_sampled_id(const Gpu *g, Image img);
uint32_t image_storage_id(const Gpu *g, Image img);
uint32_t sampler_id(const Gpu *g, Sampler sampler);

// The sampled index of part of an image: one mip, or a range of mips or
// layers. image_sampled_id() reaches every mip of an image, so a pass that
// renders into one mip reads the others through this.
uint32_t image_sub_sampled_id(Gpu *g, ImageSub sub);

// The storage index of part of an image. A storage index reaches one mip,
// the first of its range: image_storage_id() is mip 0, and a compute pass
// that writes mip n takes the index of SUB_MIP(img, n, 1).
uint32_t image_sub_storage_id(Gpu *g, ImageSub sub);

// Buffer device address and host mapping.
typedef struct BufferInfo {
    uint64_t address;
    uint64_t size;
    void *mapped;
} BufferInfo;

BufferInfo buffer_info(const Gpu *g, Buffer b);

// Shader ABI.
enum {
    BIND_SAMPLED_IMAGES = 0,
    BIND_SAMPLERS       = 1,
    BIND_STORAGE_IMAGES = 2,
    BIND_STORAGE_BUFFER = 3
};

// ============================================================
// 8. SAMPLER HELPERS
// ============================================================

static inline Sampler sampler_linear_clamp(Gpu *g) {
    return sampler_create(g, &(SamplerDesc) {
        .min_filter = FILT_LINEAR,
        .mag_filter = FILT_LINEAR,
        .mip_filter = FILT_LINEAR,
        .addr_u = ADDR_CLAMP,
        .addr_v = ADDR_CLAMP,
        .addr_w = ADDR_CLAMP,
        .max_lod = 1000.0f
    });
}

static inline Sampler sampler_linear_repeat(Gpu *g) {
    return sampler_create(g, &(SamplerDesc) {
        .min_filter = FILT_LINEAR,
        .mag_filter = FILT_LINEAR,
        .mip_filter = FILT_LINEAR,
        .addr_u = ADDR_REPEAT,
        .addr_v = ADDR_REPEAT,
        .addr_w = ADDR_REPEAT,
        .max_lod = 1000.0f
    });
}

static inline Sampler sampler_nearest_clamp(Gpu *g) {
    return sampler_create(g, &(SamplerDesc) {
        .min_filter = FILT_NEAREST,
        .mag_filter = FILT_NEAREST,
        .mip_filter = FILT_NEAREST,
        .addr_u = ADDR_CLAMP,
        .addr_v = ADDR_CLAMP,
        .addr_w = ADDR_CLAMP,
        .max_lod = 1000.0f
    });
}

static inline Sampler sampler_shadow(Gpu *g) {
    return sampler_create(g, &(SamplerDesc) {
        .min_filter = FILT_LINEAR,
        .mag_filter = FILT_LINEAR,
        .mip_filter = FILT_NEAREST,
        .addr_u = ADDR_CLAMP,
        .addr_v = ADDR_CLAMP,
        .addr_w = ADDR_CLAMP,
        .compare = CMP_LEQUAL
    });
}

// ============================================================
// 9. ARENAS
// ============================================================

typedef enum Arena {
    ARENA_DEVICE,
    ARENA_TRANSIENT,
    ARENA_STAGING
} Arena;

typedef struct ArenaStats {
    uint64_t reserved;
    uint64_t used;
    uint64_t peak;
    uint32_t allocations;
} ArenaStats;

Range arena_alloc(
    Gpu *g,
    Arena arena,
    uint64_t size,
    uint64_t alignment
);

Range arena_alias(
    Gpu *g,
    Arena arena,
    Range previous,
    uint64_t size,
    uint64_t alignment
);

ArenaStats arena_stats(const Gpu *g, Arena arena);

// ============================================================
// 10. SHADERS AND PIPELINES
// ============================================================

typedef uint16_t ShaderId;

#define SHADER_NONE ((ShaderId)0)

typedef struct ShaderDesc {
    const char *path;
    const char *entry;
    ShaderStage stage;

    const char *const *defines;
    uint32_t define_count;
} ShaderDesc;

ShaderId shader_load(Gpu *g, const ShaderDesc *desc);

static inline ShaderId shader_load_path(
    Gpu *g,
    const char *path,
    ShaderStage stage
) {
    return shader_load(g, &(ShaderDesc) {
        .path = path,
        .entry = "main",
        .stage = stage
    });
}

typedef struct PipeDesc {
    const char *name;

    ShaderId vertex;
    ShaderId fragment;
    ShaderId compute;

    const char *vertex_entry;
    const char *fragment_entry;
    const char *compute_entry;

    uint16_t view_mask;
} PipeDesc;

// Same pipeline description + same rendering compatibility = cached pipeline.
Pipe pipe_create(Gpu *g, const PipeDesc *desc);
void pipe_destroy(Gpu *g, Pipe p);

static inline Pipe pipe_graphics(
    Gpu *g,
    ShaderId vs,
    ShaderId fs
) {
    return pipe_create(g, &(PipeDesc) {
        .vertex = vs,
        .fragment = fs
    });
}

static inline Pipe pipe_compute(Gpu *g, ShaderId cs) {
    return pipe_create(g, &(PipeDesc) {
        .compute = cs
    });
}

// ============================================================
// 11. DRAW STATE
// ============================================================

typedef struct DrawState {
    uint8_t samples;
    uint8_t blend;
    uint8_t depth_compare;
    uint8_t depth_write;
    uint8_t cull;
    uint8_t topology;
    uint16_t write_mask;
} DrawState;

_Static_assert(sizeof(DrawState) == 8, "DrawState must be 8 bytes");

DrawState draw_state_opaque(uint8_t samples, uint8_t cull);
DrawState draw_state_alpha(uint8_t samples, uint8_t cull);
DrawState draw_state_depth(uint8_t samples);

static inline uint64_t draw_sort_key(uint32_t pipe_id, DrawState s) {
    uint64_t key = 0;

    key |= (uint64_t)s.samples;
    key |= (uint64_t)s.blend        << 4;
    key |= (uint64_t)s.depth_compare << 8;
    key |= (uint64_t)s.depth_write  << 12;
    key |= (uint64_t)s.cull         << 13;
    key |= (uint64_t)s.topology     << 16;
    key |= (uint64_t)s.write_mask   << 20;

    return ((uint64_t)pipe_id << 40) | key;
}

// ============================================================
// 12. FRAME
// ============================================================

bool frame_begin(Gpu *g);
void frame_end(Gpu *g);

// Acquired swapchain image. Valid only during an active frame.
Image frame_image(Gpu *g);

// ============================================================
// 13. PASS DECLARATIONS
// ============================================================

typedef struct Attach {
    ImageSub image;

    Load load;
    Store store;

    float clear[4];

    ImageSub resolve;
    uint8_t resolve_mode;

    // Depth only: tested, never written. See attach_depth_read().
    bool read_only;
} Attach;

typedef struct ImageUse {
    ImageSub image;
    Access access;
    ImageType type;
} ImageUse;

typedef struct BufferUse {
    Range range;
    Access access;
} BufferUse;

// Fixed-size value object.
// No heap allocations, no builder chains, no pointer-lifetime problems.
typedef struct PassDesc {
    const char *name;

    PassKind kind;
    Pipe pipe;
    DrawState state;

    uint32_t width, height;

    Attach colors[MAX_COLOR_ATTACHMENTS];
    uint32_t color_count;

    Attach depth;
    bool has_depth;

    ImageUse images[MAX_IMAGE_USES];
    uint32_t image_count;

    BufferUse buffers[MAX_BUFFER_USES];
    uint32_t buffer_count;
} PassDesc;

// ============================================================
// 14. PASS HELPERS
// ============================================================

// Attachments.
static inline Attach attach_clear(
    Image img,
    float r, float g, float b, float a
) {
    return (Attach) {
        .image = SUB(img),
        .load = CLEAR,
        .store = STORE,
        .clear = { r, g, b, a }
    };
}

static inline Attach attach_write(Image img) {
    return (Attach) {
        .image = SUB(img),
        .load = DONT_CARE,
        .store = STORE
    };
}

static inline Attach attach_keep(Image img) {
    return (Attach) {
        .image = SUB(img),
        .load = LOAD,
        .store = STORE
    };
}

static inline Attach attach_clear_depth(Image img, float depth) {
    return attach_clear(img, depth, 0, 0, 0);
}

// Depth that is tested and never written, whatever DrawState.depth_write
// says. The image stays readable, so the same pass may sample it as well:
// it then lists the image with use_sampled() too.
static inline Attach attach_depth_read(Image img) {
    return (Attach) {
        .image = SUB(img),
        .load = LOAD,
        .store = STORE,
        .read_only = true
    };
}

static inline Attach attach_sub(ImageSub sub, Load load, Store store) {
    return (Attach) {
        .image = sub,
        .load = load,
        .store = store
    };
}

// Image dependencies.
static inline ImageUse use_sampled(Image img) {
    return (ImageUse) {
        .image = SUB(img),
        .access = ACC_READ,
        .type = IMAGE_SAMPLED
    };
}

static inline ImageUse use_storage(Image img) {
    return (ImageUse) {
        .image = SUB(img),
        .access = ACC_WRITE,
        .type = IMAGE_STORAGE
    };
}

static inline ImageUse use_storage_rw(Image img) {
    return (ImageUse) {
        .image = SUB(img),
        .access = ACC_RW,
        .type = IMAGE_STORAGE
    };
}

static inline ImageUse use_image(ImageSub sub, Access access, ImageType type) {
    return (ImageUse) {
        .image = sub,
        .access = access,
        .type = type
    };
}

// Buffer dependencies.
static inline BufferUse use_read(Range range) {
    return (BufferUse) {
        .range = range,
        .access = ACC_READ
    };
}

static inline BufferUse use_write(Range range) {
    return (BufferUse) {
        .range = range,
        .access = ACC_WRITE
    };
}

static inline BufferUse use_rw(Range range) {
    return (BufferUse) {
        .range = range,
        .access = ACC_RW
    };
}

// ============================================================
// 15. PASS EXECUTION
// ============================================================

// Pass declaration -> barriers -> dynamic rendering -> pipeline.
Pass *begin_pass(Gpu *g, const PassDesc *desc);
void end_pass(Pass *p);

// Change pipeline/state within an active pass.
void bind_pipe(Pass *p, Pipe pipe);
void set_state(Pass *p, DrawState state);

// A graphics pass begins with a viewport and a scissor that cover all of it.
// These replace them for the draws that follow, until the pass ends. Both
// are in pixels, with (0,0) at the top left. The scissor is cut down to the
// pass, so its rectangle may start or end outside.
void set_viewport(Pass *p, float x, float y, float width, float height);
void set_scissor(Pass *p, int32_t x, int32_t y, uint32_t width, uint32_t height);

// ============================================================
// 16. COMMANDS
// ============================================================

// Push constants are part of the work command.
void draw(
    Pass *p,
    DataSpan push,
    uint32_t vertices,
    uint32_t instances
);

void draw_indexed(
    Pass *p,
    DataSpan push,
    uint32_t indices,
    uint32_t instances
);

void draw_indirect(
    Pass *p,
    DataSpan push,
    Range arguments,
    uint32_t max_draws,
    uint32_t stride
);

void draw_indirect_count(
    Pass *p,
    DataSpan push,
    Range arguments,
    Range counter,
    uint32_t max_draws,
    uint32_t stride
);

void dispatch(
    Pass *p,
    DataSpan push,
    uint32_t x,
    uint32_t y,
    uint32_t z
);

void dispatch_indirect(
    Pass *p,
    DataSpan push,
    Range arguments
);

// ============================================================
// 17. PUSH CONSTANT ABI
// ============================================================

// All user push structures occupy exactly 256 bytes.
// Backend validates the actual device limit and rejects unsupported devices.

#define PUSH(name, BODY)                                      \
    typedef struct ALIGNAS(PUSH_ALIGN) name##_data { BODY } name##_data; \
    _Static_assert(sizeof(name##_data) <= MAX_PUSH_CONSTANTS, \
                   #name " exceeds push constant capacity"); \
    typedef struct ALIGNAS(PUSH_ALIGN) name {                 \
        BODY                                                  \
        uint8_t _pad[MAX_PUSH_CONSTANTS - sizeof(name##_data)]; \
    } name;                                                   \
    _Static_assert(sizeof(name) == MAX_PUSH_CONSTANTS,        \
                   #name " must be exactly 256 bytes")

// Example:
//
// PUSH(ScenePush,
//     float view_projection[16];
//     uint32_t texture_id;
//     uint32_t sampler_id;
// );

// ============================================================
// 18. UPLOADS
// ============================================================

Upload *upload_begin(Gpu *g);

void upload_buffer(
    Upload *u,
    Range dst,
    DataSpan src
);

void upload_image(
    Upload *u,
    ImageSub dst,
    DataSpan src
);

void upload_end(Gpu *g, Upload *u);

// ============================================================
// 19. READBACK
// ============================================================

Readback *readback_buffer(
    Gpu *g,
    Range src,
    DataSpan dst
);

Readback *readback_image(
    Gpu *g,
    ImageSub src,
    DataSpan dst
);

bool readback_ready(
    Gpu *g,
    Readback *r,
    bool wait
);

void readback_release(Gpu *g, Readback *r);

// ============================================================
// 20. PROFILING
// ============================================================

// What the application knows about a pass's work and the backend cannot see.
// It is recorded and reported beside the pass's measured time, and never
// checked or enforced. Zero means "not supplied".
typedef struct PassNote {
    uint32_t samples;           // N: samples taken per pixel
    uint32_t effective_samples; // N_eff: how many of them are independent
    uint32_t steps_per_pixel;   // loop or march steps per pixel
    uint32_t workgroup[3];      // compute: the shader's local size
    uint32_t tile_size;         // compute: pixels along the edge of one group's tile
} PassNote;

typedef struct PassStat {
    char name[48];
    double gpu_ms;
    uint64_t declared_bytes;
    uint32_t draws;
    uint32_t dispatches;
    uint32_t barriers;
    PassNote note;
} PassStat;

// A named stretch of one pass: see pass_scope().
typedef struct ScopeStat {
    char name[48];
    double gpu_ms;
    uint32_t pass; // the pass it is part of: an index into FrameStats.passes
} ScopeStat;

typedef struct FrameStats {
    double cpu_ms;
    double gpu_ms;

    uint32_t pass_count;
    uint32_t scope_count;
    uint32_t draws;
    // Stretches of draws inside a pass with no change of pipeline, state,
    // viewport or scissor in between. draws / state_runs says how well a
    // frame is sorted.
    uint32_t state_runs;
    uint32_t dispatches;
    uint32_t barriers;
    uint32_t descriptor_writes;
    uint32_t layout_transitions;

    uint64_t upload_bytes;
    uint64_t transient_peak;

    PassStat passes[MAX_PASSES];
    // In the order they were opened, so the scopes of one pass are together.
    ScopeStat scopes[MAX_PROFILE_SCOPES];
} FrameStats;

const FrameStats *gpu_stats(const Gpu *g);

// Replaces the note of the open pass.
void pass_note(Pass *p, PassNote note);

// Charges GPU time from here to the next pass_scope() or end_pass() to
// `name`: one row of FrameStats.scopes, which gpu_dump_stats() prints under
// the pass. Scopes follow one another and do not nest. What a pass does
// before its first scope and as it ends (clear, resolve, store) is in the
// pass's time and in no scope's. A frame keeps MAX_PROFILE_SCOPES scopes;
// a call beyond that does nothing. The split is as good as the driver's
// timestamps inside a pass: one that bins a pass and draws it later
// (lavapipe) charges a scope with work of the scope after it.
void pass_scope(Pass *p, const char *name);

void gpu_profile(Gpu *g, bool enabled);
void gpu_dump_stats(const Gpu *g, const char *path);

// ============================================================
// 21. OPTIONAL VULKAN ESCAPE
// ============================================================

#ifdef MU_ESCAPE

VkDevice gpu_raw_device(Gpu *g);
VkCommandBuffer pass_raw_cmd(Pass *p);

#endif

// ============================================================
// 22. STATIC ASSERTIONS
// ============================================================

_Static_assert(sizeof(Buffer) == 8, "Buffer handle ABI");
_Static_assert(sizeof(Image) == 8, "Image handle ABI");
_Static_assert(sizeof(Sampler) == 8, "Sampler handle ABI");
_Static_assert(sizeof(Pipe) == 8, "Pipe handle ABI");

_Static_assert(MAX_PUSH_CONSTANTS == 256, "Push ABI");
_Static_assert(PUSH_ALIGN == 16, "Push alignment");

#endif // MU_VK_H

