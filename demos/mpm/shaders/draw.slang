// draw.slang — the particles of mpm.slang as discs, and the four walls.
// No vertex buffer: six vertices per particle, read by index.
import mu_gfx;

struct DrawPush
{
    uint  particles, count, n, view;      // view 0 material, 1 speed, 2 squeeze (J)
    float aspect, size, walls, pad;       // size: disc radius in cells
};
[[vk::push_constant]] ConstantBuffer<DrawPush> pc;

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv    : TEXCOORD0;
    [[vk::location(1)]] float4 color : TEXCOORD1;   // a = 1 for a wall
};

static const float2 CORNER[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};

// Cells to clip space: the grid fills the picture's height. The framework's
// viewport has +y up, as the grid does.
float4 place(float2 cells)
{
    float2 unit = cells / (float)(pc.n - 1u);   // nodes 0 .. n - 1 span the picture
    return float4((unit.x - 0.5) * 2.0 / pc.aspect, 2.0 * unit.y - 1.0, 0.5, 1.0);
}

[shader("vertex")]
Varyings vertDraw(uint vertexIndex : SV_VertexID)
{
    float2 corner = CORNER[vertexIndex % 6u];
    uint   id     = vertexIndex / 6u;
    Varyings o;
    o.uv = corner;
    if (id < 4u)
    {
        // The walls: left, right, floor, ceiling.
        float  n = (float)(pc.n - 1u);
        float2 lo = id == 1u ? float2(n - pc.walls, 0.0) : id == 3u ? float2(0.0, n - pc.walls) : float2(0.0, 0.0);
        float2 hi = id == 0u ? float2(pc.walls, n) : id == 2u ? float2(n, pc.walls) : float2(n, n);
        o.position = place(lerp(lo, hi, corner * 0.5 + 0.5));
        o.color    = float4(0.16, 0.17, 0.2, 1.0);
        return o;
    }
    uint   at  = pc.particles + (id - 4u) * 64u;
    float2 pos = muScene.Load<float2>(at);
    float2 vel = muScene.Load<float2>(at + 8u);
    float4 f   = muScene.Load<float4>(at + 32u);
    uint   material = muScene.Load<uint>(at + 48u);
    o.position = place(pos + corner * pc.size);

    float  speed = saturate(length(vel) / 400.0);
    float3 c = material == 0u ? lerp(float3(0.05, 0.32, 0.85), float3(0.75, 0.93, 1.0), speed * speed)
             : material == 1u ? float3(1.0, 0.55, 0.12) : float3(0.95, 0.2, 0.45);
    if (pc.view == 1u) c = lerp(float3(0.05, 0.1, 0.5), float3(1.0, 0.85, 0.3), speed);
    if (pc.view == 2u) c = lerp(float3(0.1, 0.4, 1.0), float3(1.0, 0.25, 0.1), saturate(0.5 + 10.0 * (f.x * f.w - f.y * f.z - 1.0)));
    o.color = float4(c, 0.0);
    return o;
}

[shader("fragment")]
float4 fragDraw(Varyings i) : SV_Target0
{
    if (i.color.a > 0.5) return float4(i.color.rgb, 1.0);
    float d = dot(i.uv, i.uv);
    if (d > 1.0) discard;
    return float4(i.color.rgb * (1.0 - 0.45 * d), 1.0);
}
