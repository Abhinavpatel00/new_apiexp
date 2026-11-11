// draw.slang — sphere tracing the sparse or the dense volume.
import mu_gfx;
#include "mu_volume.slang"
#include "bricks_common.slang"

[[vk::push_constant]] ConstantBuffer<Push> pc;

// p in metres, inside the world. Distance in .x, normal in .yzw (zero in empty bricks).
float4 sampleSparse(float3 p, out int3 brick, out bool stored)
{
    float3 s = p / pc.world.x;
    brick = clamp(int3(floor(s / 7.0)), int3(0), int3(pc.bricks) - 1);
    float4 e = muSampledVolumes[NonUniformResourceIndex(pc.indirection)].Load(int4(brick, 0));
    stored = e.w == 0.0;
    if (!stored) return float4(e.w, 0.0, 0.0, 0.0);
    float3 uvw = (e.xyz * 8.0 + 0.5 + (s - 7.0 * float3(brick))) / (8.0 * float(pc.atlas_side));
    return muSampledVolumes[NonUniformResourceIndex(pc.atlas)]
               .SampleLevel(muSamplers[NonUniformResourceIndex(pc.sampler)], uvw, 0.0);
}

float4 sampleDense(float3 p)
{
    float3 uvw = (p / pc.world.x + 0.5) / float(pc.bricks * 7u + 1u);
    return muSampledVolumes[NonUniformResourceIndex(pc.dense)]
               .SampleLevel(muSamplers[NonUniformResourceIndex(pc.sampler)], uvw, 0.0);
}

[shader("fragment")]
float4 fragDraw(FullscreenVaryings i) : SV_Target0
{
    float2 ndc = float2(i.position.x / pc.size.x * 2.0 - 1.0, 1.0 - i.position.y / pc.size.y * 2.0);
    float3 rd  = normalize(pc.forward.xyz + ndc.x * pc.right.xyz + ndc.y * pc.up.xyz);
    float3 ro  = pc.eye.xyz;

    // Into the world's box, if the ray meets it.
    float3 inv = 1.0 / rd;
    float3 t0  = (0.0 - ro) * inv, t1 = (WORLD - ro) * inv;
    float3 lo  = min(t0, t1), hi = max(t0, t1);
    float  t   = max(max(lo.x, max(lo.y, lo.z)), 0.0);
    float  end = min(hi.x, min(hi.y, hi.z));

    float  voxel = pc.world.x;
    uint   steps = 0u, fetches = 0u;
    bool   hit = false, stored = true;
    int3   brick = int3(0);
    float4 v = float4(0.0);
    for (; steps < 400u && t < end; ++steps)
    {
        float3 p = ro + rd * t;
        if (pc.mode == 1u) { v = sampleDense(p); fetches += 1u; }
        else { v = sampleSparse(p, brick, stored); fetches += stored ? 2u : 1u; }
        if (v.x < 0.25 * voxel + 0.0005 * t) { hit = true; break; }
        t += max(v.x, 0.05 * voxel);
    }
    if (pc.view == 1u)                                    // steps, blue 0 .. red 100
    {
        float h = saturate(float(steps) / 100.0);
        return float4(h, 0.2, 1.0 - h, 1.0);
    }
    if (pc.view == 4u) return float4(float(steps), float(fetches), hit ? t : -1.0, 1.0);   // --check
    if (!hit) return float4(0.30, 0.42, 0.60, 0.0);
    float3 p = ro + rd * t;
    float3 n;
    if (steps == 0u)
    {
        // The ray starts in the solid: the box cuts through the ground here. Deep in
        // the ground the bricks are empty and carry no normal (--check found the two
        // volumes disagreeing on exactly this face), so the cut is shaded as the box's
        // face in both modes.
        n = t <= 0.0 ? -rd : -sign(rd) * float3(lo.x >= t ? 1.0 : 0.0, lo.x < t && lo.y >= t ? 1.0 : 0.0, lo.x < t && lo.y < t ? 1.0 : 0.0);
    }
    else n = normalize(v.yzw);
    if (pc.view == 2u) return float4(n * 0.5 + 0.5, 1.0);
    float3 albedo = float3(0.72, 0.70, 0.66);
    if (pc.view == 3u && pc.mode == 0u)                   // bricks: a checker of 7-voxel cubes
        albedo = ((brick.x + brick.y + brick.z) & 1) != 0 ? float3(0.95, 0.55, 0.25) : float3(0.25, 0.55, 0.95);
    else if (p.y < 1.0 + 2.0 * voxel)
    {
        float3 c = clamp(p, 0.01, WORLD - 0.01);          // on a cut face floor(p.x) would flicker between two squares
        albedo = float3(0.45, 0.50, 0.42) * lerp(0.8, 1.0, muMod(floor(c.x) + floor(c.z), 2.0));
    }
    float3 l = normalize(float3(0.40, 0.80, 0.45));
    return float4(albedo * (0.28 + 0.75 * saturate(dot(n, l))), 1.0);
}
