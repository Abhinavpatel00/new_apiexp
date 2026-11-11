// draw.slang — the particles of snow.slang as lit discs with depth, plus the
// floor, one wall and the ball. No vertex buffer: six vertices each, by index.
import mu_gfx;

struct DrawPush
{
    float4 eye;        // w: tan(fov / 2)
    float4 right;      // w: aspect
    float4 up;         // w: disc radius, cells
    float4 forward;    // w: nodes along a side
    float4 ball;       // centre, radius
    uint   particles, count, view;   // view 0 snow, 1 speed, 2 packed or torn (J_P), 3 squeezed or stretched (det F_E)
    float  walls;
};
[[vk::push_constant]] ConstantBuffer<DrawPush> pc;

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv    : TEXCOORD0;
    [[vk::location(1)]] float4 color : TEXCOORD1;   // a: 0 a round particle, 1 flat, 2 the ball
    [[vk::location(2)]] float3 world : TEXCOORD2;
};

static const float2 CORNER[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
static const float3 LIGHT = float3(0.35, 0.8, 0.48);

float4 project(float3 world)
{
    float3 d   = world - pc.eye.xyz;
    float3 cam = float3(dot(d, pc.right.xyz), dot(d, pc.up.xyz), dot(d, pc.forward.xyz));
    // Near 1 cell, far 400. z / w has to be a + b / distance: that is what stays right when it is
    // interpolated across a large flat surface seen at a slant.
    return float4(cam.x / (pc.eye.w * pc.right.w), cam.y / pc.eye.w, (cam.z - 1.0) * 400.0 / 399.0, cam.z);
}

[shader("vertex")]
Varyings vertDraw(uint vertexIndex : SV_VertexID)
{
    float2 corner = CORNER[vertexIndex % 6u];
    uint   id     = vertexIndex / 6u;
    float  low = pc.walls, high = pc.forward.w - 1.0 - pc.walls;
    Varyings o;
    o.uv = corner;
    if (id < 2u)
    {
        // The floor, and the wall at the high x end.
        float2 t = lerp(float2(low, low), float2(high, high), corner * 0.5 + 0.5);
        o.world    = id == 0u ? float3(t.x, low, t.y) : float3(high, t.x, t.y);
        o.color    = float4(id == 0u ? float3(0.2, 0.22, 0.26) : float3(0.15, 0.165, 0.2), 1.0);
        // The wall is left out when the camera is behind it, so that it never hides the snow on it.
        o.position = id == 1u && pc.eye.x > high ? float4(0, 0, -1, 1) : project(o.world);
        return o;
    }
    if (id == 2u)
    {
        o.world    = pc.ball.xyz + (corner.x * pc.right.xyz + corner.y * pc.up.xyz) * pc.ball.w;
        o.color    = float4(0.5, 0.3, 0.18, 2.0);
        o.position = pc.ball.w > 0.0 ? project(o.world) : float4(0, 0, -1, 1);
        return o;
    }
    uint   at  = pc.particles + (id - 3u) * 80u;
    float4 pos = muScene.Load<float4>(at);
    float4 vel = muScene.Load<float4>(at + 16u);
    float3x3 f = float3x3(muScene.Load<float3>(at + 32u), muScene.Load<float3>(at + 48u), muScene.Load<float3>(at + 64u));
    o.world    = pos.xyz + (corner.x * pc.right.xyz + corner.y * pc.up.xyz) * pc.up.w;
    o.position = project(o.world);

    float3 c = float3(0.93, 0.95, 1.0);
    if (pc.view == 1u) c = lerp(float3(0.05, 0.1, 0.5), float3(1.0, 0.85, 0.3), saturate(length(vel.xyz) / 600.0));
    if (pc.view == 2u) c = vel.w < 1.0 ? lerp(float3(0.9, 0.9, 0.9), float3(0.1, 0.3, 1.0), saturate((1.0 - vel.w) * 4.0))
                                       : lerp(float3(0.9, 0.9, 0.9), float3(1.0, 0.2, 0.1), saturate((vel.w - 1.0) * 1.0));
    if (pc.view == 3u) c = lerp(float3(0.1, 0.4, 1.0), float3(1.0, 0.25, 0.1), saturate(0.5 + 12.0 * (determinant(f) - 1.0)));
    o.color = float4(c, 0.0);
    return o;
}

[shader("fragment")]
float4 fragDraw(Varyings i) : SV_Target0
{
    if (i.color.a > 0.5 && i.color.a < 1.5)
    {
        // Flat: a faint line every 8 cells.
        float3 cell = abs(frac(i.world / 8.0 + 0.5) - 0.5) * 8.0;
        float  line = min(min(cell.x, cell.y), cell.z) < 0.12 ? 0.8 : 1.0;
        return float4(i.color.rgb * line, 1.0);
    }
    float d = dot(i.uv, i.uv);
    if (d > 1.0) discard;
    float3 n = i.uv.x * pc.right.xyz + i.uv.y * pc.up.xyz - sqrt(1.0 - d) * pc.forward.xyz;
    float  light = 0.42 + 0.58 * saturate(dot(n, normalize(LIGHT)));
    return float4(i.color.rgb * light, 1.0);
}
