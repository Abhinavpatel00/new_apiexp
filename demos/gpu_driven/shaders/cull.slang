// cull.slang — which boxes to draw, decided on the GPU.
//
// Two phases per frame (Haar & Aaltonen, "GPU-Driven Rendering Pipelines",
// SIGGRAPH 2015):
//   A  draw what is in the frustum and was visible last frame
//   -- build a depth pyramid from what A drew
//   B  test every box in the frustum against that pyramid; draw the ones
//      that pass and were not drawn in A; remember the outcome for next frame
// Nothing that would show is ever left out: a box is skipped only when what
// has ALREADY been drawn this frame hides all of it. Last frame's answer only
// decides in which phase a box is drawn.
//
// mode 0: as above.  1: frustum only (all in A).  2: no culling (all in A).
import mu_gfx;
#include "gpu_common.slang"

[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;

uint pyramidLevel(uint level)
{
    return level < 4u ? pc.pyramid0[level] : (level < 8u ? pc.pyramid1[level - 4u] : pc.pyramid2[level - 8u]);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void compReset(uint3 id : SV_DispatchThreadID)
{
    // pc.cmd: phase A's command; pc.list: phase B's command (reused field).
    muSceneRW.Store<uint4>(pc.cmd, uint4(36u, 0u, 0u, 0u));
    muSceneRW.Store<uint4>(pc.list, uint4(36u, 0u, 0u, 0u));
}

struct Bounds
{
    bool   in_frustum;
    bool   crosses_near;   // some corner is at or behind the eye plane: no screen rectangle
    float2 lo, hi;         // NDC rectangle
    float  nearest;        // smallest depth of the eight corners
};

Bounds project(float3 centre, float3 half)
{
    Bounds b;
    b.lo = float2(1e9); b.hi = float2(-1e9);
    b.nearest      = 1e9;
    b.crosses_near = false;
    uint outside = 63u;                     // one bit per plane; stays set if ALL corners are outside it
    for (uint k = 0u; k < 8u; ++k)
    {
        float3 corner = centre + half * float3((k & 1u) != 0u ? 1.0 : -1.0, (k & 2u) != 0u ? 1.0 : -1.0,
                                               (k & 4u) != 0u ? 1.0 : -1.0);
        float4 c = toClip(corner);
        uint   m = 0u;
        if (c.x < -c.w) m |= 1u;
        if (c.x > c.w) m |= 2u;
        if (c.y < -c.w) m |= 4u;
        if (c.y > c.w) m |= 8u;
        if (c.z < 0.0) m |= 16u;
        if (c.z > c.w) m |= 32u;
        outside &= m;
        if (c.w <= 1e-4) { b.crosses_near = true; continue; }
        float3 ndc = c.xyz / c.w;
        b.lo       = min(b.lo, ndc.xy);
        b.hi       = max(b.hi, ndc.xy);
        b.nearest  = min(b.nearest, ndc.z);
    }
    b.in_frustum = outside == 0u;
    return b;
}

// True when everything already drawn inside the box's screen rectangle is
// closer than the box's nearest point.
bool occluded(Bounds b)
{
    if (b.crosses_near) return false;
    float2 size = float2((float)pc.width, (float)pc.height);
    // NDC y points up, pixel rows count down (vk.c flips the viewport).
    float2 top_left = float2(b.lo.x, -b.hi.y), bottom_right = float2(b.hi.x, -b.lo.y);
    int2   lo   = clamp((int2)floor((top_left * 0.5 + 0.5) * size), int2(0), (int2)size - 1);
    int2   hi   = clamp((int2)floor((bottom_right * 0.5 + 0.5) * size), int2(0), (int2)size - 1);
    // The first level whose texels are big enough that the rectangle touches
    // at most 2 x 2 of them. Level L texel i covers pixels from i << (L+1).
    uint level = 0u;
    while (level + 1u < pc.levels && (((hi.x >> (level + 1u)) - (lo.x >> (level + 1u))) > 1 ||
                                      ((hi.y >> (level + 1u)) - (lo.y >> (level + 1u))) > 1))
        ++level;
    uint image = pyramidLevel(level);
    uint w, h;
    muSampledImages[NonUniformResourceIndex(image)].GetDimensions(w, h);
    int2 last = int2((int)w, (int)h) - 1;
    int2 t0   = min(lo >> (level + 1u), last), t1 = min(hi >> (level + 1u), last);
    // On the last level the rectangle may still span more than 2 x 2: then
    // that level is 2 x 1 or 1 x 1 and these four loads cover all of it.
    float farthest = max(max(muSampledImages[NonUniformResourceIndex(image)].Load(int3(t0.x, t0.y, 0)).r,
                             muSampledImages[NonUniformResourceIndex(image)].Load(int3(t1.x, t0.y, 0)).r),
                         max(muSampledImages[NonUniformResourceIndex(image)].Load(int3(t0.x, t1.y, 0)).r,
                             muSampledImages[NonUniformResourceIndex(image)].Load(int3(t1.x, t1.y, 0)).r));
    return b.nearest > farthest + pc.light.w;
}

void append(uint i)
{
    uint slot;
    muSceneRW.InterlockedAdd(pc.cmd + 4u, 1u, slot);
    muSceneRW.Store<uint>(pc.list + slot * 4u, i);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compCull(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= pc.count) return;
    if (pc.mode == 2u)
    {
        if (pc.phase == 0u) append(i);
        return;
    }
    Bounds b = project(muSceneRW.Load<float4>(pc.instances + i * INSTANCE_SIZE).xyz,
                       muSceneRW.Load<float4>(pc.instances + i * INSTANCE_SIZE + 16u).xyz);
    if (pc.mode == 1u)
    {
        if (pc.phase == 0u && b.in_frustum) append(i);
        return;
    }
    uint was = muSceneRW.Load<uint>(pc.flags + i * 4u);
    if (pc.phase == 0u)
    {
        if (b.in_frustum && was != 0u) append(i);
        return;
    }
    bool now = b.in_frustum && !occluded(b);
    if (now && was == 0u) append(i);
    muSceneRW.Store<uint>(pc.flags + i * 4u, now ? 1u : 0u);
}
