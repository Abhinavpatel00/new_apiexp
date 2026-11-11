// canvas.slang — coloured triangles that a demo's C code wrote this frame.
// canvas.c fills the vertex list; there is nothing to light or look up here.
import mu_gfx;

struct CanvasPush
{
    float4 vp0, vp1, vp2, vp3;
    uint   offset;
};
[[vk::push_constant]] ConstantBuffer<CanvasPush> pc;

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : TEXCOORD0;
};

[shader("vertex")]
Varyings vertCanvas(uint vertexIndex : SV_VertexID)
{
    // 16 bytes a vertex: position, then red, green, blue, alpha as bytes.
    float3 w    = muScene.Load<float3>(pc.offset + vertexIndex * 16u);
    uint   rgba = muScene.Load<uint>(pc.offset + vertexIndex * 16u + 12u);
    float3 c    = float3(float(rgba & 255u), float((rgba >> 8u) & 255u), float((rgba >> 16u) & 255u)) / 255.0;
    Varyings o;
    o.position = pc.vp0 * w.x + pc.vp1 * w.y + pc.vp2 * w.z + pc.vp3;
    // The bytes are what the screen should show; the target is linear and Present encodes it.
    o.color = float4(pow(c, 2.2), 1.0);
    return o;
}

[shader("fragment")]
float4 fragCanvas(Varyings i) : SV_Target0
{
    return i.color;
}
