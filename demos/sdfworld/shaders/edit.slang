// edit.slang — compEdit: one brush applied to the volume, in place.
//
// Dispatched over the brush's bounding box only. That is enough because the
// volume stores distances within +-BAND voxels: further than radius + BAND +
// smoothing from the brush centre, the result is the old value bit for bit.
// (Claybook's reason for the limited range: with an unlimited one, a local
// change has to be propagated through the whole volume.)
//
//   op 0  generate the starting world (whole volume)
//   op 1  add clay: smooth union with a sphere, takes the brush's colour
//   op 2  dig: smooth subtraction of a sphere; the colour underneath shows
#include "compute_bindings.slang"
#include "world_common.slang"
#include "shaders/sdf.slang"

struct EditPush
{
    uint   target, op, slot, flags;           // flags bit 0: the centre is the float4 at `slot`
                                              // (xyz voxels, w 0 = nothing to do): a picked
                                              // point or a ball's impact, written by sim.slang
    float4 centre_radius;                     // voxels
    float4 color_k;                           // rgb, smoothing in voxels
    int4   base;                              // first voxel of the box
    uint4  dims_tiles;                        // VX, VY, VZ
    float4 band_voxel;                        // BAND, metres per voxel
};
[[vk::push_constant]] ConstantBuffer<EditPush> pc;

// The starting world, in metres: rolling ground, a boulder, a wall with a
// gate, a half-buried ring. Colour: strata by depth below the surface, so a
// hole shows layers.
float terrain(float3 p, out float3 albedo)
{
    float h = 3.2 + 1.1 * sin(0.33 * p.x) * cos(0.29 * p.z) + 0.5 * sin(0.85 * p.x + 1.3) * sin(0.8 * p.z + 0.4);
    // A height field is not a distance. Its slope is at most 0.788 in x and
    // 0.719 in z; dividing by sqrt(1 + 0.788^2 + 0.719^2) makes it a lower
    // bound on the distance, which is all tracing needs (--check measures it).
    float ground = (p.y - h) / 1.47;
    float depth  = h - p.y;
    float3 strata = depth < 0.35 ? float3(0.30, 0.52, 0.20)           // turf
                  : depth < 1.4  ? float3(0.50, 0.36, 0.22)           // earth
                  : depth < 2.4  ? float3(0.62, 0.55, 0.42)           // sand
                  :                float3(0.38, 0.38, 0.42);          // rock
    if (depth >= 0.35) strata *= 0.9 + 0.1 * sin(p.y * 9.0);
    albedo = strata;

    float boulder = mu_sd_sphere(p - float3(-6.0, 3.6, 5.0), 2.2);
    float3 q      = p - float3(5.0, 4.2, -4.0);
    float wall    = mu_sd_round_box(q, float3(4.0, 2.6, 0.7), 0.2);
    float gate    = mu_sd_round_box(q - float3(0.0, -1.2, 0.0), float3(1.1, 1.8, 1.5), 0.3);
    wall          = max(wall, -gate);
    float3 r      = p - float3(-4.0, 3.4, -7.0);
    float ring    = mu_sd_torus(r.xzy, float2(2.4, 0.6));

    float d = ground;
    if (boulder < d + 0.3) albedo = lerp(albedo, float3(0.55, 0.50, 0.48), saturate((d - boulder) / 0.3 + 0.5));
    d = mu_sd_smin(d, boulder, 0.6);
    if (wall < d + 0.3) albedo = lerp(albedo, float3(0.72, 0.30, 0.22), saturate((d - wall) / 0.3 + 0.5));
    d = mu_sd_smin(d, wall, 0.4);
    if (ring < d + 0.3) albedo = lerp(albedo, float3(0.85, 0.68, 0.22), saturate((d - ring) / 0.3 + 0.5));
    d = mu_sd_smin(d, ring, 0.5);
    return d;
}

[shader("compute")]
[numthreads(4, 4, 4)]
void compEdit(uint3 id : SV_DispatchThreadID)
{
    float3 centre = pc.centre_radius.xyz;
    int3   base   = pc.base.xyz;
    if ((pc.flags & 1u) != 0u)
    {
        float4 pick = muSceneRW.Load<float4>(pc.slot);
        if (pick.w < 0.5) return;                               // no hit, no impact
        centre = pick.xyz;
        base   = (int3)floor(centre) - pc.base.w;               // base.w: half the box, voxels
    }
    int3 voxel = base + (int3)id;
    if (any(voxel < 0) || any(voxel >= (int3)pc.dims_tiles.xyz)) return;
    float3 p    = (float3)voxel + 0.5;
    float  band = pc.band_voxel.x;

    float4 value;
    if (pc.op == 0u)
    {
        float3 m = p * pc.band_voxel.y - float3(0.5 * pc.dims_tiles.x * pc.band_voxel.y, 0.0,
                                                0.5 * pc.dims_tiles.z * pc.band_voxel.y);
        float3 albedo;
        value.r   = terrain(m, albedo) / pc.band_voxel.y;
        value.gba = albedo;
    }
    else
    {
        value = muStorageVolumes[NonUniformResourceIndex(pc.target)][voxel];
        float brush = length(p - centre) - pc.centre_radius.w;
        float k     = max(pc.color_k.w, 1e-3);
        if (pc.op == 1u)
        {
            // Where the brush is the nearer surface, the clay is the brush's.
            value.gba = lerp(value.gba, pc.color_k.rgb, saturate(0.5 + 0.5 * (value.r - brush) / k));
            value.r   = mu_sd_smin(value.r, brush, k);
        }
        else
        {
            value.r = mu_sd_smax(value.r, -brush, k);
        }
    }
    value.r = clamp(value.r, -band, band);
    muStorageVolumes[NonUniformResourceIndex(pc.target)][voxel] = value;
}
