// bake.slang — the distance of pat_common.slang sampled into a 64^3 volume.
//
// The picture is still drawn from pointwise queries: draw.slang walks a ray
// through this volume only to find where to start asking.
[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;
#define PAT_SCENE muSceneRW
#include "mu_volume.slang"
#include "pat_common.slang"

struct BakePush
{
    uint  header, combine, method, volume;
    float half_size;     // the volume spans [-half, half]^3
    uint  voxels;
};
[[vk::push_constant]] ConstantBuffer<BakePush> pc;

[shader("compute")]
[numthreads(4, 4, 4)]
void compBake(uint3 id : SV_DispatchThreadID)
{
    float3 x = (float3(id) + 0.5) / float(pc.voxels) * 2.0 * pc.half_size - pc.half_size;
    float3 gradient;
    float  d = patScene(pc.header, pc.combine, x, pc.method, gradient);
    muStorageVolumes[NonUniformResourceIndex(pc.volume)][id] = float4(d, 0.0, 0.0, 1.0);
}
