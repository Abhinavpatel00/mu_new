/* tests/smoke.c — headless end-to-end check of vk.c. Every public entry point
 * runs at least once, under validation, except what needs a window: a surface
 * for gpu_set_surface() and with it a frame that changes size, and
 * gpu_raw_instance(). ./toon uses those. What the GPU wrote is read back and
 * compared against what the shaders must have produced. */
#include "../external/mu_lib/mu/mu_macros.h"
#include "../external/mu_lib/mu/mu_math.h"
#include "../external/mu_lib/mu/mu_shapes.h"

#define MU_ESCAPE /* gpu_raw_device, pass_raw_cmd */
#include "../vk.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../external/glfw/deps/stb_image_write.h"

#include <stdio.h>

typedef struct Instance {
    mu_mat4 model;
    mu_vec4 color;
} Instance;

PUSH(SmokePush,
    mu_mat4  view_proj;
    uint64_t instances;
    uint32_t vertex_offset;
    uint32_t index_offset;
    uint32_t texture_id;
    uint32_t sampler_id;
    uint32_t storage_id;
    float    time;
);

int main(void)
{
    enum { W = 640, H = 360, FRAMES = 5, BLOB = 3 * 1024 * 1024, DEPTH_SIZE = 64, CLIP_SIZE = 64, LEVEL_SIZE = 32 };
    /* the "clip" pass: a 2x2 source of four colours, then the colour the target is cleared to */
    static const uint8_t QUAD[5][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 }, { 0, 0, 0, 255 } };
    /* and which of those five a pixel of the target must hold afterwards */
    static const struct { uint8_t x, y, colour; } CLIP_PIXELS[] = {
        { 8, 4, 0 },   { 23, 11, 0 }, { 24, 4, 1 },  { 39, 11, 1 }, { 8, 12, 2 }, { 23, 19, 2 }, { 24, 12, 3 }, { 39, 19, 3 }, /* the small viewport */
        { 7, 4, 4 },   { 40, 4, 4 },  { 8, 3, 4 },   { 8, 20, 4 },                                                             /* just outside it */
        { 44, 40, 3 }, { 63, 63, 3 }, { 43, 40, 4 }, { 44, 39, 4 },                                                            /* the scissor that reached past the target */
        { 0, 0, 0 },   { 3, 3, 0 },   { 4, 3, 4 },   { 3, 4, 4 },                                                              /* the one that began outside it */
    };
    /* the "follow" pass: the same source over a whole image of the frame's size, so each colour has a quarter of W x H */
    static const struct { uint16_t x, y, colour; } FOLLOW_PIXELS[] = {
        { 0, 0, 0 },         { W / 2 - 1, H / 2 - 1, 0 }, { W / 2, 0, 1 },     { W - 1, H / 2 - 1, 1 }, /* the top half */
        { 0, H / 2, 2 },     { W / 2 - 1, H - 1, 2 },     { W / 2, H / 2, 3 }, { W - 1, H - 1, 3 },     /* the bottom half */
    };
    Gpu       *g = gpu_create(&(GpuDesc){ .name = "smoke", .width = W, .height = H, .enable_validation = true, .enable_graphics_profiler = true, .enable_pipeline_stats = true });
    GpuInfo    info = gpu_info(g);
    mu_shape   cube = mu_shape_cube(1, 1.0f);
    Range      vertices, indices, instances, counter, arguments;
    ShaderId   vs, fs, fs_tint, vs_fsq, fs_blit, cs;
    Pipe       pipe_scene, pipe_tint, pipe_depth, pipe_blit, pipe_pattern;
    Image      color, depth, pattern, msaa, checker, mips, chain, shadow, depth_view, depth_read, levels, quad, clip, follow, follow_small, follow_dot;
    Buffer     big, host, dispatch_arguments;
    const FrameStats *stats;
    uint8_t   *blob;
    Sampler    linear, nearest;
    Upload    *upload;
    Readback  *shot = NULL, *bytes_back;
    SmokePush  push;
    Pass      *pass;
    mu_mat4    view, proj;
    Instance  *instance_data;
    static uint8_t  pixels[W * H * 4];
    static uint16_t depth_back[DEPTH_SIZE * DEPTH_SIZE * 4], read_back[DEPTH_SIZE * DEPTH_SIZE * 4]; /* RGBA16F */
    static uint8_t  clip_back[CLIP_SIZE * CLIP_SIZE * 4];
    static uint8_t  level_bytes[(LEVEL_SIZE * LEVEL_SIZE + LEVEL_SIZE * LEVEL_SIZE / 4) * 4], level_back[LEVEL_SIZE * LEVEL_SIZE * 4]; /* two mips; one */
    static uint32_t index_back[36];
    uint8_t    texels[(16 + 4 + 1) * 4], texel_back[4 * 4]; /* a 4x4 image and its two smaller mips */
    uint32_t   frame, i, errors = 0, mismatches = 0, level_ids[2], follow_ids[2];
    double     parts[MAX_PASSES], pass_sum;
    VkDrawIndirectCommand indirect = { 36, 2, 0, 0 };
    uint32_t   groups[3] = { 8, 8, 1 }; /* VkDispatchIndirectCommand */
    uint32_t   one = 1;

    printf("device: %s, %ux%u, subgroup %u, push %u, sample counts 0x%x\n", info.name, info.width, info.height, info.subgroup_size, info.max_push_constants_size,
           info.sample_counts);
    errors += (info.sample_counts & (SC_1 | SC_4)) != (SC_1 | SC_4); /* the two every device has; the scene below uses SC_4 */
    mu_shape_compute_normals(&cube);

    /* geometry lives in the device arena and is read through binding 3 */
    vertices  = arena_alloc(g, ARENA_DEVICE, cube.npoints * sizeof *cube.verts, 16);
    indices   = arena_alloc(g, ARENA_DEVICE, cube.ntriangles * 3 * sizeof(uint32_t), 16);
    arguments = arena_alloc(g, ARENA_DEVICE, sizeof indirect, 16);
    counter   = arena_alloc(g, ARENA_DEVICE, sizeof one, 16);
    upload    = upload_begin(g);
    upload_buffer(upload, vertices, (DataSpan){ cube.verts, (uint32_t)vertices.size });
    upload_buffer(upload, indices, (DataSpan){ cube.triangles, (uint32_t)indices.size });
    upload_buffer(upload, arguments, BYTE_SPAN(indirect));
    upload_buffer(upload, counter, BYTE_SPAN(one));
    upload_end(g, upload);

    /* 40 x 3 MB through the 32 MB staging ring: it wraps, and has to wait for
     * the timeline instead of the queue. The last copy must be the one that sticks. */
    big  = buffer_device(g, BLOB);
    blob = malloc(BLOB);
    for (i = 0; i < 40; ++i)
    {
        memset(blob, (int)(i + 1), BLOB);
        upload = upload_begin(g);
        upload_buffer(upload, RANGE(big, BLOB), (DataSpan){ blob, BLOB });
        upload_end(g, upload);
    }
    bytes_back = readback_buffer(g, RANGE_AT(big, BLOB - 64, 64), (DataSpan){ index_back, 64 });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    errors += memcmp(index_back, blob, 64) != 0;
    buffer_destroy(g, big);
    free(blob);

    /* host memory: written through the mapping, copied by the GPU, read back */
    host = buffer_host(g, 4096);
    memcpy(buffer_info(g, host).mapped, cube.triangles, sizeof index_back);
    bytes_back = readback_buffer(g, RANGE(host, sizeof index_back), (DataSpan){ index_back, sizeof index_back });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    errors += memcmp(index_back, cube.triangles, sizeof index_back) != 0;
    buffer_destroy(g, host);
    memset(index_back, 0, sizeof index_back);

    /* a buffer of its own for the indirect dispatch */
    dispatch_arguments = buffer_create(g, &(BufferDesc){ .name = "dispatch arguments", .size = sizeof groups, .usage = BUF_INDIRECT | BUF_XFER_DST });
    upload             = upload_begin(g);
    upload_buffer(upload, RANGE(dispatch_arguments, sizeof groups), BYTE_SPAN(groups));
    upload_end(g, upload);

    /* three mips in one upload; the smallest two come back texel for texel */
    for (i = 0; i < sizeof texels; ++i)
        texels[i] = (uint8_t)(i * 7 + 3);
    mips   = image_create(g, &(ImageDesc){ .name = "mips", .width = 4, .height = 4, .mips = 3, .format = FMT_RGBA8_UNORM, .usage = IMG_SAMPLED | IMG_XFER_DST | IMG_XFER_SRC });
    upload = upload_begin(g);
    upload_image(upload, SUB_MIP(mips, 0, 3), BYTE_SPAN(texels));
    upload_end(g, upload);
    bytes_back = readback_image(g, SUB_MIP(mips, 1, 1), (DataSpan){ texel_back, 16 });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    errors += memcmp(texel_back, texels + 64, 16) != 0;
    bytes_back = readback_image(g, SUB_MIP(mips, 2, 1), (DataSpan){ texel_back, 4 });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    errors += memcmp(texel_back, texels + 80, 4) != 0;
    image_destroy(g, mips);

    /* a destroyed slot comes back with a new generation; an alias reuses bytes */
    checker = image_from_bytes(g, 2, 2, FMT_RGBA8_UNORM, (uint8_t[16]){ 255, 255, 255, 255, 40, 40, 40, 255, 40, 40, 40, 255, 255, 255, 255, 255 }, 16);
    image_destroy(g, checker);
    errors += image_from_bytes(g, 1, 1, FMT_RGBA8_UNORM, (uint8_t[4]){ 1, 2, 3, 4 }, 4).gen == checker.gen;
    instances = arena_alloc(g, ARENA_TRANSIENT, 256, 64);
    errors   += arena_alias(g, ARENA_TRANSIENT, instances, 128, 64).offset != instances.offset;
    errors   += arena_alias(g, ARENA_TRANSIENT, instances, 512, 64).offset == instances.offset;

    vs      = shader_load(g, &(ShaderDesc){ .path = "tests/smoke.slang", .entry = "vs_main", .stage = STAGE_VERTEX });
    fs      = shader_load(g, &(ShaderDesc){ .path = "tests/smoke.slang", .entry = "fs_main", .stage = STAGE_FRAGMENT });
    fs_tint = shader_load(g, &(ShaderDesc){ .path = "tests/smoke.slang", .entry = "fs_tint", .stage = STAGE_FRAGMENT });
    vs_fsq  = shader_load(g, &(ShaderDesc){ .path = "tests/smoke.slang", .entry = "vs_fullscreen", .stage = STAGE_VERTEX });
    fs_blit = shader_load(g, &(ShaderDesc){ .path = "tests/smoke.slang", .entry = "fs_blit", .stage = STAGE_FRAGMENT });
    cs      = shader_load(g, &(ShaderDesc){ .path = "tests/smoke.slang", .entry = "cs_pattern", .stage = STAGE_COMPUTE });
    pipe_scene   = pipe_graphics(g, vs, fs);
    pipe_tint    = pipe_graphics(g, vs, fs_tint);
    pipe_depth   = pipe_create(g, &(PipeDesc){ .name = "depth only", .vertex = vs }); /* no fragment shader */
    pipe_blit    = pipe_graphics(g, vs_fsq, fs_blit);
    pipe_pattern = pipe_compute(g, cs);
    MU_ASSERT(pipe_graphics(g, vs, fs).index == pipe_scene.index); /* interned */
    pipe_destroy(g, pipe_scene);                                   /* drops the second reference only */

    msaa    = image_create(g, &(ImageDesc){ .name = "msaa", .width = W, .height = H, .format = FMT_RGBA16F, .usage = IMG_ATTACH, .samples = SC_4, .transient = true });
    depth   = image_create(g, &(ImageDesc){ .name = "depth", .width = W, .height = H, .format = FMT_D32F, .usage = IMG_ATTACH, .samples = SC_4, .transient = true });
    color   = image_color(g, W, H, FMT_RGBA16F);
    pattern = image_storage(g, 64, 64, FMT_RGBA8_UNORM);
    shadow     = image_depth(g, DEPTH_SIZE, DEPTH_SIZE);
    depth_view = image_color(g, DEPTH_SIZE, DEPTH_SIZE, FMT_RGBA16F);
    depth_read = image_color(g, DEPTH_SIZE, DEPTH_SIZE, FMT_RGBA16F);
    quad       = image_from_bytes(g, 2, 2, FMT_RGBA8_UNORM, QUAD, 16);
    clip       = image_create(g, &(ImageDesc){ .name = "clip", .width = CLIP_SIZE, .height = CLIP_SIZE, .format = FMT_RGBA8_UNORM, .usage = IMG_ATTACH | IMG_XFER_SRC });
    linear  = sampler_linear_repeat(g);
    nearest = sampler_nearest_clamp(g);

    /* A mip drawn from the mip above it: one is rendered into while the other is sampled, each through a
     * view of its own. Mip 0 is four 2x2 blocks of one colour each, so mip 1 has to come out as exactly
     * those four colours whatever the filter does. Mips 1 and 2 start as something else. */
    for (i = 0; i < 16; ++i)
        memcpy(texels + i * 4, (uint8_t[4]){ (uint8_t)(40 + 50 * (i % 4 / 2)), (uint8_t)(60 + 90 * (i / 8)), 200, 255 }, 4);
    memset(texels + 64, 9, 20);
    chain  = image_create(g, &(ImageDesc){ .name = "chain", .width = 4, .height = 4, .mips = 3, .format = FMT_RGBA8_UNORM, .usage = IMG_ATTACH | IMG_SAMPLED | IMG_XFER_DST | IMG_XFER_SRC });
    upload = upload_begin(g);
    upload_image(upload, SUB_MIP(chain, 0, 3), BYTE_SPAN(texels));
    upload_end(g, upload);
    memset(&push, 0, sizeof push);
    push.texture_id = image_sub_sampled_id(g, SUB_MIP(chain, 0, 1));
    push.sampler_id = sampler_id(g, nearest);
    pass = begin_pass(g, &(PassDesc){ .name = "mip", .pipe = pipe_blit, .colors = { attach_sub(SUB_MIP(chain, 1, 1), DONT_CARE, STORE) }, .color_count = 1,
                                      .images = { use_image(SUB_MIP(chain, 0, 1), ACC_READ, IMAGE_SAMPLED) }, .image_count = 1 });
    draw(pass, BYTE_SPAN(push), 3, 1);
    end_pass(pass);
    bytes_back = readback_image(g, SUB_MIP(chain, 1, 1), (DataSpan){ texel_back, 16 });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    errors += memcmp(texel_back, (uint8_t[16]){ 40, 60, 200, 255, 90, 60, 200, 255, 40, 150, 200, 255, 90, 150, 200, 255 }, 16) != 0;
    bytes_back = readback_image(g, SUB_MIP(chain, 2, 1), (DataSpan){ texel_back, 4 });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    errors += memcmp(texel_back, (uint8_t[4]){ 9, 9, 9, 9 }, 4) != 0; /* the mip nobody named is as it was */
    /* the whole image is the image's own id; a part has one of its own, and keeps it */
    errors += image_sub_sampled_id(g, SUB_MIP(chain, 0, 3)) != image_sampled_id(g, chain);
    errors += push.texture_id == image_sampled_id(g, chain) || push.texture_id != image_sub_sampled_id(g, SUB_MIP(chain, 0, 1));
    image_destroy(g, chain);

    /* Compute writes one mip through the storage index of that mip: the pattern has to land in mip 1,
     * and mip 0, which the image's own index reaches, has to stay as it was uploaded. */
    memset(level_bytes, 9, sizeof level_bytes);
    levels = image_create(g, &(ImageDesc){ .name = "levels", .width = LEVEL_SIZE, .height = LEVEL_SIZE, .mips = 2, .format = FMT_RGBA8_UNORM, .usage = IMG_STORAGE | IMG_XFER_DST | IMG_XFER_SRC });
    upload = upload_begin(g);
    upload_image(upload, SUB_MIP(levels, 0, 2), BYTE_SPAN(level_bytes));
    upload_end(g, upload);
    memset(&push, 0, sizeof push);
    push.storage_id = image_sub_storage_id(g, SUB_MIP(levels, 1, 1));
    pass = begin_pass(g, &(PassDesc){ .name = "level", .kind = PASS_COMPUTE, .pipe = pipe_pattern, .images = { use_image(SUB_MIP(levels, 1, 1), ACC_WRITE, IMAGE_STORAGE) }, .image_count = 1 });
    dispatch(pass, BYTE_SPAN(push), LEVEL_SIZE / 16, LEVEL_SIZE / 16, 1);
    end_pass(pass);
    bytes_back = readback_image(g, SUB_MIP(levels, 1, 1), (DataSpan){ level_back, LEVEL_SIZE * LEVEL_SIZE });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    /* cs_pattern: 8x8 cells, blue 0.2 or 0.8 by the cell's parity, alpha 1 */
    for (i = 0; i < LEVEL_SIZE * LEVEL_SIZE / 4; ++i)
        mismatches += level_back[i * 4 + 2] != ((i % (LEVEL_SIZE / 2) / 8 + i / (LEVEL_SIZE / 2) / 8) & 1 ? 204 : 51) || level_back[i * 4 + 3] != 255;
    bytes_back = readback_image(g, SUB_MIP(levels, 0, 1), (DataSpan){ level_back, sizeof level_back });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    errors += mismatches != 0 || memcmp(level_back, level_bytes, sizeof level_back) != 0;
    /* as with sampling: the whole image is the image's own index, a part has one of its own, and keeps it */
    errors += image_sub_storage_id(g, SUB_MIP(levels, 0, 2)) != image_storage_id(g, levels);
    errors += push.storage_id == image_storage_id(g, levels) || push.storage_id != image_sub_storage_id(g, SUB_MIP(levels, 1, 1));
    level_ids[0] = image_storage_id(g, levels);
    level_ids[1] = push.storage_id;
    image_destroy(g, levels);

    /* Images without a size of their own are as large as the frame. One at the frame's size has the quad drawn over
     * all of it and is read back whole: the four colours have to meet in the middle of W x H. One at a sixteenth of
     * the frame asks for more mips than 40x22 has, and has to come out with those six: the last is one pixel, and
     * mips 0 to 5 are the whole image. One is halved until nothing would be left, and is one pixel. Then
     * gpu_set_surface() looks for images to build again and has to find none, because this frame keeps its size:
     * all are what they were, indices and contents. */
    follow       = image_create(g, &(ImageDesc){ .name = "follow", .format = FMT_RGBA8_UNORM, .usage = IMG_ATTACH | IMG_SAMPLED | IMG_XFER_SRC });
    follow_small = image_create(g, &(ImageDesc){ .name = "follow small", .frame_shift = 4, .mips = 12, .format = FMT_RGBA8_UNORM, .usage = IMG_ATTACH | IMG_SAMPLED | IMG_XFER_SRC });
    follow_dot   = image_create(g, &(ImageDesc){ .name = "follow dot", .frame_shift = 12, .mips = 12, .format = FMT_RGBA8_UNORM, .usage = IMG_ATTACH | IMG_SAMPLED | IMG_XFER_SRC });
    memset(&push, 0, sizeof push);
    push.texture_id = image_sampled_id(g, quad);
    push.sampler_id = sampler_id(g, nearest);
    pass = begin_pass(g, &(PassDesc){ .name = "follow", .pipe = pipe_blit, .colors = { attach_write(follow) }, .color_count = 1, .images = { use_sampled(quad) }, .image_count = 1 });
    draw(pass, BYTE_SPAN(push), 3, 1);
    end_pass(pass);
    /* nothing is drawn: the two clears are the test, and one pass takes both targets only if both are one pixel */
    pass = begin_pass(g, &(PassDesc){ .name = "follow small", .color_count = 2,
                                      .colors = { { .image = SUB_MIP(follow_small, 5, 1), .load = CLEAR, .store = STORE, .clear = { 0.2f, 0.4f, 0.6f, 1.0f } },
                                                  { .image = SUB(follow_dot), .load = CLEAR, .store = STORE, .clear = { 0.6f, 0.4f, 0.2f, 1.0f } } } });
    end_pass(pass);
    follow_ids[0] = image_sampled_id(g, follow);
    follow_ids[1] = image_sub_sampled_id(g, SUB_MIP(follow_small, 5, 1));
    errors       += image_sub_sampled_id(g, SUB_MIP(follow_small, 0, 6)) != image_sampled_id(g, follow_small);
    errors       += image_sub_sampled_id(g, SUB_MIP(follow_dot, 0, 1)) != image_sampled_id(g, follow_dot);
    errors       += !gpu_set_surface(g, VK_NULL_HANDLE) || gpu_info(g).width != W || gpu_info(g).height != H;
    errors       += image_sampled_id(g, follow) != follow_ids[0] || image_sub_sampled_id(g, SUB_MIP(follow_small, 5, 1)) != follow_ids[1];
    bytes_back    = readback_image(g, SUB(follow), (DataSpan){ pixels, sizeof pixels }); /* a larger image would not fit */
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    for (i = 0; i < MU_ARRAY_COUNT(FOLLOW_PIXELS); ++i)
        errors += memcmp(pixels + (FOLLOW_PIXELS[i].y * W + FOLLOW_PIXELS[i].x) * 4, QUAD[FOLLOW_PIXELS[i].colour], 4) != 0;
    bytes_back = readback_image(g, SUB_MIP(follow_small, 5, 1), (DataSpan){ texel_back, 4 }); /* nor would anything but one pixel */
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    errors    += memcmp(texel_back, (uint8_t[4]){ 51, 102, 153, 255 }, 4) != 0;
    bytes_back = readback_image(g, SUB(follow_dot), (DataSpan){ texel_back, 4 });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    errors += memcmp(texel_back, (uint8_t[4]){ 153, 102, 51, 255 }, 4) != 0;
    image_destroy(g, follow);
    image_destroy(g, follow_small);
    image_destroy(g, follow_dot);

    /* A frame with more passes than it has rows for. The ones beyond MAX_PASSES still run, untimed, and a scope
     * opened in one of them is not kept: of the eight below, the frame has the two of its last two rows. */
    memset(&push, 0, sizeof push);
    push.texture_id = image_sampled_id(g, quad);
    push.sampler_id = sampler_id(g, nearest);
    frame_begin(g);
    for (i = 0; i < MAX_PASSES + 6; ++i)
    {
        pass = begin_pass(g, &(PassDesc){ .name = "over", .pipe = pipe_blit, .colors = { attach_write(clip) }, .color_count = 1, .images = { use_sampled(quad) }, .image_count = 1 });
        if (i >= MAX_PASSES - 2)
            pass_scope(pass, "over");
        draw(pass, BYTE_SPAN(push), 3, 1);
        end_pass(pass);
    }
    frame_end(g);
    gpu_wait_idle(g);
    stats   = gpu_stats(g);
    errors += stats->pass_count != MAX_PASSES || stats->draws != MAX_PASSES + 6 || stats->scope_count != 2 || stats->scopes[1].pass != MAX_PASSES - 1;

    for (frame = 0; frame < FRAMES; ++frame)
    {
        gpu_profile(g, frame != 1); /* one frame goes untimed */
        if (!frame_begin(g))
            continue;

        /* per-frame data: written straight into the transient arena, read by address */
        instances     = arena_alloc(g, ARENA_TRANSIENT, 2 * sizeof(Instance), 16);
        instance_data = (Instance *)((uint8_t *)buffer_info(g, instances.buffer).mapped + instances.offset);
        for (i = 0; i < 2; ++i)
        {
            mu_mat4 rotation, translation;
            mu_mat4_rotate(rotation, (mu_vec3){ 0.3f, 1.0f, 0.2f }, 0.6f + 0.4f * (float)frame + (float)i);
            mu_mat4_translate(translation, (mu_vec3){ i ? 1.1f : -1.1f, 0.0f, 0.0f });
            mu_mat4_mul(translation, rotation, instance_data[i].model);
            mu_vec4_copy(instance_data[i].color, i ? (mu_vec4){ 0.4f, 0.8f, 1.0f, 1.0f } : (mu_vec4){ 1.0f, 0.6f, 0.5f, 1.0f });
        }

        mu_mat4_look_at(view, (mu_vec3){ 0.0f, 1.2f, 4.0f }, (mu_vec3){ 0.0f, 0.0f, 0.0f }, (mu_vec3){ 0.0f, 1.0f, 0.0f });
        mu_mat4_perspective_zo(proj, mu_radians(45.0f), (float)W / (float)H, 50.0f, 0.1f); /* far and near swapped: reverse-Z */
        memset(&push, 0, sizeof push);
        mu_mat4_mul(proj, view, push.view_proj);
        push.instances     = buffer_info(g, instances.buffer).address + instances.offset;
        push.vertex_offset = (uint32_t)vertices.offset;
        push.index_offset  = (uint32_t)indices.offset;
        push.texture_id    = image_sampled_id(g, pattern);
        push.sampler_id    = sampler_id(g, linear);
        push.storage_id    = image_storage_id(g, pattern);

        /* the same 8x8 groups, from the argument buffer on odd frames */
        pass = begin_pass(g, &(PassDesc){ .name = "pattern", .kind = PASS_COMPUTE, .pipe = pipe_pattern, .images = { use_storage(pattern) }, .image_count = 1,
                                          .buffers = { use_read(RANGE(dispatch_arguments, sizeof groups)) }, .buffer_count = 1 });
        pass_note(pass, (PassNote){ .workgroup = { 8, 8, 1 }, .tile_size = 8 }); /* what cs_pattern is, said next to its time */
        pass_scope(pass, "dispatch"); /* a scope in a compute pass */
        if (frame & 1)
            dispatch_indirect(pass, BYTE_SPAN(push), RANGE(dispatch_arguments, sizeof groups));
        else
            dispatch(pass, BYTE_SPAN(push), groups[0], groups[1], groups[2]);
        errors += pass_raw_cmd(pass) == VK_NULL_HANDLE;
        end_pass(pass);

        /* depth only: the left cube into a depth image, no colour and no fragment shader */
        pass = begin_pass(g, &(PassDesc){ .name = "depth only", .pipe = pipe_depth, .state = draw_state_depth(SC_1), .depth = attach_clear_depth(shadow, 0.0f), .has_depth = true });
        draw_indexed(pass, BYTE_SPAN(push), cube.ntriangles * 3, 1);
        end_pass(pass);

        /* That depth, tested against and sampled in one pass. The triangle is at depth 0, the clear value, so
         * LESS passes where the cube is and nowhere else; there it writes the depth it samples. The state asks
         * for depth writes, and a read-only attachment does not take them: the next pass still finds the cube. */
        push.texture_id = image_sampled_id(g, shadow);
        push.sampler_id = sampler_id(g, nearest);
        pass = begin_pass(g, &(PassDesc){ .name = "depth read", .pipe = pipe_blit, .state = { .depth_compare = CMP_LESS, .depth_write = 1, .write_mask = 0xffff },
                                          .colors = { attach_clear(depth_read, 0.25f, 0.5f, 0.75f, 1.0f) }, .color_count = 1, .depth = attach_depth_read(shadow), .has_depth = true,
                                          .images = { use_sampled(shadow) }, .image_count = 1 });
        draw(pass, BYTE_SPAN(push), 3, 1);
        end_pass(pass);

        /* that depth, sampled, into something a readback can look at */
        pass = begin_pass(g, &(PassDesc){ .name = "depth view", .pipe = pipe_blit, .colors = { attach_write(depth_view) }, .color_count = 1, .images = { use_sampled(shadow) }, .image_count = 1 });
        draw(pass, BYTE_SPAN(push), 3, 1);
        end_pass(pass);
        push.texture_id = image_sampled_id(g, pattern);
        push.sampler_id = sampler_id(g, linear);

        pass = begin_pass(g, &(PassDesc){
            .name        = "scene",
            .pipe        = pipe_scene,
            .state       = draw_state_opaque(SC_4, CULL_BACK),
            .colors      = { { .image = SUB(msaa), .load = CLEAR, .store = STORE_DISCARD, .clear = { 0.05f, 0.07f, 0.12f, 1.0f }, .resolve = SUB(color) } },
            .color_count = 1,
            .depth       = { .image = SUB(depth), .load = CLEAR, .store = STORE_DISCARD },
            .has_depth   = true,
            .images      = { use_sampled(pattern) },
            .image_count = 1,
            .buffers     = { use_read(arguments), use_read(counter) },
            .buffer_count = 2,
        });
        pass_scope(pass, "cubes"); /* the pass in two parts: the three cube draws, then the blended triangle */
        draw_indexed(pass, BYTE_SPAN(push), cube.ntriangles * 3, 1);         /* left cube only */
        set_state(pass, draw_state_opaque(SC_4, CULL_BACK));                  /* the state it has: nothing is emitted, the run goes on */
        draw_indirect_count(pass, BYTE_SPAN(push), arguments, counter, 1, 0); /* both cubes */
        draw_indirect(pass, BYTE_SPAN(push), arguments, 1, 0);
        pass_scope(pass, "tint");
        bind_pipe(pass, pipe_tint); /* another fragment shader, mid-pass, blended */
        set_state(pass, draw_state_alpha(SC_4, CULL_NONE));
        draw(pass, BYTE_SPAN(push), 3, 1);
        end_pass(pass);

        /* viewport and scissor: the 2x2 source drawn small, then twice through a scissor that the pass has to cut down */
        push.texture_id = image_sampled_id(g, quad);
        push.sampler_id = sampler_id(g, nearest);
        pass = begin_pass(g, &(PassDesc){ .name = "clip", .pipe = pipe_blit, .colors = { attach_clear(clip, 0.0f, 0.0f, 0.0f, 1.0f) }, .color_count = 1, .images = { use_sampled(quad) }, .image_count = 1 });
        /* one frame opens more scopes than a frame keeps: the calls beyond that do nothing */
        for (i = 0; i < MAX_PROFILE_SCOPES + 6 && frame == 2; ++i)
            pass_scope(pass, "many");
        set_viewport(pass, 8.0f, 4.0f, 32.0f, 16.0f);
        draw(pass, BYTE_SPAN(push), 3, 1);
        set_viewport(pass, 0.0f, 0.0f, CLIP_SIZE, CLIP_SIZE);
        set_scissor(pass, 44, 40, 100, 100); /* ends outside: the corner from (44,40) is left */
        draw(pass, BYTE_SPAN(push), 3, 1);
        set_scissor(pass, -5, -5, 9, 9); /* begins outside: the 4x4 corner is left */
        draw(pass, BYTE_SPAN(push), 3, 1);
        set_scissor(pass, -9, -9, 13, 13); /* another way to say that corner: nothing is emitted, the run goes on */
        draw(pass, BYTE_SPAN(push), 3, 1);
        /* An empty scissor: the next three draws leave no pixel and only count. Each follows one change, and each change starts a run. */
        set_scissor(pass, 20, 20, 0, 0);
        draw(pass, BYTE_SPAN(push), 3, 1);
        set_state(pass, draw_state_alpha(SC_1, CULL_NONE)); /* the state alone */
        draw(pass, BYTE_SPAN(push), 3, 1);
        bind_pipe(pass, pipe_tint); /* the pipeline alone */
        draw(pass, BYTE_SPAN(push), 3, 1);
        end_pass(pass);

        push.texture_id = image_sampled_id(g, color);
        pass = begin_pass(g, &(PassDesc){ .name = "blit", .pipe = pipe_blit, .colors = { attach_write(frame_image(g)) }, .color_count = 1, .images = { use_sampled(color) }, .image_count = 1 });
        pass_note(pass, (PassNote){ .samples = 1, .effective_samples = 1 });
        draw(pass, BYTE_SPAN(push), 3, 1);
        end_pass(pass);

        shot = frame == FRAMES - 1 ? readback_image(g, SUB(frame_image(g)), (DataSpan){ pixels, sizeof pixels }) : NULL;
        frame_end(g);

        /* a frame's numbers are there once it has left the GPU; only the timed ones carry a GPU time */
        gpu_wait_idle(g);
        stats   = gpu_stats(g);
        /* 15 draws in 12 runs: a run ends where the pipeline, the state, the viewport or the scissor changes */
        errors += stats->pass_count != 7 || stats->dispatches != 1 || stats->draws != 15 || stats->state_runs != 12;
        /* "depth view" follows a pass that only read the depth image, so its one barrier is for its own target */
        errors += stats->passes[3].barriers != 1;
        errors += (stats->gpu_ms > 0.0) != (frame != 1);
        /* a note is in the row of its pass ("pattern" is the first, "blit" the last), and a pass without one has zeros */
        errors += stats->passes[0].note.workgroup[1] != 8 || stats->passes[0].note.tile_size != 8 || stats->passes[6].note.samples != 1;
        errors += memcmp(&stats->passes[1].note, &(PassNote){ 0 }, sizeof(PassNote)) != 0;
        /* scopes: one in "pattern" (pass 0), two in "scene" (pass 4), and in frame 2 as many of "clip" (pass 5) as fit */
        errors += stats->scope_count != (frame == 2 ? MAX_PROFILE_SCOPES : 3);
        errors += strcmp(stats->scopes[0].name, "dispatch") != 0 || strcmp(stats->scopes[1].name, "cubes") != 0 || strcmp(stats->scopes[2].name, "tint") != 0;
        errors += stats->scopes[0].pass != 0 || stats->scopes[1].pass != 4 || stats->scopes[2].pass != 4;
        errors += frame == 2 && (stats->scopes[MAX_PROFILE_SCOPES - 1].pass != 5 || strcmp(stats->scopes[MAX_PROFILE_SCOPES - 1].name, "many") != 0);
        /* each of the three holds work, so it took time; without timing none has a time */
        for (i = 0; i < 3; ++i)
            errors += (stats->scopes[i].gpu_ms > 0.0) != (frame != 1);
        /* The scopes of a pass are parts of it and the passes are parts of the frame: the parts never take longer
         * than the whole, give or take the rounding of the sums. */
        memset(parts, 0, sizeof parts);
        for (i = 0; i < stats->scope_count; ++i)
            parts[stats->scopes[i].pass] += stats->scopes[i].gpu_ms;
        for (i = 0, pass_sum = 0.0; i < stats->pass_count; ++i)
        {
            errors   += parts[i] > stats->passes[i].gpu_ms + 1e-6 || (stats->passes[i].gpu_ms > 0.0) != (frame != 1);
            pass_sum += stats->passes[i].gpu_ms;
        }
        errors += pass_sum > stats->gpu_ms + 1e-6;
    }

    readback_ready(g, shot, true);
    readback_release(g, shot);
    bytes_back = readback_buffer(g, indices, (DataSpan){ index_back, sizeof index_back });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    bytes_back = readback_image(g, SUB(depth_view), (DataSpan){ depth_back, sizeof depth_back });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    bytes_back = readback_image(g, SUB(depth_read), (DataSpan){ read_back, sizeof read_back });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    bytes_back = readback_image(g, SUB(clip), (DataSpan){ clip_back, sizeof clip_back });
    readback_ready(g, bytes_back, true);
    readback_release(g, bytes_back);
    stbi_write_png("tests/smoke.png", W, H, 4, pixels, W * 4);

    /* the corner is the clear colour, the centre row crosses both cubes */
    errors += !(pixels[0] < 40 && pixels[2] > pixels[0]);
    errors += memcmp(index_back, cube.triangles, sizeof index_back) != 0;
    /* reverse-Z: the cleared corner is 0, the cube (centred a third of the way across) is nearer, so larger */
    errors += depth_back[0] != 0 || depth_back[(DEPTH_SIZE / 2 * DEPTH_SIZE + DEPTH_SIZE / 3) * 4] == 0;
    /* depth read: on the cube the test passed and the pass wrote the depth it sampled, the one the depth view
     * shows; everywhere else the test failed and the clear colour (0.25, 0.5, 0.75, 1 as half floats) is left */
    for (i = 0, mismatches = 0; i < DEPTH_SIZE * DEPTH_SIZE; ++i)
        mismatches += depth_back[i * 4] ? read_back[i * 4] != depth_back[i * 4] : memcmp(read_back + i * 4, (uint16_t[4]){ 0x3400, 0x3800, 0x3a00, 0x3c00 }, 8) != 0;
    errors += mismatches != 0;
    for (i = 0; i < MU_ARRAY_COUNT(CLIP_PIXELS); ++i)
        errors += memcmp(clip_back + (CLIP_PIXELS[i].y * CLIP_SIZE + CLIP_PIXELS[i].x) * 4, QUAD[CLIP_PIXELS[i].colour], 4) != 0;
    errors += gpu_raw_device(g) == VK_NULL_HANDLE;
    errors += gpu_error(g)[0] != 0;
    printf("corner %u %u %u, centre-left %u %u %u, centre-right %u %u %u\n", pixels[0], pixels[1], pixels[2],
           pixels[(H / 2 * W + W / 4) * 4], pixels[(H / 2 * W + W / 4) * 4 + 1], pixels[(H / 2 * W + W / 4) * 4 + 2],
           pixels[(H / 2 * W + 3 * W / 4) * 4], pixels[(H / 2 * W + 3 * W / 4) * 4 + 1], pixels[(H / 2 * W + 3 * W / 4) * 4 + 2]);

    /* The GPU is long past the image destroyed before the first frame, so both of its storage indices are
     * free again, and indices are handed out lowest first: an image made the same way gets the same two. */
    levels  = image_create(g, &(ImageDesc){ .name = "levels again", .width = LEVEL_SIZE, .height = LEVEL_SIZE, .mips = 2, .format = FMT_RGBA8_UNORM, .usage = IMG_STORAGE });
    errors += image_storage_id(g, levels) != level_ids[0] || image_sub_storage_id(g, SUB_MIP(levels, 1, 1)) != level_ids[1];
    image_destroy(g, levels);

    gpu_wait_idle(g);
    gpu_dump_stats(g, NULL);
    printf("arena device used %llu, transient peak %llu\n", (unsigned long long)arena_stats(g, ARENA_DEVICE).used, (unsigned long long)arena_stats(g, ARENA_TRANSIENT).peak);

    image_destroy(g, msaa);
    image_destroy(g, depth);
    image_destroy(g, color);
    image_destroy(g, pattern);
    image_destroy(g, shadow);
    image_destroy(g, depth_view);
    image_destroy(g, depth_read);
    image_destroy(g, quad);
    image_destroy(g, clip);
    buffer_destroy(g, dispatch_arguments);
    sampler_destroy(g, linear);
    sampler_destroy(g, nearest);
    pipe_destroy(g, pipe_scene);
    pipe_destroy(g, pipe_tint);
    pipe_destroy(g, pipe_depth);
    pipe_destroy(g, pipe_blit);
    pipe_destroy(g, pipe_pattern);
    mu_shape_free(&cube);
    printf("%s (%u errors)%s%s\n", errors ? "FAILED" : "OK", errors, gpu_error(g)[0] ? ": " : "", gpu_error(g));
    gpu_destroy(g);
    return errors != 0;
}
