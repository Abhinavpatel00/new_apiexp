// One fullscreen triangle, no vertex buffer, no index buffer.
// (0,0),(2,0),(0,2) -> clip (-1,1),(3,1),(-1,-3): the v2 convention.
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VSOut vs_main(uint vid : SV_VertexID)
{
    float2 p    = float2(float((vid << 1) & 2u), float(vid & 2u));
    VSOut  o;
    o.pos = float4(p.x * 2.0f - 1.0f, 1.0f - p.y * 2.0f, 0.0f, 1.0f);
    o.uv  = p * 0.5f;
    return o;
}

[shader("fragment")]
float4 fs_main(VSOut i) : SV_Target
{
    return float4(i.uv, 0.25f, 1.0f);
}
