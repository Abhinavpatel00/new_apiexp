// build.slang — building the sparse volume on the GPU, once.
//
//   compClassify   one thread per brick: the distance at its centre
//   compAllocate   ONE thread: walks the bricks in order, hands out atlas
//                  slots, writes the indirection volume, the list of stored
//                  bricks and the size of the next dispatch. Serial on
//                  purpose: simple, and the same layout every run. A world
//                  that changes every frame would need a parallel prefix sum.
//   compBricks     indirect dispatch, 8 groups of 4^3 per stored brick
//   compDense      the whole volume densely, for comparison (--dense)
[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;
#include "mu_volume.slang"
#include "bricks_common.slang"

[[vk::push_constant]] ConstantBuffer<Push> pc;

[shader("compute")]
[numthreads(4, 4, 4)]
void compClassify(uint3 id : SV_DispatchThreadID)
{
    uint B = pc.bricks;
    if (any(id >= B)) return;
    float3 centre = (float3(id) * 7.0 + 3.5) * pc.world.x;
    muSceneRW.Store<float>(pc.flags + (id.x + B * (id.y + B * id.z)) * 4u, scene(centre));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void compAllocate(uint3 id : SV_DispatchThreadID)
{
    uint  B = pc.bricks, A = pc.atlas_side;
    uint  count = 0u, wanted = 0u;
    float reach = HALF_DIAGONAL * pc.world.x;
    for (uint b = 0u; b < B * B * B; ++b)
    {
        float d     = muSceneRW.Load<float>(pc.flags + b * 4u);
        uint3 brick = uint3(b % B, b / B % B, b / (B * B));
        float4 entry;
        if (abs(d) < reach + MARGIN * pc.world.x)
        {
            ++wanted;
            if (count < pc.capacity)
            {
                entry = float4(float3(count % A, count / A % A, count / (A * A)), 0.0);
                muSceneRW.Store<uint>(pc.list + count * 4u, b);
                ++count;
            }
            else entry = float4(0.0, 0.0, 0.0, 1e-6);   // no room: a tiny step, so the hole shows as slowness and --check sees it
        }
        else entry = float4(0.0, 0.0, 0.0, sign(d) * (abs(d) - reach));
        muStorageVolumes[NonUniformResourceIndex(pc.indirection)][brick] = entry;
    }
    muSceneRW.Store<uint3>(pc.args, uint3(64u, (count + 7u) / 8u, 1u));
    muSceneRW.Store<uint>(pc.args + 12u, count);
    muSceneRW.Store<uint>(pc.args + 16u, wanted);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void compBricks(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    uint g    = gid.x + 64u * gid.y;
    uint slot = g / 8u, octant = g % 8u;
    if (slot >= muSceneRW.Load<uint>(pc.args + 12u)) return;
    uint  B = pc.bricks, A = pc.atlas_side;
    uint  b = muSceneRW.Load<uint>(pc.list + slot * 4u);
    uint3 brick = uint3(b % B, b / B % B, b / (B * B));
    uint3 t     = lid + 4u * uint3(octant & 1u, (octant >> 1u) & 1u, octant >> 2u);
    uint3 where = uint3(slot % A, slot / A % A, slot / (A * A)) * 8u + t;
    muStorageVolumes[NonUniformResourceIndex(pc.atlas)][where] = texelValue(int3(brick * 7u + t), pc.world.x);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void compDense(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= pc.bricks * 7u + 1u)) return;
    muStorageVolumes[NonUniformResourceIndex(pc.dense)][id] = texelValue(int3(id), pc.world.x);
}
