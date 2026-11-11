// draw.slang — draws the boxes a draw list names. The instance count comes
// from the indirect command; SV_InstanceID picks an entry of the list.
import mu_gfx;
import mu_mesh;
#include "gpu_common.slang"

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal : NORMAL;
    [[vk::location(1)]] float3 albedo : COLOR0;
    [[vk::location(2)]] float  distance : TEXCOORD0;
    [[vk::location(3)]] nointerpolation uint id : TEXCOORD1;
};

[shader("vertex")]
Varyings vertDraw(uint vertexIndex : SV_VertexID, uint instance : SV_InstanceID)
{
    MeshVertex v    = muLoadMeshVertex(pc.vertices, vertexIndex);
    uint       id   = muScene.Load<uint>(pc.list + instance * 4u);
    uint       base = pc.instances + id * INSTANCE_SIZE;
    float3 world    = muScene.Load<float4>(base).xyz + v.position * muScene.Load<float4>(base + 16u).xyz;
    Varyings r;
    r.position = toClip(world);
    r.normal   = v.normal;
    r.albedo   = muScene.Load<float4>(base + 32u).rgb;
    r.distance = r.position.w;
    r.id       = id;
    return r;
}

[shader("fragment")]
float4 fragDraw(Varyings i) : SV_Target0
{
    // View 1: the instance, exactly (two numbers below 1024 fit a half float).
    if (pc.view == 1u) return float4((float)(i.id & 1023u), (float)(i.id >> 10u), 0.0, 1.0);
    float3 n     = normalize(i.normal);
    float  light = 0.18 + 0.95 * saturate(dot(n, pc.light.xyz)) + 0.10 * n.y;
    float3 color = i.albedo * light;
    // View 2: what phase B added this frame, i.e. what last frame did not know was visible.
    if (pc.view == 2u) color = pc.phase == 1u ? float3(1.0, 0.15, 0.05) * light : float3(light * 0.35);
    float fog = 1.0 - exp(-i.distance * 0.0022);
    return float4(lerp(color, float3(0.30, 0.45, 0.75), fog), 1.0);
}
