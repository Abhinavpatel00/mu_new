/* tests/smoke.slang — exercises every part of the vk.h shader ABI once. */

struct Vertex   { float3 p; float3 n; float2 uv; };
struct Instance { float4x4 model; float4 color; };

struct Push
{
    float4x4  view_proj;
    Instance *instances;      /* a device address: the transient arena */
    uint      vertex_offset;  /* byte offsets into binding 3: the device arena */
    uint      index_offset;
    uint      texture_id;
    uint      sampler_id;
    uint      storage_id;
    float     time;
};

[[vk::push_constant]] Push push;
[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_samplers[];
[[vk::binding(2, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> g_storage[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_data;

struct VsOut
{
    float4 pos : SV_Position;
    float3 n : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VsOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    uint     index = g_data.Load<uint>(push.index_offset + vid * 4);
    Vertex   v     = g_data.Load<Vertex>(push.vertex_offset + index * 32);
    Instance inst  = push.instances[iid];
    VsOut    o;

    o.pos   = mul(push.view_proj, mul(inst.model, float4(v.p, 1.0)));
    o.n     = mul((float3x3)inst.model, v.n);
    o.uv    = v.uv;
    o.color = inst.color;
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut i) : SV_Target0
{
    float4 t = g_textures[NonUniformResourceIndex(push.texture_id)].Sample(g_samplers[NonUniformResourceIndex(push.sampler_id)], i.uv * 4.0);
    float  l = 0.35 + 0.65 * saturate(dot(normalize(i.n), normalize(float3(0.4, 0.8, 0.5))));
    return float4(t.rgb * i.color.rgb * l, 1.0);
}

/* a second fragment shader behind the same vertex stage: bind_pipe switches to it mid-pass */
[shader("fragment")]
float4 fs_tint(VsOut i) : SV_Target0
{
    return float4(i.color.rgb, 0.5);
}

/* fullscreen triangle, uv (0,0) top-left with the backend's negative-height viewport */
struct FsqOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

[shader("vertex")]
FsqOut vs_fullscreen(uint vid : SV_VertexID)
{
    FsqOut o;
    o.uv  = float2(float((vid << 1) & 2), float(vid & 2));
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_blit(FsqOut i) : SV_Target0
{
    return g_textures[NonUniformResourceIndex(push.texture_id)].Sample(g_samplers[NonUniformResourceIndex(push.sampler_id)], i.uv);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_pattern(uint3 id : SV_DispatchThreadID)
{
    uint2 cell = id.xy / 8;
    float c    = float((cell.x + cell.y) & 1);
    g_storage[NonUniformResourceIndex(push.storage_id)][id.xy] = float4(lerp(float3(0.9, 0.5, 0.2), float3(1.0, 0.95, 0.8), c), 1.0);
}
