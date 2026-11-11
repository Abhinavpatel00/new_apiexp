// coarse.slang — the empty-space level (Claybook's dilated mips, reduced to
// one level and one number).
//
//   compOccupied   per cell of 4^3 voxels: 0 if any voxel is closer than
//                  BAND to a surface or inside one, else REACH + 1
//   compReach      three times, along x, y, z: the Chebyshev distance, in
//                  cells, to the nearest occupied cell, up to REACH + 1
//
// Chebyshev, not Euclidean, on purpose. It never exceeds the true distance
// between the cells, so a ray may jump (D - 1) cells' width in any
// direction; and it separates:
//   min over cells of max(|dx|, |dy|, |dz|)
//     = min over dz of max(|dz|, min over dy of max(|dy|, min over dx |dx|))
// so three passes of 2 * REACH + 1 reads replace one of (2 * REACH + 1)^3.
#include "compute_bindings.slang"

static const int REACH = 8;

struct CoarsePush
{
    uint  source, target;      // bindless: volume read (sampled), volume written (storage)
    uint  pad_a, pad_b;
    uint4 dims;                // VX, VY, VZ, voxels per cell
    float band; uint axis; float pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<CoarsePush> pc;

[shader("compute")]
[numthreads(4, 4, 4)]
void compOccupied(uint3 id : SV_DispatchThreadID)
{
    uint3 cdims = pc.dims.xyz / pc.dims.w;
    if (any(id >= cdims)) return;
    float nearest = pc.band;
    for (uint z = 0u; z < pc.dims.w; ++z)
        for (uint y = 0u; y < pc.dims.w; ++y)
        {
            for (uint x = 0u; x < pc.dims.w; ++x)
                nearest = min(nearest, muSampledVolumes[NonUniformResourceIndex(pc.source)].Load(int4(int3(id * pc.dims.w + uint3(x, y, z)), 0)).r);
        }
    muStorageVolumes[NonUniformResourceIndex(pc.target)][id] =
        float4(nearest < pc.band * 0.98 ? 0.0 : (float)(REACH + 1), 0.0, 0.0, 0.0);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void compReach(uint3 id : SV_DispatchThreadID)
{
    int3 cdims = (int3)(pc.dims.xyz / pc.dims.w);
    if (any((int3)id >= cdims)) return;
    int3 step = int3(pc.axis == 0u ? 1 : 0, pc.axis == 1u ? 1 : 0, pc.axis == 2u ? 1 : 0);
    int  best = REACH + 1;
    for (int o = -REACH; o <= REACH; ++o)
    {
        int3 c = (int3)id + step * o;
        if (any(c < 0) || any(c >= cdims)) continue;
        int along = (int)muSampledVolumes[NonUniformResourceIndex(pc.source)].Load(int4(c, 0)).r;
        best = min(best, max(along, abs(o)));
    }
    muStorageVolumes[NonUniformResourceIndex(pc.target)][id] = float4((float)best, 0.0, 0.0, 0.0);
}
