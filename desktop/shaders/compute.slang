/* shaders/compute.slang — the two compute passes.
 * ============================================================================
 *    cs_paper   once, before the first frame: bakes the paper texture the
 *               composite reads (line wobble in RG, tooth in B, fibres in A).
 *    cs_petals  every frame: advances the petals in place in the device arena.
 *               The scene pass then draws them from the same bytes.
 * ==========================================================================*/
#include "../external/mu_lib/mu/shaders/common.slang"
#include "../external/mu_lib/mu/shaders/noise.slang"

[[vk::binding(2, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> g_storage[];
[[vk::binding(3, 0)]] RWByteAddressBuffer g_data;

struct ComputePush
{
    float4 wind;      /* xyz drift per second, w time                       */
    float4 volume;    /* xyz half extent of the petal box, w its floor      */
    float4 origin;    /* xyz centre of the box, w seconds since last frame  */
    uint   storage_id;
    uint   petal_offset; /* binding 3: 16 bytes per petal                   */
    uint   petal_count;
    uint   size;         /* paper texture edge in texels                    */
};
[[vk::push_constant]] ComputePush push;

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_paper(uint3 id : SV_DispatchThreadID)
{
    float2 p = (float2(id.xy) + 0.5) / float(push.size);
    /* RG: two smooth, decorrelated fields that push the ink lines around */
    float wobble_x = mu_fbm4(p * 5.0, 2.0, 0.5) * 1.6;
    float wobble_y = mu_fbm4(p * 5.0 + 19.0, 2.0, 0.5) * 1.6;
    /* B: tooth — fine grain riding on a slow blotch, the way wash settles */
    float tooth    = mu_value_noise2(p * 180.0) * 0.55 + mu_fbm4(p * 9.0 + 5.0, 2.0, 0.5) * 0.45;
    /* A: long fibres, stretched one way */
    float fibre    = mu_value_noise2(p * float2(40.0, 320.0));

    /* mu noise is signed and a UNORM texel is not: 0.5 is "no change" */
    g_storage[NonUniformResourceIndex(push.storage_id)][id.xy] = mu_saturate(float4(wobble_x, wobble_y, tooth, fibre) * 0.5 + 0.5);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_petals(uint3 id : SV_DispatchThreadID)
{
    uint   offset = push.petal_offset + min(id.x, push.petal_count - 1u) * 16u;
    float4 petal  = g_data.Load<float4>(offset);
    float  seed   = petal.w;
    float  time   = push.wind.w;
    float  dt     = push.origin.w;
    /* a slow curl field makes them wander instead of falling in lanes */
    float2 curl   = mu_curl_noise2(petal.xz * 0.35 + time * 0.05, 0.05);
    float3 p      = petal.xyz;

    p   += (push.wind.xyz + float3(curl.x, 0.0, curl.y) * 0.9) * dt;
    p.y -= (0.55 + seed * 0.5) * dt;
    p.x += sin(time * (1.1 + seed) + seed * 31.0) * 0.35 * dt;

    /* out of the box on any side: back in at the top, somewhere new */
    float3 rel     = p - push.origin.xyz;
    bool   outside = p.y < push.volume.w || abs(rel.x) > push.volume.x || abs(rel.z) > push.volume.z;
    float3 fresh   = mu_hash03(uint3(id.x, uint(time * 60.0), 977u));
    float3 respawn = push.origin.xyz + float3((fresh.x * 2.0 - 1.0) * push.volume.x, push.volume.y * (0.6 + 0.4 * fresh.y), (fresh.z * 2.0 - 1.0) * push.volume.z);

    g_data.Store<float4>(offset, float4(select(outside, respawn, p), seed));
}
