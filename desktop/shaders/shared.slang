/* shaders/shared.slang — the vk.h shader ABI and the records main.c writes.
 * ============================================================================
 *  One descriptor set, bound once, plus <= 256 bytes of push constants:
 *    binding 0  sampled images[]      binding 2  storage images[]
 *    binding 1  samplers[]            binding 3  the device arena (bytes)
 *  Geometry is pulled, never bound: main.c passes byte offsets into binding 3
 *  for static meshes and a device address for the per-frame instance records.
 *
 *  The viewport has negative height (vk.c), so clip +Y is up and uv (0,0) is
 *  the top-left texel: MU_NDC_TO_UV from the mu library is the right constant.
 * ==========================================================================*/
#ifndef TOON_SHARED_SLANG
#define TOON_SHARED_SLANG

#include "../external/mu_lib/mu/shaders/common.slang"
#include "../external/mu_lib/mu/shaders/dispatch.slang"

[[vk::binding(0, 0)]] Texture2D         g_textures[];
[[vk::binding(1, 0)]] SamplerState      g_samplers[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_data;

/* mu_shapes_vertex, byte for byte: 32 bytes, interleaved */
struct Vertex
{
    float3 p;
    float3 n;
    float2 uv;
};

/* material kinds: what the fragment shader paints on top of the base colour */
static const uint KIND_TOON    = 0u; /* plain cel shading                         */
static const uint KIND_TERRAIN = 1u; /* grass on top, strata on the sides         */
static const uint KIND_WATER   = 2u; /* foam cells and a shore ring               */
static const uint KIND_GLOW    = 3u; /* unlit, HDR: feeds the bloom               */
static const uint KIND_FOLIAGE = 4u; /* leaf clumps, wind, two greens             */
static const uint KIND_CLOUD   = 5u; /* soft wrap light, no hard shadow edge      */

/* One drawn object. 128 bytes, written by main.c into the transient arena. */
struct Instance
{
    float4x4 model;
    float4   base;    /* rgb albedo (linear), a = material kind                    */
    float4   shade;   /* rgb what the albedo is multiplied by in shadow, a = hatch */
    float4   surface; /* x specular strength, y specular power, z rim, w emissive  */
    float4   extra;   /* x outline id, y wind amount, z phase, w ink weight        */
};

/* The fullscreen triangle every post pass and the sky draw: three vertices,
 * no fetch at all. */
struct ScreenVaryings
{
    float4 clip : SV_Position;
    float2 uv : TEXCOORD0;
};

ScreenVaryings screen_triangle(uint vertex_id, float depth)
{
    ScreenVaryings o;
    o.uv   = mu_fullscreen_triangle_uv(vertex_id);
    o.clip = float4(mu_fullscreen_triangle_pos(vertex_id).xy, depth, 1.0);
    return o;
}

#endif /* TOON_SHARED_SLANG */
