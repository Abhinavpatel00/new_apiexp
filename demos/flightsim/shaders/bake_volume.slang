// bake_volume.slang — fill one 3D density volume from the periodic noise field.
//
//   compBakeVolume   one dispatch per volume; which volume, how large, what
//                    lattice period and how many octaves are all push constants
//
// Three volumes are baked once at startup and never touched again:
//
//   base_hi   128 x 128 x 64   the shape the eye resolves, sampled on the march
//   base_lo    64 x  64 x 32   the same field at half the frequency, sampled by
//                              the light cone so a wide cone does not alias
//   detail     32 x  32 x 32   the erosion mask that breaks the base up into
//                              wisps at the cloud's edges
//
// base_lo exists because this API cannot build a mip chain for a volume: a
// storage image descriptor is bound to one subresource, so a compute pass can
// only ever write mip 0, and there is no blit on the public surface. Baking the
// low-frequency level straight from the same analytic field is both simpler and
// slightly better than a box filter of the high level, which would blur the
// warp instead of re-evaluating it. The cost is one extra volume and one extra
// dispatch at startup.
#include "fs_common.slang"

import mu_gfx;
#include "mu_volume.slang"      // muStorageVolumes: RWTexture3D<float4>, rgba16f

struct BakePush
{
    uint4  dims;     // x width, y height, z depth, w bindless index of the volume
    uint4  period;   // x lattice cells across X, y across Y, z across Z, w octaves
    uint4  mode;     // x 0 = base, 1 = detail; y warp strength (0.001 units); z, w unused
    float4 shape;    // x contrast, y bias, z second-channel frequency ratio, w unused
};
[[vk::push_constant]] ConstantBuffer<BakePush> pc;

[numthreads(4, 4, 4)]
void compBakeVolume(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= pc.dims.xyz)) return;

    // Texel centre in lattice units. The lattice wraps on `period`, so the
    // volume tiles horizontally when the march takes frac() of its world
    // coordinate; Y is clamped inside fsPeriodicValue3 because the deck has a
    // floor and a ceiling that must not be glued together.
    //
    // The mapping is the one the march assumes: volume X is world X, volume Y is
    // height and volume Z is world Z, and X and Z share the horizontal period.
    float3 lattice = float3((float(id.x) + 0.5) / float(pc.dims.x) * float(pc.period.x),
                            (float(id.y) + 0.5) / float(pc.dims.y) * float(pc.period.y),
                            (float(id.z) + 0.5) / float(pc.dims.z) * float(pc.period.z));
    int3   per = int3(pc.period.xyz);
    int    oct = int(pc.period.w);

    float contrast = pc.shape.x;
    float bias     = pc.shape.y;

    float4 out_v;
    if (pc.mode.x == 0u) {
        // Base: two shapes from one field, at two frequencies. The march picks
        // between them with the weather map's cloud-type channel, so stratus
        // and cumulus are different fields without a second volume or a second
        // sample per step.
        float a = fsCloudShape(lattice, per, oct);
        float b = fsCloudShape(lattice * pc.shape.z + float3(37.1, 11.3, 23.7), per * 2, oct);
        out_v = float4(saturate((a - bias) * contrast),
                       saturate((b - bias) * contrast),
                       a,
                       0.0);
    } else {
        // Detail: the erosion mask. Sharper than the base because it multiplies
        // the base rather than replacing it, and two channels so the march can
        // vary the erosion with height without another fetch.
        float a = fsCloudShape(lattice, per, oct);
        float b = fsCloudShape(lattice * 1.9 + float3(5.3, 2.9, 8.1), per * 2, oct);
        out_v = float4(saturate((a - bias) * contrast),
                       saturate((b - bias) * contrast),
                       0.0, 0.0);
    }
    muStorageVolumes[NonUniformResourceIndex(pc.dims.w)][id] = out_v;
}
