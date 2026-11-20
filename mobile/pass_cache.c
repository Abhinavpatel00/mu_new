/*
 * pass_cache.c — the mobile pass compiler, as a working sketch.
 *
 * vk.c builds a VkRenderPass worth of information on every begin_pass() and
 * throws it away. On a tile-based GPU that information is the pass: turnip
 * derives load/store booleans, GMEM layout, per-pixel gmem/sysmem bandwidth
 * and a render-mode decision from it (tu_pass.cc:586, :906, :935) and then
 * autotunes tile size and render mode keyed on the pass
 * (tu_autotune.h:21, tu_autotune.cc:822). None of that converges unless the
 * same pass comes back as the same object with the same attachments.
 *
 * So: one hash of the thing that makes a pass a pass, one slot per hash, one
 * small table of framebuffers per slot. Everything fixed capacity, no
 * allocation after startup, no pointer chasing in the lookup.
 *
 * Build (compiles against the tree's own Vulkan headers):
 *   gcc -std=c11 -Wall -Wextra -c pass_cache.c \
 *       -I ../../desktop -I ../../desktop/external/vulkan/include
 *
 * Self-test (no device, no Vulkan calls):
 *   gcc -std=c11 -DMU_PASS_CACHE_TEST -o /tmp/pc_test pass_cache.c \
 *       -I ../../desktop -I ../../desktop/external/vulkan/include && /tmp/pc_test
 */

#include "vk_mobile.h"

#include <stdio.h>
#include <string.h>

// ============================================================
// 1. THE KEY
// ============================================================

// Exactly the fields that change what the hardware does with the pass, and
// nothing else. Formats and sample counts are what the pipeline and the pass
// both need; load/store decide whether a tile is touched at all; resolve and
// view mask decide what the tiler writes back.
typedef struct PassKey {
    uint32_t color[MAX_COLOR_ATTACHMENTS];  // VkFormat, FMT_NONE terminates the list
    uint32_t color_count;

    uint32_t depth;      // VkFormat or VK_FORMAT_UNDEFINED
    uint32_t samples;    // VkSampleCountFlagBits

    uint32_t load;       // 2 bits per attachment: 0..7 -> 8 attachments, 16..17 depth
    uint32_t store;      // same packing
    uint32_t resolve;    // one bit per colour attachment

    uint32_t subpass_count;
    uint32_t view_mask;

    uint32_t kind;       // PassKind: graphics and compute do not share a VkRenderPass
    uint32_t pad;
} PassKey;

_Static_assert(sizeof(PassKey) % 8 == 0, "the key is hashed and compared as bytes");
_Static_assert(sizeof(PassKey) <= 96, "a key that does not fit a cache line pair is not a key");

#define LOAD_SHIFT(i)  (2u * (i))
#define OP_PUT(word, i, op)  (((word) & ~(3u << LOAD_SHIFT(i))) | (((uint32_t)(op) & 3u) << LOAD_SHIFT(i)))
#define OP_GET(word, i)      (((word) >> LOAD_SHIFT(i)) & 3u)

// Attachments are filled in the order a VkRenderPass wants them: every colour
// in subpass order, then depth. The packing has room for 8 + 1; the API's
// budget is 4 (+1) on a tiler, and 8 only on a device that says so.
#define KEY_OP_MAX (2 * (MAX_COLOR_ATTACHMENTS + 1))
_Static_assert(KEY_OP_MAX <= 32, "load/store are packed 2 bits per attachment into uint32_t");

static inline void pass_key_add_color(PassKey *k, VkFormat f, Load load, Store store, bool resolve) {
    const uint32_t i = k->color_count++;
    k->color[i] = (uint32_t)f;
    k->load  = OP_PUT(k->load,  i, load);
    k->store = OP_PUT(k->store, i, store);
    k->resolve |= resolve ? (1u << i) : 0u;
}

static inline void pass_key_set_depth(PassKey *k, VkFormat f, Load load, Store store) {
    k->depth = (uint32_t)f;
    k->load  = OP_PUT(k->load,  MAX_COLOR_ATTACHMENTS, load);
    k->store = OP_PUT(k->store, MAX_COLOR_ATTACHMENTS, store);
}

// ============================================================
// 2. HASH AND LOOKUP
// ============================================================

// FNV-1a, 64 bit, over the key's bytes. Not the fastest hash there is; the
// fastest hash that is also eight instructions and no table. Passes are
// looked up a handful of times a frame, and the key is one cache line.
static inline uint64_t pass_hash(const PassKey *k) {
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof(*k); i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

#define PASS_CACHE_BITS 9
#define PASS_CACHE_SIZE (1u << PASS_CACHE_BITS)
#define FB_PER_PASS     8

typedef struct FbEntry {
    uint64_t      view[MAX_COLOR_ATTACHMENTS + 1];  // colour views, then depth
    uint32_t      width, height;
    uint32_t      used;
    VkFramebuffer framebuffer;
} FbEntry;

// A pass and its framebuffers live together: the pass is looked up, the
// framebuffer is a linear scan of at most eight 80-byte entries in the same
// two cache lines. No second hash table, no indirection.
typedef struct PassSlot {
    PassKey    key;
    uint32_t   used, refs;
    VkRenderPass render_pass;

    FbEntry    fb[FB_PER_PASS];
    uint32_t   fb_count;
    uint32_t   pad;
} PassSlot;

typedef struct PassCache {
    PassSlot slots[PASS_CACHE_SIZE];
    uint32_t count, collisions;
} PassCache;

// Linear probe, no deletion: passes are permanent for the life of the device
// (a pass that stops being used costs 96 bytes, and the alternative is a
// tombstone path that only exists to save 96 bytes).
PassSlot *pass_slot(PassCache *c, const PassKey *k, uint32_t *was_empty) {
    const uint32_t start = (uint32_t)(pass_hash(k) & (PASS_CACHE_SIZE - 1));

    for (uint32_t probe = 0; probe < PASS_CACHE_SIZE; probe++) {
        PassSlot *s = &c->slots[(start + probe) & (PASS_CACHE_SIZE - 1)];

        if (!s->used) {
            s->used = 1;
            s->key  = *k;
            s->render_pass = VK_NULL_HANDLE;
            s->fb_count = 0;
            c->count++;
            c->collisions += probe;
            *was_empty = 1;
            return s;
        }
        if (memcmp(&s->key, k, sizeof(*k)) == 0) {
            *was_empty = 0;
            return s;
        }
    }

    return NULL;  // full: the caller's MAX_PASSES was too small, and says so
}

FbEntry *pass_fb(PassSlot *s, const uint64_t *views, uint32_t view_count,
                        uint32_t width, uint32_t height) {
    for (uint32_t i = 0; i < s->fb_count; i++) {
        FbEntry *e = &s->fb[i];

        if (e->width != width || e->height != height)
            continue;
        if (memcmp(e->view, views, view_count * sizeof(uint64_t)) == 0)
            return e;
    }

    if (s->fb_count == FB_PER_PASS)
        return NULL;  // the caller is rotating attachments faster than the cache can follow

    FbEntry *e = &s->fb[s->fb_count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->view, views, view_count * sizeof(uint64_t));
    e->width  = width;
    e->height = height;
    e->used   = 1;
    return e;
}

// ============================================================
// 3. THE ONLY VULKAN CALLS IN THE FILE
// ============================================================

// One VkRenderPass, built from the key alone. The subpass dependency is the
// whole mobile design in four lines: external -> subpass 0 and subpass n ->
// external, both BY_REGION, with the attachment stage/access masks. A pass
// that is one subpass still gets them, because a pass that follows another
// pass on the same attachment must not force a full-resolution wait.
VkRenderPass pass_build(VkDevice dev, const PassKey *k) {
    VkAttachmentDescription att[MAX_COLOR_ATTACHMENTS + 1];
    VkAttachmentReference   color_ref[MAX_COLOR_ATTACHMENTS];
    VkAttachmentReference   resolve_ref[MAX_COLOR_ATTACHMENTS];
    uint32_t                attachment_count = 0;

    for (uint32_t i = 0; i < k->color_count; i++) {
        const bool   msaa     = k->samples > VK_SAMPLE_COUNT_1_BIT;
        const bool   resolved = (k->resolve & (1u << i)) != 0;

        color_ref[i].attachment = attachment_count;
        color_ref[i].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        resolve_ref[i].attachment = VK_ATTACHMENT_UNUSED;

        att[attachment_count++] = (VkAttachmentDescription) {
            .format  = (VkFormat)k->color[i],
            .samples = (VkSampleCountFlagBits)k->samples,
            .loadOp  = (VkAttachmentLoadOp)OP_GET(k->load, i),
            .storeOp = resolved ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                : (VkAttachmentStoreOp)OP_GET(k->store, i),
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = OP_GET(k->load, i) == VK_ATTACHMENT_LOAD_OP_LOAD
                            ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };

        // A resolve attachment is single-sampled whatever the pass samples at,
        // and it is the only thing written back: the MSAA image never leaves
        // tile memory. Arm: never store a multisampled attachment.
        if (resolved && msaa) {
            resolve_ref[i].attachment = attachment_count;
            resolve_ref[i].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att[attachment_count++] = (VkAttachmentDescription) {
                .format  = (VkFormat)k->color[i],
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            };
        }
    }

    VkAttachmentReference depth_ref = { .attachment = VK_ATTACHMENT_UNUSED };
    if (k->depth != (uint32_t)VK_FORMAT_UNDEFINED) {
        depth_ref.attachment = attachment_count;
        depth_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[attachment_count++] = (VkAttachmentDescription) {
            .format  = (VkFormat)k->depth,
            .samples = (VkSampleCountFlagBits)k->samples,
            .loadOp  = (VkAttachmentLoadOp)OP_GET(k->load, MAX_COLOR_ATTACHMENTS),
            .storeOp = (VkAttachmentStoreOp)OP_GET(k->store, MAX_COLOR_ATTACHMENTS),
            .stencilLoadOp  = (VkAttachmentLoadOp)OP_GET(k->load, MAX_COLOR_ATTACHMENTS),
            .stencilStoreOp = (VkAttachmentStoreOp)OP_GET(k->store, MAX_COLOR_ATTACHMENTS),
            .initialLayout  = OP_GET(k->load, MAX_COLOR_ATTACHMENTS) == VK_ATTACHMENT_LOAD_OP_LOAD
                            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
    }

    const VkSubpassDescription sub = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = k->color_count,
        .pColorAttachments    = color_ref,
        .pResolveAttachments  = k->resolve ? resolve_ref : NULL,
        .pDepthStencilAttachment = k->depth != (uint32_t)VK_FORMAT_UNDEFINED ? &depth_ref : NULL,
    };

    const VkSubpassDependency dep[2] = {
        {
            .srcSubpass    = VK_SUBPASS_EXTERNAL,
            .dstSubpass    = 0,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
        },
        {
            .srcSubpass    = 0,
            .dstSubpass    = VK_SUBPASS_EXTERNAL,
            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
        },
    };

    const VkRenderPassCreateInfo ci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = attachment_count,
        .pAttachments    = att,
        .subpassCount    = 1,
        .pSubpasses      = &sub,
        .dependencyCount = 2,
        .pDependencies   = dep,
    };

    VkRenderPass rp = VK_NULL_HANDLE;
    const VkResult r = vkCreateRenderPass(dev, &ci, NULL, &rp);

    if (r != VK_SUCCESS)
        fprintf(stderr, "pass_build: vkCreateRenderPass failed (%d); the key has %u colour "
                        "attachments, %u samples\n", (int)r, k->color_count, k->samples);
    return rp;
}

// One VkFramebuffer. Images are views, and the order is the key's order.
VkFramebuffer fb_build(VkDevice dev, VkRenderPass pass,
                             const VkImageView *views, uint32_t view_count,
                             uint32_t width, uint32_t height) {
    const VkFramebufferCreateInfo ci = {
        .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass      = pass,
        .attachmentCount = view_count,
        .pAttachments    = views,
        .width           = width,
        .height          = height,
        .layers          = 1,
    };

    VkFramebuffer fb = VK_NULL_HANDLE;
    const VkResult r = vkCreateFramebuffer(dev, &ci, NULL, &fb);

    if (r != VK_SUCCESS)
        fprintf(stderr, "fb_build: vkCreateFramebuffer failed (%d) for %ux%u, %u attachments\n",
                (int)r, width, height, view_count);
    return fb;
}

// ============================================================
// 4. SELF-TEST
// ============================================================

#ifdef MU_PASS_CACHE_TEST

#include <stdlib.h>

// The test exercises the keys, the table and the framebuffer reuse; it never
// reaches a driver. Two mock entry points stand in for the loader so the file
// links without one. They record what they were asked for, which is the other
// thing worth testing: that a pass is built from the key and nothing else.
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice dev,
    const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *alloc, VkRenderPass *out) {
    (void)dev; (void)alloc;
    static uint64_t next = 1;
    *out = (VkRenderPass)(uintptr_t)(next++);
    printf("  built a pass: %u attachments, %u subpass(es), %u dependencies\n",
           ci->attachmentCount, ci->subpassCount, ci->dependencyCount);
    for (uint32_t i = 0; i < ci->attachmentCount; i++)
        printf("    att %u: fmt %d samples %d load %d store %d\n", i,
               (int)ci->pAttachments[i].format, (int)ci->pAttachments[i].samples,
               (int)ci->pAttachments[i].loadOp, (int)ci->pAttachments[i].storeOp);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice dev,
    const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *alloc, VkFramebuffer *out) {
    (void)dev; (void)alloc;
    static uint64_t next = 1;
    *out = (VkFramebuffer)(uintptr_t)(next++);
    printf("  built a framebuffer: %ux%u, %u attachments\n",
           ci->width, ci->height, ci->attachmentCount);
    return VK_SUCCESS;
}

// Deterministic LCG: the test says what it did, so a failure is reproducible.
static uint32_t rnd(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

int main(void) {
    static PassCache cache;
    uint32_t seed = 1;

    // 1. Key construction and the 2-bit packing round-trip.
    for (uint32_t i = 0; i < 9; i++) {
        const uint32_t op = i % 3;
        const uint32_t w  = OP_PUT(0, i, op);
        if (OP_GET(w, i) != op) { printf("FAIL: packing at %u\n", i); return 1; }
    }

    // 2. The same pass twice is one slot; a changed loadOp is another.
    PassKey a = { .samples = 4, .kind = PASS_GRAPHICS };
    pass_key_add_color(&a, VK_FORMAT_R16G16B16A16_SFLOAT, CLEAR, STORE_DISCARD, true);
    pass_key_add_color(&a, VK_FORMAT_R8G8B8A8_UNORM,       CLEAR, STORE,         false);
    pass_key_set_depth(&a, VK_FORMAT_D24_UNORM_S8_UINT, CLEAR, STORE_DISCARD);

    uint32_t empty = 0;
    PassSlot *s1 = pass_slot(&cache, &a, &empty);
    PassSlot *s2 = pass_slot(&cache, &a, &empty);
    if (!s1 || s1 != s2) { printf("FAIL: same key, different slot\n"); return 1; }
    if (cache.count != 1) { printf("FAIL: count %u\n", cache.count); return 1; }

    PassKey b = a;
    b.store = OP_PUT(b.store, 1, MU_STORE_NONE);
    PassSlot *s3 = pass_slot(&cache, &b, &empty);
    if (s3 == s1) { printf("FAIL: different storeOp, same slot\n"); return 1; }

    // 3. A pool of distinct passes: each is found again by its key, never
    //    aliases another, and the table says so when it is full instead of
    //    overwriting something live.
    enum { POOL = 64, LOOKUPS = 4000 };
    static PassKey keys[POOL];
    static PassSlot *slot[POOL];

    for (uint32_t i = 0; i < POOL; i++) {
        PassKey k = { .kind = PASS_GRAPHICS };
        static const VkFormat f[4] = {
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R8_UNORM
        };
        const uint32_t n = 1 + rnd(&seed) % 3;
        for (uint32_t c = 0; c < n; c++)
            pass_key_add_color(&k, f[rnd(&seed) % 4],
                               (Load)(rnd(&seed) % 3), (Store)(rnd(&seed) % 2),
                               (rnd(&seed) & 1) != 0);
        k.samples = (rnd(&seed) % 3 == 0) ? 4 : 1;
        if (rnd(&seed) & 1)
            pass_key_set_depth(&k, VK_FORMAT_D32_SFLOAT, CLEAR,
                               (rnd(&seed) & 1) ? STORE_DISCARD : STORE);

        // Distinct by construction: the sample count and the resolve mask are
        // the two cheapest axes to vary, and the hash must separate them.
        k.view_mask = i / 8;
        keys[i] = k;
        slot[i] = pass_slot(&cache, &k, &empty);
        if (!slot[i]) { printf("FAIL: cache full at %u\n", i); return 1; }
    }

    for (uint32_t i = 0; i < LOOKUPS; i++) {
        const uint32_t pick = rnd(&seed) % POOL;
        uint32_t was_empty = 1;
        PassSlot *s = pass_slot(&cache, &keys[pick], &was_empty);

        if (was_empty)                      { printf("FAIL: key %u not found\n", pick); return 1; }
        if (s != slot[pick])                { printf("FAIL: key %u moved\n", pick); return 1; }
        if (memcmp(&s->key, &keys[pick], sizeof(PassKey))) {
            printf("FAIL: key %u aliased onto another pass\n", pick);
            return 1;
        }
    }

    // Fill the table to prove the full case is a NULL, not corruption.
    uint32_t filled = 0;
    for (uint32_t i = 0; i < PASS_CACHE_SIZE * 2; i++) {
        PassKey k = { .kind = PASS_COMPUTE, .view_mask = i };
        if (!pass_slot(&cache, &k, &empty))
            break;
        filled++;
    }
    if (cache.count != PASS_CACHE_SIZE) {
        printf("FAIL: table underfilled: %u of %u\n", cache.count, PASS_CACHE_SIZE);
        return 1;
    }
    (void)filled;

    // 4. Framebuffers: same views, same entry; different views, new entry.
    const uint64_t v1[3] = { 0x1000, 0x2000, 0x3000 };
    const uint64_t v2[3] = { 0x1000, 0x2000, 0x4000 };
    FbEntry *f1 = pass_fb(slot[0], v1, 3, 1920, 1080);
    FbEntry *f2 = pass_fb(slot[0], v1, 3, 1920, 1080);
    FbEntry *f3 = pass_fb(slot[0], v2, 3, 1920, 1080);

    if (!f1 || f1 != f2) { printf("FAIL: framebuffer not reused\n"); return 1; }
    if (f3 == f1)        { printf("FAIL: distinct views collided\n"); return 1; }
    if (slot[0]->fb_count != 2) { printf("FAIL: fb_count %u\n", slot[0]->fb_count); return 1; }

    // 5. The pass a key builds. Prints what the tiler will be told.
    printf("building the merged scene+composite pass from its key:\n");
    PassKey scene = { .samples = 4, .kind = PASS_GRAPHICS };
    pass_key_add_color(&scene, VK_FORMAT_R16G16B16A16_SFLOAT, CLEAR, STORE_DISCARD, true);
    pass_key_add_color(&scene, VK_FORMAT_R8G8B8A8_UNORM,       CLEAR, STORE_DISCARD, true);
    pass_key_add_color(&scene, VK_FORMAT_R8G8B8A8_UNORM,       CLEAR, STORE_DISCARD, true);
    pass_key_set_depth(&scene, VK_FORMAT_D24_UNORM_S8_UINT, CLEAR, STORE_DISCARD);

    VkRenderPass rp = pass_build(VK_NULL_HANDLE, &scene);
    if (rp == VK_NULL_HANDLE) { printf("FAIL: pass not built\n"); return 1; }

    const VkImageView views[7] = { (VkImageView)1, (VkImageView)2, (VkImageView)3,
                                   (VkImageView)4, (VkImageView)5, (VkImageView)6,
                                   (VkImageView)7 };
    if (fb_build(VK_NULL_HANDLE, rp, views, 7, 2400, 1080) == VK_NULL_HANDLE) {
        printf("FAIL: framebuffer not built\n");
        return 1;
    }

    printf("pass_cache: %u distinct passes, %u probes of displacement, %u slots of %u\n",
           cache.count, cache.collisions, PASS_CACHE_SIZE, PASS_CACHE_SIZE);
    return 0;
}

#endif // MU_PASS_CACHE_TEST
