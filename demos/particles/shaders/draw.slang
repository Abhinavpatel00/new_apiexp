// draw.slang — vertDraw, fragDraw. See particles_abi.slang.
import mu_gfx;
#include "particles_abi.slang"

[[vk::push_constant]] ConstantBuffer<DrawPush> pc;

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv    : TEXCOORD0;
    [[vk::location(1)]] float4 color : TEXCOORD1;
};

static const float2 CORNER[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};

// Black body-ish ramp over the particle's life: white-blue, yellow, red, out.
float3 ramp(float t)
{
    float3 c = lerp(float3(0.75, 0.85, 1.0), float3(1.0, 0.72, 0.25), smoothstep(0.0, 0.25, t));
    c = lerp(c, float3(0.9, 0.16, 0.05), smoothstep(0.25, 0.7, t));
    return c * (1.0 - smoothstep(0.6, 1.0, t));
}

[shader("vertex")]
Varyings vertDraw(uint vertexIndex : SV_VertexID)
{
    uint   id     = muScene.Load<uint>(pc.compact + (vertexIndex / 6u) * 4u);
    float2 corner = CORNER[vertexIndex % 6u];
    float4 p      = muScene.Load<float4>(pc.particles + id * PARTICLE_SIZE);
    float4 v      = muScene.Load<float4>(pc.particles + id * PARTICLE_SIZE + 16u);
    Varyings o;
    o.uv = corner;
    if (pc.view == 3u)
    {
        // State dump for --check: particle `id` owns pixel (id % w, id / w)
        // and writes its position there. One pixel centre per quad.
        float2 pixel = float2(id % pc.width, id / pc.width) + (corner * 0.5 + 0.5);
        o.position = float4(pixel / float2(pc.width, pc.height) * 2.0 - 1.0, 0.5, 1.0);
        o.color    = float4(p.xyz, 1.0);
        return o;
    }
    float  age   = v.w;
    float  size  = pc.right_size.w * lerp(0.6, 1.6, age);
    float3 world = p.xyz + (pc.right_size.xyz * corner.x + pc.up_gain.xyz * corner.y) * size;
    o.position = pc.vp0 * world.x + pc.vp1 * world.y + pc.vp2 * world.z + pc.vp3;
    float3 c = ramp(age);
    if (pc.view == 1u) c = lerp(float3(0.1, 0.3, 1.0), float3(1.0, 0.3, 0.1), saturate(length(v.xyz) * 0.5));
    if (pc.view == 2u) c = float3(0.25);
    o.color = float4(c * pc.up_gain.w, 1.0);
    return o;
}

[shader("fragment")]
float4 fragDraw(Varyings i) : SV_Target0
{
    if (pc.view == 3u) return i.color;
    float d = saturate(1.0 - dot(i.uv, i.uv));
    return float4(i.color.rgb * d * d, 1.0);
}
