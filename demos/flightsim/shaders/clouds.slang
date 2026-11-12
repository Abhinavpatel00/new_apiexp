// clouds.slang — the volumetric cloud march.
//
//   compClouds   one dispatch over the cloud target, writing accumulated
//                in-scattered radiance in rgb and transmittance in a, plus the
//                distance of the first contact in a second target
//
// The pass carries four algorithm variants behind one push-constant mode, so
// the cost of every optimisation can be measured against the one before it
// rather than argued about:
//
//   1  analytic   density from five octaves of periodic noise per sample,
//                 uniform stepping, no jitter, no early exit, cone taps from the
//                 same analytic field. This is the straightforward implementation
//                 and the baseline the rest are measured against.
//   2  volume     + the density is baked into 3D textures and read with one
//                 hardware trilinear fetch, and the light cone reads the
//                 low-frequency volume
//   3  adaptive   + coarse-to-fine stepping: the march only pays for detail
//                 inside a cloud, and stops as soon as transmittance is gone
//   4  temporal   + jittered entry point, so the frame can be accumulated with
//                 its history at a fraction of the resolution
//
// The variants are a uniform branch on a push constant, so they cost nothing in
// divergence; they exist to be measured, and README.md records the numbers.
//
// Reference: Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in
// Frostbite" (SIGGRAPH 2016) and Nubla de Carpentier / Arcas, "Decima Engine:
// Advances in Lighting and AA" (SIGGRAPH 2015), whose cone light march and
// multiple-scattering octave series are what fsLightEnergy() implements.
#include "fs_common.slang"

import mu_gfx;

// Sampled 3D volumes. They share the bindless index space with mu_gfx's 2D
// array at the same binding, which is the arrangement mu_volume.slang documents:
// index a volume only through this array and a 2D image only through that one.
[[vk::binding(0, 0)]] Texture3D<float4> fsVolumes[];
// Both outputs are rgba16f, so one storage array at binding 1 serves them.
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> fsCloudOut[];

struct CloudPush
{
    float4 eye;         // xyz camera, w tan(fov_y / 2)
    float4 right;       // xyz camera right, w aspect
    float4 up;          // xyz camera up
    float4 forward;     // xyz camera forward
    float4 sun;         // xyz direction TO the sun, w intensity
    float4 sun_color;   // rgb the sun's colour at this elevation
    float4 wind;        // x wind u in map units per second, y wind v, z seconds, w cone spread
    float4 quality;     // x mode, y coarse budget, z refine steps, w jitter amount
    float4 shape;       // x coverage bias, y extinction per unit density, z detail amount,
                        // w the transmittance below which the march stops
    float4 size;        // x target width, y height, z 1 / width, w 1 / height
    float4 scene;       // x scene width, y height, z 1 / width, w 1 / height
    uint4  idx;         // x weather map, y base_hi volume, z base_lo volume, w detail volume
    uint4  idx2;        // x volume sampler, y linear clamp sampler, z scene linear depth,
                        // w the accumulation output
    uint4  idx3;        // x the first-contact-distance output
};
[[vk::push_constant]] ConstantBuffer<CloudPush> pc;

// ---- density --------------------------------------------------------------

// The weather map, scrolled by the wind. Sampling the map is what makes weather
// move without anything being re-baked: the field is static and the coordinate
// is not.
float4 fsWeatherAt(float3 pos)
{
    float2 uv = pos.xz * (1.0 / FS_WEATHER_TILE) + pc.wind.xy * pc.wind.z;
    return muSampleLevel(pc.idx.x, pc.idx2.y, uv, 0.0);
}

// The analytic density: what variant 1 marches through. Same field the volumes
// are baked from, evaluated live instead of fetched, so the comparison between
// the variants is a comparison of where the samples come from and nothing else.
float fsAnalyticDensity(float3 pos, float coverage, float type, bool detail)
{
    float h = fsPlanetHeight(pos);
    if (h < FS_CLOUD_BASE || h > FS_CLOUD_TOP) return 0.0;

    float3 lattice = float3(pos.x / (FS_BASE_TILE / 12.0),
                            (h - FS_CLOUD_BASE) / ((FS_CLOUD_TOP - FS_CLOUD_BASE) / 6.0),
                            pos.z / (FS_BASE_TILE / 12.0));
    float base = fsCloudShape(lattice, int3(12, 6, 12), detail ? 5 : 4);
    float d    = fsRemap(base, 1.0 - coverage, 1.0) * fsHeightGradient(h, type);
    if (!detail) return d;

    float3 dl    = lattice * 6.0 + float3(5.3, 2.9, 8.1);
    float  erode = fsCloudShape(dl, int3(72, 36, 72), 3);
    // Multiplicative, not a second gate: remapping d against a threshold made
    // from its own value deleted every density below ~0.3, which is most of a
    // cloud, and left the sky with wisps. Scaling by the sharpened mask keeps
    // the interior and bites at the edges, which is what erosion is for.
    return d * lerp(1.0, fsRemap(erode, 0.2, 0.9), pc.shape.z);
}

// The volume density. One trilinear fetch of the base field, one of the detail
// field, and the same coverage gate and height profile the analytic path uses.
//
// `hi` picks between the two base levels: the march reads base_hi, the light
// cone reads base_lo, which is the poor man's mip chain that stands in for the
// one this API cannot build (see bake_volume.slang).
// Nearest and trilinear volume fetches built on Load(); see the comment at
// the call site for why the hardware filtered fetch is not used. `dim` is a
// power of two in every volume this demo bakes, so the horizontal wrap is a
// mask and the vertical edge clamps.
float4 fsVolLoad(Texture3D<float4> vol, uint dim, float3 uvw)
{
    int3 t = int3(int(fract(uvw.x) * float(dim)) & (int(dim) - 1),
                  min(int(saturate(uvw.y) * float(dim)), int(dim) - 1),
                  int(fract(uvw.z) * float(dim)) & (int(dim) - 1));
    return vol.Load(int4(t, 0));
}

float4 fsVolTrilinear(Texture3D<float4> vol, uint dim, float3 uvw)
{
    float3 p   = float3(fract(uvw.x), saturate(uvw.y), fract(uvw.z)) * float(dim) - 0.5;
    int3   b   = int3(floor(p));
    float3 f   = p - float3(b);
    int    dm  = int(dim);
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    [ForceUnroll]
    for (int dx = 0; dx < 2; ++dx) {
        [ForceUnroll]
        for (int dy = 0; dy < 2; ++dy) {
            [ForceUnroll]
            for (int dz = 0; dz < 2; ++dz) {
                float3 w = float3(lerp(1.0 - f.x, f.x, float(dx)),
                                  lerp(1.0 - f.y, f.y, float(dy)),
                                  lerp(1.0 - f.z, f.z, float(dz)));
                int3   t = int3((b.x + dx) & (dm - 1),
                                 min(max(b.y + dy, 0), dm - 1),
                                 (b.z + dz) & (dm - 1));
                acc += (w.x * w.y * w.z) * vol.Load(int4(t, 0));
            }
        }
    }
    return acc;
}

float fsVolumeDensity(float3 pos, float coverage, float type, float lod, bool hi, bool detail)
{
    float h = fsPlanetHeight(pos);
    if (h < FS_CLOUD_BASE || h > FS_CLOUD_TOP) return 0.0;

    // Horizontal coordinates are not frac()'d: the sampler repeats on u and w
    // and clamps on v, so the wrap happens in hardware and the volume tiles.
    float2 scroll = pc.wind.xy * pc.wind.z * (FS_WEATHER_TILE / FS_BASE_TILE);
    float3 uvw    = float3(pos.x / FS_BASE_TILE + scroll.x,
                           (h - FS_CLOUD_BASE) / (FS_CLOUD_TOP - FS_CLOUD_BASE),
                           pos.z / FS_BASE_TILE + scroll.y);
    // A hardware trilinear fetch of a 3D image from a compute stage returns
    // zero on some drivers (llvmpipe among them): SampleLevel and a repeating
    // sampler both read nothing here, while Load works. So the fetch is eight
    // Loads and a manual trilinear, which is driver-proof and lets the volume
    // wrap in u and w by masking the texel index: the baked field is periodic
    // across the whole volume, so the wrap is exact, not a seam.
    float4 v      = fsVolTrilinear(hi ? fsVolumes[NonUniformResourceIndex(pc.idx.y)]
                                      : fsVolumes[NonUniformResourceIndex(pc.idx.z)],
                                   hi ? FS_VOL_HI_DIM : FS_VOL_LO_DIM, uvw);

    // The two base channels are two different fields; the weather map's type
    // channel picks between them, so stratus and cumulus have different
    // structure without a second volume or a second fetch.
    float base    = lerp(v.g, v.r, type);
    float d       = fsRemap(base, 1.0 - coverage, 1.0) * fsHeightGradient(h, type);
    if (!detail || d <= 0.0) return d;

    float3 duvw   = float3(pos.x / FS_DETAIL_TILE + scroll.x * 8.0, uvw.y,
                           pos.z / FS_DETAIL_TILE + scroll.y * 8.0);
    float  erode  = fsVolTrilinear(fsVolumes[NonUniformResourceIndex(pc.idx.w)],
                                   FS_VOL_DETAIL_DIM, duvw).r;
    // The same multiplicative erosion as the analytic path: the mask scales
    // the density instead of gating it, so a cloud keeps its interior and
    // frays at the edges instead of vanishing below a threshold.
    return d * lerp(1.0, fsRemap(erode, 0.2, 0.9), pc.shape.z);
}

// The density the march integrates, and the one the light cone approximates.
// `coverage` and `type` are passed in because the cone reuses the primary
// sample's weather fetch: the cone reaches at most two kilometres and the
// weather map resolves 96 m, so re-reading it six times buys detail the eye
// cannot separate from the light shaft it is supposed to be shaping.
float fsDensity(float3 pos, float coverage, float type, bool detail)
{
    if (pc.quality.x == (float)FS_CLOUDS_ANALYTIC) return fsAnalyticDensity(pos, coverage, type, detail);
    return fsVolumeDensity(pos, coverage, type, 0.0, true, detail);
}

float fsDensityLow(float3 pos, float coverage, float type, float lod)
{
    if (pc.quality.x == (float)FS_CLOUDS_ANALYTIC) return fsAnalyticDensity(pos, coverage, type, false);
    return fsVolumeDensity(pos, coverage, type, lod, false, false);
}

// ---- lighting -------------------------------------------------------------

// Henyey-Greenstein, dual lobe: a strong forward peak and a weak backscatter
// one. Clouds are neither isotropic nor purely forward, and a single lobe reads
// as either a lightbox or a grey lump. Normalised so 1.0 means isotropic.
float fsPhase(float cos_t)
{
    float g1 = 0.62, g2 = -0.28;
    float h1 = (1.0 - g1 * g1) / pow(max(1.0 + g1 * g1 - 2.0 * g1 * cos_t, 1e-4), 1.5);
    float h2 = (1.0 - g2 * g2) / pow(max(1.0 + g2 * g2 - 2.0 * g2 * cos_t, 1e-4), 1.5);
    return (0.6 * h1 + 0.4 * h2) * 0.25;      // 1 / (4 pi) folded into the mix
}

// The light march: six cone samples toward the sun at increasing distance, each
// read from the low-frequency level. A cone, not a ray, because a shadow ray
// from one point inside a cloud produces a hard speckle the size of one texel;
// widening it with distance is what makes the shadow soft and the shaft legible.
//
// Returns the optical depth from this sample toward the sun, already normalised:
// the weights sum to 1.0 including the primary sample's own density.
float fsLightOpticalDepth(float3 pos, float3 sun_dir, float coverage, float type, float self_density)
{
    static const float k_dist[5] = { 90.0, 210.0, 430.0, 880.0, 1750.0 };
    static const float k_wt[6]   = { 0.28, 0.24, 0.19, 0.13, 0.09, 0.07 };
    float sum = k_wt[0] * self_density;
    [ForceUnroll]
    for (int i = 0; i < 5; ++i) {
        float3 sp = pos + sun_dir * (k_dist[i] * pc.wind.w);
        sum += k_wt[i + 1] * fsDensityLow(sp, coverage, type, float(i));
    }
    return sum;
}

// Multiple scattering as an octave series. A single Beer-Lambert term makes
// thick cloud black, which is wrong: light that has scattered twice inside the
// cloud still leaves it. Each octave halves the optical depth and its own
// contribution, and the sum is normalised by the octave weights so a thin cloud
// is not brighter than the light that entered it.
float fsLightEnergy(float optical_depth)
{
    float ms    = 0.0;
    float scale = 1.0;
    [ForceUnroll]
    for (int i = 0; i < 4; ++i) {
        float od = optical_depth * scale;
        // Beer's law times the powder term: the powder term is what darkens the
        // crevices, where light has to pass through cloud twice to get out.
        ms   += scale * exp(-od) * (1.0 - exp(-2.0 * od)) * 2.0;
        scale *= 0.5;
    }
    return ms / 1.875;      // 1 + 0.5 + 0.25 + 0.125
}

// ---- the march ------------------------------------------------------------

[numthreads(8, 8, 1)]
void compClouds(uint3 id : SV_DispatchThreadID)
{
    uint2 out_dims = uint2(uint(pc.size.x + 0.5), uint(pc.size.y + 0.5));
    if (any(id.xy >= out_dims)) return;

    uint   mode    = uint(pc.quality.x + 0.5);
    float4 empty   = float4(0.0, 0.0, 0.0, 1.0);
    if (mode == 0u) {
        fsCloudOut[NonUniformResourceIndex(pc.idx2.w)][id.xy] = empty;
        fsCloudOut[NonUniformResourceIndex(pc.idx3.x)][id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Texel y = 0 is the top of the image, matching the fragment stages' uv
    // convention under vk.c's negative-height viewport.
    float2 ndc = float2((float(id.x) + 0.5) * pc.size.z * 2.0 - 1.0,
                        1.0 - (float(id.y) + 0.5) * pc.size.w * 2.0);
    float3 rd  = normalize(pc.forward.xyz
                           + pc.right.xyz * (ndc.x * pc.right.w * pc.eye.w)
                           + pc.up.xyz * (ndc.y * pc.eye.w));
    float3 ro  = pc.eye.xyz;

    // Cheapest exit in the pass: four flops and a sqrt, and on a typical frame
    // most of the sky takes it.
    float t0, t1;
    if (!fsShellIntersect(ro, rd, t0, t1)) {
        fsCloudOut[NonUniformResourceIndex(pc.idx2.w)][id.xy] = empty;
        fsCloudOut[NonUniformResourceIndex(pc.idx3.x)][id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Terrain occlusion. The minimum of the four full-res depths this texel
    // covers, not the centre one: a single sample lets cloud bleed through the
    // near side of a ridge at the half-resolution boundary.
    float scene_d = 0.0;
    {
        int2 base = int2(float2(id.xy) * (pc.scene.xy * pc.size.zw));
        float d00 = muSampledImages[NonUniformResourceIndex(pc.idx2.z)].Load(int3(base + int2(0, 0), 0)).r;
        float d10 = muSampledImages[NonUniformResourceIndex(pc.idx2.z)].Load(int3(base + int2(1, 0), 0)).r;
        float d01 = muSampledImages[NonUniformResourceIndex(pc.idx2.z)].Load(int3(base + int2(0, 1), 0)).r;
        float d11 = muSampledImages[NonUniformResourceIndex(pc.idx2.z)].Load(int3(base + int2(1, 1), 0)).r;
        // Zero means "nothing drawn here", so it must not win the minimum.
        float big = pc.forward.w * 2.0;
        d00 = d00 > 0.0 ? d00 : big; d10 = d10 > 0.0 ? d10 : big;
        d01 = d01 > 0.0 ? d01 : big; d11 = d11 > 0.0 ? d11 : big;
        float dmin = min(min(d00, d10), min(d01, d11));
        scene_d = dmin < big ? dmin : 0.0;
    }
    if (scene_d > 0.0) t1 = min(t1, scene_d);
    if (t1 <= t0) {
        fsCloudOut[NonUniformResourceIndex(pc.idx2.w)][id.xy] = empty;
        fsCloudOut[NonUniformResourceIndex(pc.idx3.x)][id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // ---- stepping ---------------------------------------------------------
    float span         = t1 - t0;
    int   budget       = int(pc.quality.y + 0.5);
    // The step is the span divided by the budget, clamped so a ray that just
    // grazes the deck does not take 96 steps of two metres, and a ray that
    // crosses it at the horizon does not take steps longer than the detail
    // volume can resolve.
    float coarse       = clamp(span / float(budget), 24.0, 460.0);
    int   refine_count = int(pc.quality.z + 0.5);
    float fine         = coarse / float(refine_count + 1);

    // Jitter the entry point by a fraction of one step. Without it, halving the
    // resolution halves the information; with it, consecutive frames sample
    // different points of the same cloud and the history pass recovers the rest.
    // Interleaved gradient noise rather than a hash because it is one FMA and
    // because its correlation pattern is the one a temporal filter removes best.
    if (mode >= FS_CLOUDS_TEMPORAL && pc.quality.w > 0.0) {
        float2 seed = float2(frac(pc.wind.z * 0.6180339887) * 61.0,
                             frac(pc.wind.z * 0.3247179572) * 47.0);
        t0 += mu_ign(float2(id.xy) + seed) * coarse * pc.quality.w;
        t0 = min(t0, t1);
    }

    float3 sun_dir     = normalize(pc.sun.xyz);
    float  cos_phase   = dot(rd, sun_dir);
    float  phase       = fsPhase(cos_phase);

    float3 radiance    = float3(0.0, 0.0, 0.0);
    float  trans       = 1.0;
    float  first_hit   = 0.0;
    float  cur_step    = coarse;
    float  t           = t0;
    int    refine_left = 0;
    int    i           = 0;

    // One loop, and the only data-dependent branch inside it is the density
    // test itself. The variant switch is hoisted out of the accumulation by
    // fsDensity(); the refine counter is arithmetic, not a second loop.
    while (i < budget + refine_count && t < t1 && trans > pc.shape.w) {
        float3 pos       = ro + rd * t;
        float4 weather   = fsWeatherAt(pos);
        float  coverage  = saturate(weather.r + pc.shape.x);
        float  type      = saturate(weather.g);
        float  precip    = weather.b;

        float  d         = coverage > 0.002 ? fsDensity(pos, coverage, type, true) : 0.0;

        if (d > 0.0) {
            if (first_hit <= 0.0) first_hit = t;

            // Coarse-to-fine: on the first contact, back up to the last point
            // that was empty and walk the gap again at 1/N the step. The cloud's
            // near edge is where the silhouette is decided, so that is the only
            // place worth paying for extra samples; the interior is smooth
            // enough that the coarse step integrates it accurately.
            if (mode >= FS_CLOUDS_ADAPTIVE && refine_left == 0 && refine_count > 0 && cur_step > fine) {
                refine_left = refine_count;
                cur_step    = fine;
                t          -= coarse;
                i++;
                continue;
            }

            float sigma = d * pc.shape.y;
            float od    = sigma * cur_step;
            float tr    = exp(-od);

            float  od_sun = fsLightOpticalDepth(pos, sun_dir, coverage, type, d) * pc.shape.y;
            float  energy = fsLightEnergy(od_sun);
            float  hgt    = fsRemap(fsPlanetHeight(pos), FS_CLOUD_BASE, FS_CLOUD_TOP);

            // Ambient is the sky above, weighted by how high in the deck this
            // sample is: the base of a cloud is in its own shadow.
            float3 amb_col = lerp(pc.sun_color.rgb, float3(0.40, 0.54, 0.76), 0.72)
                             * pc.sun.w * (0.35 + 0.65 * hgt);
            // Rain darkens the base, which is the only cue a player has that a
            // cell is a storm rather than a fair-weather cumulus.
            float  rain    = 1.0 - precip * 0.55 * (1.0 - hgt);

            float3 lighting = (pc.sun_color.rgb * pc.sun.w * energy * phase + amb_col) * rain;

            // The analytic integral of the source term over a homogeneous step:
            // L * (1 - e^(-sigma ds)), scaled by the transmittance that reached
            // it. Integrating rather than sampling at the midpoint is what lets
            // the coarse step stay coarse without banding.
            radiance += lighting * trans * (1.0 - tr);
            trans    *= tr;
        } else if (refine_left > 0) {
            // Refinement is a fixed number of steps after the first contact,
            // hit or miss, then the march goes back to the coarse step.
            refine_left--;
            if (refine_left == 0) cur_step = coarse;
        }

        t += cur_step;
        i++;
    }

    fsCloudOut[NonUniformResourceIndex(pc.idx2.w)][id.xy] = float4(radiance, trans);
    fsCloudOut[NonUniformResourceIndex(pc.idx3.x)][id.xy] = float4(first_hit, float(i), 0.0, 0.0);
}
