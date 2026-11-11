// bake.slang — the particles' distance field, rebuilt every frame.
//
// Each voxel looks at the 27 grid cells around it. Every particle centre
// within REACH (one cell) of the voxel is in one of them, so
// min(|x - centre|, REACH) - radius is exact; further away the value is the
// clamp. With look.x > 0 the minimum is a smooth one (order dependent, for
// looks; --check sets it to 0).
[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;
#include "mu_volume.slang"
#include "clay_common.slang"

[[vk::push_constant]] ConstantBuffer<Push> pc;

[shader("compute")]
[numthreads(4, 4, 4)]
void compBake(uint3 id : SV_DispatchThreadID)
{
    float3 x    = ORIGIN + (float3(id) + 0.5) * VOXEL;
    int3   c    = cellOf(x);
    float  best = REACH;
    float  k    = pc.look.x;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int xx = -1; xx <= 1; ++xx)
            {
                int3 n = c + int3(xx, y, z);
                if (any(n < 0) || any(n >= int(CELLS))) continue;
                uint cell  = cellOffset(pc.grid, n);
                uint count = min(muSceneRW.Load<uint>(cell), CELL_CAP);
                for (uint s = 0u; s < count; ++s)
                {
                    uint  j = muSceneRW.Load<uint>(cell + 4u + s * 4u);
                    float d = min(length(x - muSceneRW.Load<float3>(pc.src + j * PREDICTED_SIZE)), REACH);
                    if (k > 0.0)
                    {
                        float h = max(k - abs(best - d), 0.0) / k;
                        best = min(best, d) - h * h * k * 0.25;
                    }
                    else best = min(best, d);
                }
            }
    muStorageVolumes[NonUniformResourceIndex(pc.volume)][id] = float4(best - pc.radius, 0.0, 0.0, 1.0);
}
