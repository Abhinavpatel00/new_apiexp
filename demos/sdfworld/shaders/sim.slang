// sim.slang — things that happen to the world without the CPU.
//
//   compPick    one thread: trace the pointer's ray, store where it hits.
//               The brush pass reads the point from the buffer; the CPU
//               never learns it and never waits for it.
// Both also write the arguments of the brush's dispatch (three group counts,
// or zeros): next frame's Edit pass dispatches indirectly, so a ball that
// hit nothing costs nothing.
//   compBalls   one thread per ball: fall, test the distance field, report
//               an impact. The brush pass turns impacts into craters
//               (stones) or lumps (clay) the same frame.
#include "compute_bindings.slang"
#include "world_common.slang"
#include "volume.slang"

struct SimPush { uint frame_offset, pick_offset, args_offset, pad; uint4 groups; };   // groups: stone, clay, pointer
[[vk::push_constant]] ConstantBuffer<SimPush> pc;

uint balls_max() { return 32u; }   // MAX_BALLS in main.c; the pointer's arguments follow the balls'

Volume loadVolume(uint o)
{
    float4 vol   = muSceneRW.Load<float4>(o + F_VOL);
    uint4  index = muSceneRW.Load<uint4>(o + F_INDEX);
    Volume v;
    v.fine = index.x; v.coarse = index.y; v.linear = index.z; v.pad = 0u;
    v.dims = vol.xyz; v.band = vol.w;
    return v;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void compPick(uint3 id : SV_DispatchThreadID)
{
    uint   o     = pc.frame_offset;
    Volume v     = loadVolume(o);
    float4 mouse = muSceneRW.Load<float4>(o + F_MOUSE);         // ndc x, y; z > 0: pick this frame
    float4 pick  = float4(0.0);
    if (mouse.z > 0.5)
    {
        float3 ro = muSceneRW.Load<float4>(o + F_EYE).xyz;
        float3 rd = normalize(muSceneRW.Load<float4>(o + F_FORWARD).xyz
                            + mouse.x * muSceneRW.Load<float4>(o + F_RIGHT).xyz
                            + mouse.y * muSceneRW.Load<float4>(o + F_UP).xyz);
        float3 face;
        float2 box = volumeBox(v, ro, rd, face);
        if (box.x < box.y)
        {
            VolumeHit h = volumeTrace(v, ro, rd, box.x, box.y, 0.0, true, 192u, 1.0);
            if (h.hit) pick = float4(ro + rd * h.t, 1.0);
        }
    }
    muSceneRW.Store<float4>(pc.pick_offset, pick);
    // The brush's dispatch: its box if there is something to do, else nothing.
    muSceneRW.Store<uint3>(pc.args_offset + balls_max() * 16u, pick.w > 0.5 ? uint3(pc.groups.z) : uint3(0u));
}

float hash1(uint n)
{
    n = (n ^ 61u) ^ (n >> 16u); n *= 9u; n ^= n >> 4u; n *= 0x27d4eb2du; n ^= n >> 15u;
    return (float)(n & 0xffffffu) / 16777216.0;
}

[shader("compute")]
[numthreads(32, 1, 1)]
void compBalls(uint3 id : SV_DispatchThreadID)
{
    uint  o     = pc.frame_offset;
    uint4 balls = muSceneRW.Load<uint4>(o + F_BALLS);           // records, count, impacts, frame number
    if (id.x >= balls.y) return;
    Volume v    = loadVolume(o);
    float  dt   = muSceneRW.Load<float4>(o + F_SIZE).w;
    uint   rec  = balls.x + id.x * BALL_SIZE;
    float4 pr   = muSceneRW.Load<float4>(rec);
    float4 vs   = muSceneRW.Load<float4>(rec + 16u);            // velocity, state: 1 = hit last frame
    float4 impact = float4(0.0);

    if (vs.w > 0.5)
    {
        // It became part of the world last frame. Start again from the sky.
        uint seed = id.x * 7919u + balls.w * 104729u;
        pr.xyz = float3(lerp(8.0, v.dims.x - 8.0, hash1(seed)), v.dims.y - 2.0,
                        lerp(8.0, v.dims.z - 8.0, hash1(seed + 1u)));
        vs = float4((hash1(seed + 2u) - 0.5) * 12.0, -8.0 - 10.0 * hash1(seed + 3u), (hash1(seed + 4u) - 0.5) * 12.0, 0.0);
    }
    else
    {
        vs.y   -= 39.2 * dt;                                    // 9.8 m/s^2 in voxels of 0.25 m
        float3 next = pr.xyz + vs.xyz * dt;
        // The field at the new position, and halfway: a fast ball must not
        // step through a thin wall.
        float d = min(volumeDistance(v, next), volumeDistance(v, 0.5 * (pr.xyz + next)));
        bool  outside = any(next < float3(1.0)) || any(next > v.dims - 1.0);
        if (d < pr.w * 0.6 || outside)
        {
            impact = float4(next, outside ? 0.0 : 1.0);         // leaving the box is not an impact
            vs.w   = 1.0;
        }
        pr.xyz = next;
    }
    muSceneRW.Store<float4>(rec, pr);
    muSceneRW.Store<float4>(rec + 16u, vs);
    muSceneRW.Store<float4>(balls.z + id.x * 16u, impact);
    uint groups = (id.x & 1u) != 0u ? pc.groups.y : pc.groups.x;
    muSceneRW.Store<uint3>(pc.args_offset + id.x * 16u, impact.w > 0.5 ? uint3(groups) : uint3(0u));
}
