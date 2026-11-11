// cluster.slang — compCluster: which lights can reach which cluster.
//
// One thread per cluster. It builds the cluster's bounding box in view space
// and tests every light's sphere of influence against it, in light order.
// The test is conservative (a box around a frustum piece), never the reverse:
// a light that is listed and does not reach a pixel costs time; a light that
// reaches a pixel and is not listed is a wrong image.
#include "deferred_common.slang"

[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;

struct ClusterPush { uint frame_offset, pad0, pad1, pad2; };
[[vk::push_constant]] ConstantBuffer<ClusterPush> pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void compCluster(uint3 id : SV_DispatchThreadID)
{
    uint  o    = pc.frame_offset;
    uint4 grid = muSceneRW.Load<uint4>(o + F_GRID);     // nx, ny, nz, capacity
    uint4 data = muSceneRW.Load<uint4>(o + F_DATA);     // lights, light count, clusters
    uint  c    = id.x;
    if (c >= grid.x * grid.y * grid.z) return;
    float4 proj = muSceneRW.Load<float4>(o + F_PROJ);   // tan x, tan y, cluster near, cluster far
    float4 v0 = muSceneRW.Load<float4>(o + F_VIEW), v1 = muSceneRW.Load<float4>(o + F_VIEW + 16u),
           v2 = muSceneRW.Load<float4>(o + F_VIEW + 32u), v3 = muSceneRW.Load<float4>(o + F_VIEW + 48u);

    uint tx = c % grid.x, ty = (c / grid.x) % grid.y, tz = c / (grid.x * grid.y);
    // Depth range of the slice. Slice 0 reaches back to the camera and the
    // last one has no far end, so every visible depth belongs to a slice.
    float d0 = tz == 0u ? 0.0 : sliceDepth(tz, proj.z, proj.w, grid.z);
    float d1 = tz + 1u == grid.z ? 1e5 : sliceDepth(tz + 1u, proj.z, proj.w, grid.z);
    // The tile's edges as slopes x/depth and y/depth.
    float2 s0 = (float2(tx, ty) / float2(grid.xy) * 2.0 - 1.0) * proj.xy;
    float2 s1 = (float2(tx + 1u, ty + 1u) / float2(grid.xy) * 2.0 - 1.0) * proj.xy;
    // A hair of margin: the lighting pass finds a pixel's cluster from a
    // position it rebuilt out of the depth buffer, and that position is only
    // as exact as 32-bit floats. A pixel on a boundary must be covered by the
    // clusters on both sides.
    d0 *= 0.999; d1 *= 1.001;
    s0 -= 1e-3 * proj.xy; s1 += 1e-3 * proj.xy;
    // View space looks down -z. A slope times a depth is widest at one of
    // the slice's two ends, so the box takes the extreme of both.
    float3 lo = float3(min(s0 * d0, s0 * d1), -d1);
    float3 hi = float3(max(s1 * d0, s1 * d1), -d0);

    uint base  = data.z + c * (grid.w + 1u) * 4u;
    uint count = 0u;
    for (uint i = 0u; i < data.y; ++i)
    {
        float4 light = muSceneRW.Load<float4>(data.x + i * LIGHT_SIZE);
        float3 p     = (v0 * light.x + v1 * light.y + v2 * light.z + v3).xyz;
        float3 d     = p - clamp(p, lo, hi);            // to the nearest point of the box
        if (dot(d, d) <= light.w * light.w)
        {
            if (count < grid.w) muSceneRW.Store<uint>(base + 4u + count * 4u, i);
            ++count;                                    // counts past capacity too: overflow is visible
        }
    }
    muSceneRW.Store<uint>(base, count);
}
