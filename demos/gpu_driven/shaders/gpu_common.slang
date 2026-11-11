// gpu_common.slang — what the passes of gpu_driven share. Include only.
//
// An instance is an axis-aligned box: 48 bytes in the storage buffer,
//   float4 centre, float4 half extents, float4 colour (w unused in all three).
// A draw list is a VkDrawIndirectCommand (vertexCount, instanceCount,
// firstVertex, firstInstance) and, elsewhere, one uint per listed instance.
// The command's instanceCount is the list's length: culling adds to it with
// an atomic add and writes the instance's index into the slot it got.
static const uint INSTANCE_SIZE = 48u;

struct Push
{
    float4 vp0, vp1, vp2, vp3;             // view-projection, columns
    uint   instances, flags, cmd, list;    // byte offsets; cmd and list are this phase's
    uint   count, mode, phase, view;
    uint   vertices, levels, width, height;
    float4 light;                          // xyz: towards the light; w: depth bias of the occlusion test
    uint4  pyramid0, pyramid1, pyramid2;   // bindless indices of the depth pyramid's levels
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

float4 toClip(float3 p) { return pc.vp0 * p.x + pc.vp1 * p.y + pc.vp2 * p.z + pc.vp3; }
