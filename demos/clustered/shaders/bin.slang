// bin.slang — compBin: one 32-bit word of one tile's light mask per thread.
//
// Granite bins "32 lights at a time to compute a 32-bit bitmask"; so does
// this. A light may touch a tile if its bounding sphere is not entirely
// outside one of the four planes through the eye and the tile's edges.
// (Conservative: near a tile's corner the sphere can pass all four planes
// and still miss the tile. Such bits cost time, never correctness.)
#include "clustered_common.slang"

[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;

struct BinPush { uint frame_offset, pad0, pad1, pad2; };
[[vk::push_constant]] ConstantBuffer<BinPush> pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void compBin(uint3 id : SV_DispatchThreadID)
{
    uint  o     = pc.frame_offset;
    uint4 grid  = muSceneRW.Load<uint4>(o + F_GRID);    // tiles x, tiles y, words per tile, z bins
    uint4 data  = muSceneRW.Load<uint4>(o + F_DATA);    // lights, light count, tile masks, z bins
    uint4 data2 = muSceneRW.Load<uint4>(o + F_DATA2);   // cull spheres, types
    if (id.x >= grid.x * grid.y * grid.z) return;
    float4 proj = muSceneRW.Load<float4>(o + F_PROJ);   // tan x, tan y
    float4 size = muSceneRW.Load<float4>(o + F_SIZE);   // width, height, tile size in pixels

    uint tile = id.x / grid.z, word = id.x % grid.z;
    uint tx = tile % grid.x, ty = tile / grid.x;
    // The tile's edges as slopes x/depth and y/depth. Pixel rows run down,
    // view-space y runs up.
    float2 p0 = float2(tx, ty) * size.z / size.xy, p1 = float2(tx + 1u, ty + 1u) * size.z / size.xy;
    float2 s0 = float2(p0.x * 2.0 - 1.0, 1.0 - p1.y * 2.0) * proj.xy;
    float2 s1 = float2(p1.x * 2.0 - 1.0, 1.0 - p0.y * 2.0) * proj.xy;
    s0 -= 1e-3 * proj.xy; s1 += 1e-3 * proj.xy;         // a pixel on an edge belongs to both sides
    // Inward unit normals of the four side planes (view space looks down -z;
    // a point is inside the left plane when x >= s0.x * depth = -s0.x * z).
    float3 nl = normalize(float3( 1.0, 0.0,  s0.x)), nr = normalize(float3(-1.0, 0.0, -s1.x));
    float3 nb = normalize(float3(0.0,  1.0,  s0.y)), nt = normalize(float3(0.0, -1.0, -s1.y));

    uint mask  = 0u;
    uint first = word * 32u;
    uint count = min(32u, data.y - min(data.y, first));
    for (uint b = 0u; b < count; ++b)
    {
        float4 s = muSceneRW.Load<float4>(data2.x + (first + b) * 16u);
        bool   in = s.z - s.w < 0.0                      // reaches in front of the eye
                 && dot(nl, s.xyz) >= -s.w && dot(nr, s.xyz) >= -s.w
                 && dot(nb, s.xyz) >= -s.w && dot(nt, s.xyz) >= -s.w;
        if (in) mask |= 1u << b;
    }
    muSceneRW.Store<uint>(data.z + id.x * 4u, mask);
}
