// bake.slang — samples the scene's distance function into the volume the
// forward pass reads shadows from. Runs once: the scene does not move.
import mu_gfx;
#include "clustered_common.slang"
#include "sdf_shadow.slang"

struct Push { uint frame_offset, pad0, pad1, pad2; };
[[vk::push_constant]] ConstantBuffer<Push> pc;

[shader("compute")]
[numthreads(4, 4, 4)]
void compBake(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= SDF_DIMS)) return;
    SdfInfo s = sdfInfo(pc.frame_offset);
    float   d = sceneDistance(s, s.origin + (float3(id) + 0.5) * s.voxel);
    // Steps are at most 1 m, so nothing further than that needs to be exact.
    muStorageVolumes[NonUniformResourceIndex(s.volume)][id] = float4(min(d, 4.0), 0.0, 0.0, 0.0);
}
