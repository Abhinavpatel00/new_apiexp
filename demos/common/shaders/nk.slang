// nk.slang — Nuklear overlay (shared by every demo; was duplicated verbatim as
// demos/galaxy/shaders/nk.{vert,frag} and demos/toon/shaders/nk.{vert,frag}).
//
// Vertices are fetched from the storage buffer by vertex/instance ID, the
// vk.h §17 pattern: no vertex-input state. One instance is one triangle, so a
// draw is `pass_draw(3, triangles, 0, 0)`.
//
//   NkVkVertex (C, 32 B): float pos[2]; float uv[2]; float col[4];
//   NkVkPush   (C, 48 B): uint vert_offset, texture_index, sampler_index;
//                         float width, height, pad[3]; float clip[4];
//
// vk.h has no scissor state, so Nuklear's clip rectangle arrives in the push
// block and fragments outside it are discarded.
import mu_gfx;

struct NkVertex
{
    float2 pos;
    float2 uv;
    float4 col;
};

struct NkPush
{
    uint  vert_offset;   // byte offset of NkVertex[0] in the storage buffer
    uint  texture_index; // font atlas
    uint  sampler_index;
    float width;         // framebuffer size in pixels
    float height;
    float pad0, pad1, pad2;
    float4 clip;         // x0, y0, x1, y1 in framebuffer pixels
};
[[vk::push_constant]] ConstantBuffer<NkPush> pc;

struct NkVaryings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv  : TEXCOORD0;
    [[vk::location(1)]] float4 col : COLOR0;
};

[shader("vertex")]
NkVaryings vertNk(uint vertexIndex : SV_VertexID, uint instanceIndex : SV_InstanceID)
{
    uint     index = instanceIndex * 3u + vertexIndex;
    NkVertex v     = muScene.Load<NkVertex>(pc.vert_offset + index * 32u);

    NkVaryings o;
    o.position = float4((v.pos.x / pc.width) * 2.0 - 1.0,
                        1.0 - (v.pos.y / pc.height) * 2.0, 0.0, 1.0);
    o.uv  = v.uv;
    o.col = v.col;
    return o;
}

// Font atlas * vertex colour. Solid shapes sample the atlas' white pixel.
[shader("fragment")]
float4 fragNk(NkVaryings i) : SV_Target0
{
    float2 px = i.position.xy;
    if (px.x < pc.clip.x || px.y < pc.clip.y || px.x >= pc.clip.z || px.y >= pc.clip.w)
        discard;
    return muSample(pc.texture_index, pc.sampler_index, i.uv) * i.col;
}
