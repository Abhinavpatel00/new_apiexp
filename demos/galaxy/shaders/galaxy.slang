// galaxy.slang — analytic star/dust/core billboards.
//
// No vertex data at all: every star's position, colour and energy is derived
// from SV_InstanceID with an integer hash, and SV_VertexID picks the corner of
// a two-triangle billboard. One pass_draw(6, count, 0, 0) per layer.
//
// Push (176 B) must match GalaxyPush in demos/galaxy/main.c.

struct GalaxyPush
{
    column_major float4x4 view_proj;
    float4 camera_right;
    float4 camera_up;
    float4 params;    // time, radius, thickness, arm twist
    float4 tint_core;
    float4 tint_arm;
    float4 params2;   // star size, brightness, dust size, core size
    uint4  meta;      // layer, seed, reserved, reserved
};
[[vk::push_constant]] ConstantBuffer<GalaxyPush> pc;

struct GalaxyVaryings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 local  : TEXCOORD0;
    [[vk::location(1)]] float3 color  : COLOR0;
    [[vk::location(2)]] float  energy : TEXCOORD1;
    [[vk::location(3)]] float  sharp  : TEXCOORD2;
};

uint hashU32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hashF(uint x, uint salt)
{
    return float(hashU32(x ^ salt) >> 8) * (1.0 / 16777216.0);
}

static const float2 kCorners[6] = {
    float2(-1, -1), float2( 1, -1), float2( 1,  1),
    float2(-1, -1), float2( 1,  1), float2(-1,  1),
};

static const float TAU = 6.2831853;

[shader("vertex")]
GalaxyVaryings vertMain(uint vertexIndex : SV_VertexID, uint instanceIndex : SV_InstanceID)
{
    float2 corner = kCorners[vertexIndex % 6u];
    uint   layer  = pc.meta.x;
    uint   seed   = instanceIndex * 2654435761u + pc.meta.y * 1013904223u;
    float  t      = pc.params.x;

    float3 center;
    float3 color;
    float  energy;
    float  size;
    float  sharp;

    if (layer == 2u)
    {
        center = float3(0.0, 0.0, 0.0);
        size   = pc.params2.w;
        color  = pc.tint_core.rgb;
        energy = 0.55;
        sharp  = 1.6;
    }
    else
    {
        float radius = pc.params.y;
        const float arms = 4.0;
        float u = hashF(seed, 0xA1B2C3D4u);
        float v = hashF(seed, 0x13579BDFu);
        float w = hashF(seed, 0x2468ACE0u);
        float r = radius * pow(u, 0.62);
        float arm    = floor(hashF(seed, 0x0F1E2D3Cu) * arms);
        float knorm  = r / radius;
        float spread = layer == 0u ? (0.10 + 0.28 * knorm) : (0.9 + 1.2 * knorm);
        float theta  = knorm * pc.params.w * 2.5 + arm * TAU / arms + (v - 0.5) * spread;
        float omega  = 2.2 / pow(1.0 + r * 0.42, 0.95);
        theta += t * omega;

        float thick = pc.params.z * (1.0 - 0.7 * saturate(knorm));
        float y     = (w - 0.5) * 2.0 * thick;
        y += 0.10 * sin(theta * 3.0 + w * TAU) * thick;
        center = float3(cos(theta) * r, y, sin(theta) * r);

        float k = saturate(knorm);
        color = lerp(pc.tint_core.rgb, pc.tint_arm.rgb, pow(k, 0.75));
        color *= 0.70 + 0.60 * hashF(seed, 0x5A5A5A5Au);
        color = lerp(color, float3(1.0, 1.0, 1.0), pow(hashF(seed, 0x99AA33CCu), 12.0) * 0.8);

        if (layer == 0u)
        {
            size   = pc.params2.x * (0.55 + 1.7 * hashF(seed, 0x3C3C3C3Cu));
            energy = pc.params2.y * (0.05 + 1.10 * pow(hashF(seed, 0x77112233u), 8.0));
            energy *= 0.78 + 0.22 * sin(t * (2.0 + 4.0 * w) + u * TAU);
            sharp  = 4.2;
        }
        else
        {
            size   = pc.params2.z * (0.6 + 0.9 * v);
            energy = 0.010 + 0.022 * pow(hashF(seed, 0x5511AA33u), 2.0);
            color  = lerp(float3(0.42, 0.16, 0.62), float3(0.10, 0.34, 0.55), v);
            sharp  = 1.5;
        }
    }

    float3 world = center + (pc.camera_right.xyz * corner.x + pc.camera_up.xyz * corner.y) * size;

    GalaxyVaryings o;
    o.position = mul(pc.view_proj, float4(world, 1.0));
    o.local    = corner;
    o.color    = color;
    o.energy   = energy;
    o.sharp    = sharp;
    return o;
}

[shader("fragment")]
float4 fragMain(GalaxyVaryings i) : SV_Target0
{
    float d2 = dot(i.local, i.local);
    if (d2 > 1.0)
        discard;
    float fall = exp(-d2 * i.sharp) * (1.0 - d2);
    fall *= 1.0 + 2.0 * exp(-d2 * 24.0);
    return float4(i.color * (i.energy * fall), 1.0);
}
