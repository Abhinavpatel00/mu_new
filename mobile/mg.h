/*
 * mg.h — a GPU API for mobile, designed from the hardware up.
 *
 * This header does not include Vulkan. No Vk* type reaches the application:
 * the platform boundary is MgDevice, MgFrame and 8-byte ids. The backend
 * (mg.c) is the only file that knows what a descriptor set is.
 *
 * The five decisions, and the measurement each one rests on
 * (MOBILE_TALKS.md has the sources, MOBILE_API_DESIGN.md the reasoning):
 *
 *   1. DRAW SUBMISSION IS CPU-SIDE AND INDEXED.
 *      The cheapest measured mobile configuration is CPU culling + indexed
 *      batched draws: 121 FPS / 2.02 W against 84 FPS / 3.84 W for the fully
 *      GPU-driven path on the same Adreno device, with a thermal cliff at
 *      4 minutes for the latter (Qualcomm, SIGGRAPH 2026). Roblox shipped
 *      840 draws in 13 ms CPU on a Mali-G72 (GDC 2020).
 *
 *   2. NO MULTI-DRAW INDIRECT. EVER, AS A REQUIREMENT.
 *      maxDrawIndirectCount == 1 on all pre-Mali-G710 hardware (Arm). MDI is
 *      an escape (mg_draw_indirect, one draw per call, one call per batch),
 *      never the primary path.
 *
 *   3. NO BINDLESS. A MATERIAL IS 4 TEXTURES.
 *      Descriptor indexing is not in Android Baseline 2021/2022 profiles, and
 *      an indexed descriptor costs a dependent load: 40.5 text-read cycles
 *      against 12.1 for the indexed-draw path (Adreno offline compiler).
 *
 *   4. A DRAW IS 24 BYTES IN A FLAT ARRAY.
 *      10,000 draws at 60 fps on a $99 phone came from precreated pipelines
 *      and bind groups plus a metadata-only command stream (HypeHype,
 *      SIGGRAPH 2023). mg_draw() appends; it never calls the driver.
 *
 *   5. THE FRAME IS SORTED BEFORE IT IS RECORDED.
 *      10,000 records become ~8 pipeline binds and ~64 material binds
 *      (mg.c, -DMG_TEST). State changes are the cost; sort them away.
 *
 *   6. VERTEX FETCH STAYS FIXED-FUNCTION.
 *      Positions and attributes come from two bound arenas, addressed per draw
 *      by firstIndex and vertexOffset. Pulling them through a storage buffer
 *      trades a format-converting, pre-fetching, cache-line-aware fetch unit
 *      for generic loads — inside a shader both vendors execute MORE THAN ONCE
 *      per frame. Adreno runs the VS in the binning pass and again in the
 *      render pass, with separate hardware counters for each (msm_kgsl.h
 *      defines VFD and BV_VFD as distinct perfcounter groups); Mali runs a
 *      position-only IDVS shader during binning and a varying shader per tile,
 *      and panvk notes the position results go through memory at 16 B/vertex
 *      against a 64 B cache line. mg_pipe(desc.pull) opts in — for geometry
 *      with no CPU-side layout, and for nothing else.
 *
 * Shader ABI — fixed, two descriptor sets, no unbounded arrays:
 *
 *   set 0, binding 0   uniform buffer    per-frame constants (camera, time)
 *   set 0, binding 1   storage buffer    the arena: geometry, params, draws
 *   set 1, binding 0   sampler2D [4]     the material; immutable samplers
 *
 *   push constants (8 bytes, MG_PUSH_BYTES):
 *     uint32 const_off;    arena offset of this draw's 16-byte param slot
 *     uint32 flags;        material/state flags for this draw
 *
 *   Geometry offsets are NOT in the push: they are draw-command arguments
 *   (firstIndex, vertexOffset) that the hardware's vertex fetch adds itself.
 *   The push carries an index into the arena, not data, which is why it fits
 *   in 128 bytes of maxPushConstantsSize with 120 to spare, and why it is the
 *   mobile-legal form of "root data is a pointer".
 */

#ifndef MG_H
#define MG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================
// 0. CONFIGURATION
// ============================================================

#define MG_MAX_DRAWS         16384   /* per frame; 24 B each = 384 KB      */
#define MG_MAX_PASSES           32
#define MG_MAX_PIPES           128
#define MG_MAX_MATERIALS      1024
#define MG_MAX_STATES           16   /* closed set; see MgStateDesc         */
#define MG_MAX_COLOR_ATTACH      4   /* tile memory budget, not 8           */
#define MG_MAX_SUBPASSES         4
#define MG_MAX_MESHES         4096

#define MG_PUSH_BYTES            8   /* maxPushConstantsSize is 128 on phones */
#define MG_CONST_SLOT_BYTES     16
#define MG_CONST_SLOTS        MG_MAX_DRAWS   /* one slot per draw: 256 KB per frame in flight */
#define MG_FRAMES_IN_FLIGHT      2   /* latency and memory, not throughput  */

#define MG_TEXTURE_SLOTS         4   /* a material cannot exceed this       */

#define MG_ALIGN               16   /* every slot in the constant ring */

#define MG_HANDLE(T) typedef struct T { uint32_t index, gen; } T
MG_HANDLE(MgPass);
MG_HANDLE(MgPipe);
MG_HANDLE(MgMat);
MG_HANDLE(MgMesh);
MG_HANDLE(MgTex);
MG_HANDLE(MgState);

#define MG_NONE(T) ((T){ 0, 0 })
#define MG_OK(h)   ((h).index != 0)

#define MG_PASS_NONE  MG_NONE(MgPass)
#define MG_PIPE_NONE  MG_NONE(MgPipe)
#define MG_MAT_NONE   MG_NONE(MgMat)
#define MG_TEX_NONE   MG_NONE(MgTex)
#define MG_STATE_NONE MG_NONE(MgState)

// ============================================================
// 1. TYPES
// ============================================================

typedef enum MgFmt {
    MG_FMT_NONE = 0,
    MG_RGBA8, MG_RGBA8_SRGB, MG_BGRA8, MG_R8, MG_R8G8, MG_RGBA8_SNORM,
    MG_R11G11B10, MG_RGBA16F, MG_R16F, MG_R32F,
    MG_RGB10A2, MG_A2B10G10R10,
    MG_D16, MG_D24S8, MG_D32F,
    MG_ASTC_4x4, MG_ASTC_6x6, MG_ETC2_RGB, MG_ETC2_RGBA,
    MG_SWAPCHAIN,          /* "this attachment is the swapchain image" */
    MG_FMT_COUNT
} MgFmt;

// Load/store: the two words that decide whether a tile is touched at all.
// DONT_CARE and NONE are not "don't care" — they are what lets the hardware
// skip the tile completely (turnip's cond_load_allowed / cond_store_allowed).
typedef enum MgLoad  { MG_LOAD, MG_CLEAR, MG_DONT_CARE, MG_LOAD_NONE }  MgLoad;
// DONT_CARE and NONE are different promises: "the contents are garbage, throw
// the tile away" versus "nothing wrote to it, there is nothing to write back".
// The second is the cheap one for a depth buffer you only test against (Arm).
typedef enum MgStore {
    MG_STORE,            // write the attachment back to memory
    MG_STORE_NONE,       // transient: contents discarded (pair with MG_TEX_RENDER|transient)
    MG_STORE_READONLY,   // read-only attachment: STORE_OP_NONE where available
} MgStore;

typedef enum MgBlend {
    MG_BLEND_NONE = 0,
    MG_BLEND_ALPHA, MG_BLEND_PREMUL, MG_BLEND_ADD, MG_BLEND_MUL
} MgBlend;

typedef enum MgDepth {
    MG_DEPTH_NONE = 0,
    MG_DEPTH_READ,          /* test, no write */
    MG_DEPTH_READ_WRITE,
    MG_DEPTH_EQUAL          /* the sky / decal case: no write, no clear */
} MgDepth;

typedef enum MgCmp   { MG_CMP_LESS, MG_CMP_GREATER, MG_CMP_GEQUAL } MgCmp;
typedef enum MgCull  { MG_CULL_NONE, MG_CULL_BACK, MG_CULL_FRONT }  MgCull;
typedef enum MgTopo  { MG_TRIANGLES, MG_TRIANGLE_STRIP }            MgTopo;

typedef enum MgMem {
    MG_MEM_FRAME = 0,       /* rewound at mg_frame(); write-combining       */
    MG_MEM_STATIC,          /* never rewound                                 */
    MG_MEM_READBACK         /* host-cached; the only one safe to read from   */
} MgMem;

typedef enum MgTexFlags {
    MG_TEX_SRGB     = 1u << 0,
    MG_TEX_MIP      = 1u << 1,
    MG_TEX_RENDER   = 1u << 2,   /* also usable as an attachment             */
    MG_TEX_STORAGE  = 1u << 3,
    MG_TEX_ARRAY    = 1u << 4    /* a material slot may be a 2D array        */
} MgTexFlags;

typedef enum MgFilter { MG_NEAREST, MG_LINEAR, MG_ANISO_2, MG_ANISO_4 } MgFilter;
typedef enum MgWrap   { MG_WRAP_REPEAT, MG_WRAP_CLAMP, MG_WRAP_MIRROR }  MgWrap;

// ============================================================
// 2. DEVICE
// ============================================================

// Which checks the validation layer runs. These are the layer's own settings,
// handed to it at vkCreateInstance through VK_EXT_layer_settings, so no device
// property, settings file or environment variable is involved.
//
//   CORE           API misuse: the default VK_LAYER_KHRONOS_validation
//   SYNC           read-after-write / write-after-write hazards between
//                  stages, passes, submits and presents (very expensive)
//   BEST_PRACTICES performance and portability advice
//
// Validation is a debugging aid: it costs 2-10x (CORE) to 20x+ (SYNC) and must
// never be the reason a frame is late in a measurement run.
typedef enum MgValidation {
    MG_VALIDATE_NONE           = 0,
    MG_VALIDATE_CORE           = 1u << 0,
    MG_VALIDATE_SYNC           = 1u << 1,
    MG_VALIDATE_BEST_PRACTICES = 1u << 2,
    MG_VALIDATE_ALL            = MG_VALIDATE_CORE | MG_VALIDATE_SYNC | MG_VALIDATE_BEST_PRACTICES,
} MgValidation;

typedef struct MgDesc {
    const char *name;            /* names the pipeline cache file            */
    uint32_t    width, height;   /* the frame's size; 0 = follow the surface */
    bool        vsync;
    uint32_t    validation;      /* MgValidation bits; 0 = off               */
    uint32_t    frames_in_flight;
    uint64_t    arena_bytes;     /* MG_MEM_FRAME + MG_MEM_STATIC pool        */
} MgDesc;

typedef struct MgCaps {
    // What the application may branch on, once, at setup. Nothing in the
    // mandatory path requires any of these.
    bool     dynamic_rendering;
    bool     subpass_input;         /* VkRenderPass path is available        */
    bool     indirect_single;       /* maxDrawIndirectCount >= 1: universal  */
    uint32_t indirect_max;          /* 1 on pre-Mali-G710: never rely on >1  */
    bool     store_op_none;         /* VK_EXT_load_store_op_none / 1.3      */
    bool     pre_rotate;            /* swapchain can be IDENTITY: DPU path  */
    bool     descriptor_buffer;
    bool     astc;
    uint32_t push_bytes;
    uint32_t color_attachments;
    uint32_t sampled_images;        /* per stage; material needs 4           */
    uint32_t workgroup_invocations;
    uint32_t workgroup_shared;
    uint32_t subgroup_size;
    uint32_t texture_slots;         /* min(MG_TEXTURE_SLOTS, hw)             */
} MgCaps;

typedef struct MgDevice MgDevice;
typedef struct MgFrame  MgFrame;

MgDevice *mg_create(const MgDesc *desc);
void      mg_destroy(MgDevice *d);
MgCaps    mg_caps(const MgDevice *d);

// The one place a window is mentioned. The backend owns the swapchain.
bool mg_surface(MgDevice *d, void *native_window);   /* ANativeWindow* */
void mg_surface_lost(MgDevice *d);
void mg_resize(MgDevice *d, uint32_t width, uint32_t height);

// Degrees (0, 90, 180, 270) the application must rotate its geometry by. The
// swapchain is created with preTransform = IDENTITY whenever the surface allows
// it, so the display processor never rotates the frame -- the vertex shader
// does, for free. This is a mobile-only concern that no wrapper provides.
uint32_t mg_display_rotation(const MgDevice *d);

// Every object is created here and nowhere else. `mg_warm()` compiles every
// pipeline the declared passes/states/pipes can form; call it behind a
// loading screen and before the first frame.
void mg_warm(MgDevice *d, bool blocking);

// ============================================================
// 3. OBJECTS
// ============================================================

typedef struct MgSubpass {
    uint32_t  color[MG_MAX_COLOR_ATTACH];  /* indices into PassDesc.color   */
    uint32_t  color_count;
    int32_t   depth;                       /* index or -1                   */
    uint8_t   inputs[MG_MAX_COLOR_ATTACH]; /* attachments read via TileRead */
    uint32_t  input_count;
    MgPipe    pipe;                        /* a subpass may change pipe     */
    MgState   state;
} MgSubpass;

typedef struct MgPassDesc {
    const char *name;

    struct { MgFmt fmt; MgLoad load; MgStore store; bool transient; } color[MG_MAX_COLOR_ATTACH];
    uint32_t color_count;

    MgFmt   depth;
    MgLoad  depth_load;
    MgStore depth_store;

    uint32_t width, height;        /* 0 = the frame's size                  */
    uint8_t  samples;              /* 1, 2, 4; 4 is what every device has   */
    bool     resolve;              /* MSAA resolved in the pass, always     */

    MgSubpass subpass[MG_MAX_SUBPASSES];
    uint32_t  subpass_count;       /* 1 for the common case                 */
} MgPassDesc;

typedef struct MgShader {
    const uint32_t *spirv;         /* compiled words, not a path: no IO in a frame */
    uint32_t        words;
} MgShader;

typedef struct MgPipeDesc {
    MgPass   pass;                 /* the pass this pipe draws into, known up front */
    MgShader vs, fs;
    uint8_t  streams;              /* 0 = attribute-less (fullscreen), 1 = positions,
                                      2 = positions + attribute stream             */
    MgFmt    color[MG_MAX_COLOR_ATTACH];
    uint8_t  color_count;
    MgFmt    depth;
    uint8_t  samples;
    MgTopo   topology;
    bool     alpha_to_coverage;
    // Opt out of fixed-function vertex fetch. Only for geometry the CPU cannot
    // describe — GPU-compacted output, procedural meshes. Never for a mesh the
    // application already has as a vertex array: see rule 6 at the top.
    bool     pull;
} MgPipeDesc;

// The thick half of the state. A closed set declared at startup, indexed by a
// 16-bit field in every draw record, so it is part of the sort key and part
// of the pre-warm — never a runtime pipeline lookup.
typedef struct MgStateDesc {
    MgBlend blend;
    MgDepth depth;
    MgCmp   cmp;
    MgCull  cull;
    uint8_t write_mask;            /* RGBA bits                             */
} MgStateDesc;

typedef struct MgSamplerDesc { MgFilter filter; MgWrap wrap; float max_lod; bool compare; } MgSamplerDesc;

typedef struct MgMatDesc {
    MgTex    tex[MG_TEXTURE_SLOTS];   /* MG_TEX_NONE leaves the slot empty */
    uint8_t  samplers[MG_TEXTURE_SLOTS];
    uint8_t  slots;                   /* <= MG_TEXTURE_SLOTS               */
    uint8_t  flags;                   /* alpha test / two sided / ...      */
} MgMatDesc;

// A mesh is two ranges in two arenas, not a set of buffers to bind. The
// attributes and the positions are SEPARATE streams, always: the binning pass
// and the tile pass only read positions, and Mali's IDVS writes position
// results to memory at 16 B/vertex against a 64 B cache line, so an
// interleaved 32-byte vertex doubles the tiler's read traffic for nothing.
typedef struct MgMeshDesc {
    const void *positions;         /* R32G32B32 (12 B) or R16G16B16A16_SNORM (8 B) */
    uint32_t    vertex_count;
    const void *attributes;        /* normal / uv / tangent stream, its own        */
    uint32_t    attribute_stride;  /* 0 when the pipe reads no attributes          */
    const void *indices;           /* uint16 or uint32, LOCAL to this mesh         */
    uint32_t    index_count;
    bool        indices_u32;
} MgMeshDesc;

typedef struct MgTexDesc {
    uint32_t  width, height, layers;
    uint32_t  mips;
    MgFmt     format;
    uint32_t  flags;               /* MgTexFlags                          */
} MgTexDesc;

MgPass  mg_pass(MgDevice *d, const MgPassDesc *desc);

// The image a pass writes: its resolve target, or its colour attachment when
// the pass has no MSAA. Use it as a texture in a later pass's material — this
// is the only way to read a pass output, and the handle stays valid across
// surface recreation.
MgTex   mg_pass_output(MgDevice *d, MgPass pass);
MgPipe  mg_pipe(MgDevice *d, const MgPipeDesc *desc);
MgState mg_state(MgDevice *d, const MgStateDesc *desc);
MgMat   mg_material(MgDevice *d, const MgMatDesc *desc);
MgMesh  mg_mesh(MgDevice *d, const MgMeshDesc *desc);
MgTex   mg_texture(MgDevice *d, const MgTexDesc *desc, const void *data, uint64_t bytes);

// Textures and buffers are uploaded at frame boundaries. mg_upload records a
// copy into the frame being recorded; it never blocks and never allocates.
void mg_upload(MgFrame *f, MgTex dst, const void *data, uint64_t bytes);
void mg_readback(MgFrame *f, MgTex src, void *dst, uint64_t bytes);

void mg_destroy_tex(MgDevice *d, MgTex t);
void mg_destroy_mesh(MgDevice *d, MgMesh m);
void mg_destroy_mat(MgDevice *d, MgMat m);

// ============================================================
// 4. MEMORY
// ============================================================

// A pointer that is both writable by the CPU and addressable by the GPU.
// MG_MEM_FRAME is write-combining: never read through it.
void *mg_malloc(MgDevice *d, uint64_t bytes, uint64_t align, MgMem mem);
void  mg_free(MgDevice *d, void *ptr);

// A 16-byte-aligned slot in this frame's constant ring. One per draw.
uint32_t mg_const(MgFrame *f, const void *data, uint32_t bytes);

// Per-frame constants: camera, time, resolution. Written once per frame before
// any pass, read by every shader from set 0 binding 0. Keep it under 256 bytes;
// it is one buffer for the whole frame, because a per-draw camera is a bug.
void mg_frame_const(MgFrame *f, const void *data, uint32_t bytes);

// ============================================================
// 5. THE FRAME
// ============================================================

// Begins a frame: acquires, rewinds MG_MEM_FRAME, resets the draw list.
// Returns false when the surface is gone; the caller recreates it.
bool mg_frame(MgDevice *d, MgFrame **out);

void mg_pass_begin(MgFrame *f, MgPass pass);
void mg_pass_end(MgFrame *f);

// The mobile frame is more often a stack of translucent UI quads than a pile of
// opaque triangles (Samsung, MMG 2026: overdraw is the dominant cost). For that
// frame, sorting by state is a correctness bug, so a draw can opt out.
typedef enum MgDrawFlags {
    MG_DRAW_ORDERED = 1u << 0,   // keep submission order; never reorder
} MgDrawFlags;

// What the application passes in. Handles resolve to indices at the door;
// nothing downstream of mg_draw() sees a 64-bit value.
typedef struct MgDrawDesc {
    MgPipe   pipe;
    MgMat    mat;
    MgMesh   mesh;
    MgState  state;
    uint32_t params;        /* the slot mg_const() returned                 */
    uint16_t instances;
    uint32_t flags;         /* MgDrawFlags                                  */
} MgDrawDesc;

// 8 bytes. An index into the arena, never data — and, now that firstIndex and
// vertexOffset are draw-command arguments, not even a geometry offset.
typedef struct MgPush {
    uint32_t const_off;
    uint32_t flags;
} MgPush;

_Static_assert(sizeof(MgPush) == MG_PUSH_BYTES, "the push block is exactly 8 bytes");

void mg_draw(MgFrame *f, const MgDrawDesc *draw);
void mg_draw_batch(MgFrame *f, const MgDrawDesc *draws, uint32_t count);

// One draw per call. `count_addr` is a GPU pointer to a device-side count, or
// NULL for a fixed one: this is the only indirect shape the fleet allows.
void mg_draw_indirect(MgFrame *f, MgPipe pipe, MgMat mat, MgState state,
                      const void *args, const void *count_addr);

// Ends the frame: sorts every pass's records by (pipe, material, state),
// records one command buffer, submits once, presents once.
void mg_frame_end(MgDevice *d, MgFrame *f);

// ============================================================
// 6. THE COLLECTOR — pure data, no Vulkan
// ============================================================
//
// This is the half of a frame that never touches the driver: the flat draw
// array, the stable radix sort, and the coalescing run list. mg.c embeds it
// as the first member of MgFrame; `cc -DMG_TEST mg.c` builds it standalone,
// which is how the numbers in MOBILE_API_DESIGN.md were produced.

// The stored record: indices, not handles. 24 bytes, which is the whole
// reason 10,000 draws cost 240 KB and not a megabyte.
typedef struct MgDraw {
    uint32_t pipe;          /* 12 bits                                  */
    uint32_t mat;           /* 12 bits                                  */
    uint32_t mesh;          /* 12 bits                                  */
    uint32_t params;        /* offset into the constant ring            */
    uint16_t instances;
    uint8_t  state;         /* state index (MG_MAX_STATES is 16)        */
    uint8_t  flags;         /* MgDrawFlags: MG_DRAW_ORDERED             */
    uint32_t order;         /* append index: keeps the sort stable      */
} MgDraw;

_Static_assert(sizeof(MgDraw) == 24, "a draw record is 24 bytes: 10k draws = 240 KB");

typedef struct MgDrawList {
    MgDraw   draw[MG_MAX_DRAWS];
    MgDraw   scratch[MG_MAX_DRAWS];
    uint32_t count;
    uint32_t overflow;          // appended while full: a bug, counted anyway
} MgDrawList;

typedef struct MgCollStats {
    uint32_t records;
    uint32_t runs;              // coalesced runs after the sort
    uint32_t pipe_binds;        // pipeline changes the driver must see
    uint32_t mat_binds;         // descriptor set 1 binds
    uint32_t push_count;        // one per draw: 8 B, unavoidable
    uint32_t bucket_changes;    // blend/depth state changes
} MgCollStats;

void        mg_coll_clear(MgDrawList *l);
void        mg_coll_draw(MgDrawList *l, const MgDraw *d);
void        mg_coll_sort(MgDrawList *l);                 // stable, 4 x 8 bit
void        mg_coll_sort_range(MgDrawList *l, uint32_t first, uint32_t count);
MgCollStats mg_coll_coalesce(const MgDrawList *l);       // walk final order
uint32_t    mg_coll_key(const MgDraw *d);                // exposed for tests

// ============================================================
// 7. STATS AND ESCAPE
// ============================================================

typedef struct MgStats {
    uint32_t draws;
    uint32_t passes;
    uint32_t pipe_binds;        /* after coalescing                        */
    uint32_t mat_binds;
    uint32_t push_count;
    uint32_t barriers;
    uint32_t sort_us;
    uint64_t cmd_bytes;
    uint64_t arena_used;
    double   cpu_ms, gpu_ms;
    // Messages the Khronos validation layer delivered this run (0/0 when it is
    // not loaded). Errors are API misuse, not performance: fix them.
    uint32_t val_errors;
    uint32_t val_warnings;
} MgStats;

const MgStats *mg_stats(const MgDevice *d);
void mg_dump(const MgDevice *d, const char *path);

#ifdef MG_ESCAPE
void *mg_raw_device(const MgDevice *d);   // VkDevice
void *mg_raw_cmd(const MgFrame *f);       // VkCommandBuffer
#endif

#endif // MG_H
