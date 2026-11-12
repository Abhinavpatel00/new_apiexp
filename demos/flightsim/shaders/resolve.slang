// resolve.slang — take the cloud march's half-resolution answer and put it into
// the full-resolution picture.
//
//   fragResolve   depth-aware upsample, temporal reprojection, composite
//
// Three jobs, one pass, because each of them already needs the values the
// others fetched:
//
//   Upsample. The march ran at half resolution, so each full-resolution pixel
//   has four candidates. Averaging them smears cloud across every ridge and
//   mast in the picture, which is the artefact that gives reduced-resolution
//   volumetrics a bad name. Instead the four stored first-contact distances are
//   compared with this pixel's own scene depth and the closest one wins, so a
//   cloud edge stops at the terrain edge rather than hanging over it.
//
//   Reproject. The cloud field is static in world space and only the camera
//   moves, so the previous frame's answer for this pixel is found by taking the
//   world point at the stored contact distance and projecting it with the
//   previous view-projection. No motion vector target and no second full-
//   resolution write.
//
//   Composite. Scene times transmittance plus accumulated in-scatter, which is
//   what the march's two outputs were arranged to make a single multiply-add.
//
// History is variance-clipped against the current frame's four candidates
// before it is blended. A flight simulator turns and climbs hard enough that
// reprojection is regularly wrong, and an unclipped history smears the old
// answer across the new one for as long as the blend weight takes to forget it.
#include "fs_common.slang"

import mu_gfx;

struct ResolvePush
{
    float4x4 prev_view_proj;   // last frame's, for the reprojection
    float4   eye;              // xyz camera
    float4   right;            // xyz camera right,   w tan(fov_y / 2)
    float4   up;               // xyz camera up,      w aspect
    float4   forward;          // xyz camera forward
    float4   params;           // x history weight, y mode, z seconds, w unused
    float4   size;             // x full width, y full height, z cloud width, w cloud height
    uint4    idx;              // x cloud accumulation, y cloud first-contact distance,
                               // z previous accumulation, w previous distance
    uint4    idx2;             // x scene hdr, y scene linear depth, z nearest sampler,
                               // w linear clamp sampler
};
[[vk::push_constant]] ConstantBuffer<ResolvePush> pc;

float4 fsCloudAccum(int2 px)  { return muSampledImages[NonUniformResourceIndex(pc.idx.x)].Load(int3(px, 0)); }
float  fsCloudDist(int2 px)   { return muSampledImages[NonUniformResourceIndex(pc.idx.y)].Load(int3(px, 0)).r; }

float4 fragResolve(FullscreenVaryings i) : SV_Target0
{
    int2   full_px = int2(i.position.xy);
    float3 scene   = muSampledImages[NonUniformResourceIndex(pc.idx2.x)].Load(int3(full_px, 0)).rgb;
    float  depth   = muSampledImages[NonUniformResourceIndex(pc.idx2.y)].Load(int3(full_px, 0)).r;

    uint   mode    = uint(pc.params.y + 0.5);
    if (mode == 0u) return float4(scene, 1.0);

    // ---- depth-aware upsample ---------------------------------------------
    // The four cloud texels this pixel falls inside, and the nearest of them by
    // first-contact distance. With no geometry in front (depth == 0) there is
    // nothing for cloud to bleed over, so the four are averaged instead, which
    // is the cheaper and smoother answer.
    float2 cloud_px = float2(full_px) * (pc.size.zw * (1.0 / pc.size.xy));
    int2   base     = int2(floor(cloud_px - 0.5));
    float4 c00 = fsCloudAccum(base + int2(0, 0));
    float4 c10 = fsCloudAccum(base + int2(1, 0));
    float4 c01 = fsCloudAccum(base + int2(0, 1));
    float4 c11 = fsCloudAccum(base + int2(1, 1));

    float4 cloud = (c00 + c10 + c01 + c11) * 0.25;
    float  dist  = (fsCloudDist(base + int2(0, 0)) + fsCloudDist(base + int2(1, 0))
                    + fsCloudDist(base + int2(0, 1)) + fsCloudDist(base + int2(1, 1))) * 0.25;

    if (depth > 0.0) {
        float d00 = fsCloudDist(base + int2(0, 0));
        float d10 = fsCloudDist(base + int2(1, 0));
        float d01 = fsCloudDist(base + int2(0, 1));
        float d11 = fsCloudDist(base + int2(1, 1));
        // A texel with no contact is infinitely far for this comparison.
        float big = 1e12;
        d00 = d00 > 0.0 ? d00 : big; d10 = d10 > 0.0 ? d10 : big;
        d01 = d01 > 0.0 ? d01 : big; d11 = d11 > 0.0 ? d11 : big;
        float e00 = abs(d00 - depth), e10 = abs(d10 - depth);
        float e01 = abs(d01 - depth), e11 = abs(d11 - depth);
        float m   = min(min(e00, e10), min(e01, e11));
        cloud = e00 == m ? c00 : e10 == m ? c10 : e01 == m ? c01 : c11;
        dist  = e00 == m ? d00 : e10 == m ? d10 : e01 == m ? d01 : d11;
        if (dist >= big) dist = 0.0;
    }

    // ---- temporal reprojection --------------------------------------------
    if (mode >= FS_CLOUDS_TEMPORAL && dist > 0.0) {
        float2 ndc     = i.uv * 2.0 - 1.0;
        float3 rd      = normalize(pc.forward.xyz
                                   + pc.right.xyz * (ndc.x * pc.up.w * pc.right.w)
                                   + pc.up.xyz * (ndc.y * pc.right.w));
        float3 world   = pc.eye.xyz + rd * dist;
        float4 clip    = mul(pc.prev_view_proj, float4(world, 1.0));
        // NDC +y is image row 0 under vk.c's negative-height viewport, and a
        // texture's v = 0 is also row 0, so v is (1 - ndc.y) / 2 and not
        // (ndc.y + 1) / 2. Getting this backwards reprojects every pixel onto
        // the one mirrored about the horizon, which the variance clip then
        // rejects almost everywhere: the history silently stops contributing
        // and the pass costs what it costs for nothing.
        float2 prev_uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);

        if (all(prev_uv >= float2(0.0, 0.0)) && all(prev_uv <= float2(1.0, 1.0))) {
            float4 hist = muSampleLevel(pc.idx.z, pc.idx2.w, prev_uv, 0.0);
            float  hd   = muSampleLevel(pc.idx.w, pc.idx2.w, prev_uv, 0.0).r;

            // Reject a history that disagrees about where the cloud was. This is
            // the disocclusion test: a pixel that has just come out from behind
            // a ridge, or that a bank turn has exposed, has no valid past.
            float  rel  = abs(hd - dist) / max(dist, 1.0);
            if (hd > 0.0 && rel < 0.06) {
                // Variance clip against the four candidates already in hand:
                // the history is pulled inside the box this frame's
                // neighbourhood spans, so a wrong reprojection decays in one
                // frame instead of over the whole blend time constant.
                float3 lo = min(min(c00.rgb, c10.rgb), min(c01.rgb, c11.rgb));
                float3 hi = max(max(c00.rgb, c10.rgb), max(c01.rgb, c11.rgb));
                float  ta = min(min(c00.a, c10.a), min(c01.a, c11.a));
                float  tb = max(max(c00.a, c10.a), max(c01.a, c11.a));
                // Widen the box by a quarter of its own extent: a hard clip
                // against four samples freezes legitimate motion.
                float3 ext = (hi - lo) * 0.25 + 1e-4;
                float  eta = (tb - ta) * 0.25 + 1e-4;
                lo -= ext; hi += ext; ta -= eta; tb += eta;
                hist = float4(clamp(hist.rgb, lo, hi), clamp(hist.a, ta, tb));

                float w = clamp(pc.params.x, 0.0, 0.97);
                cloud   = lerp(cloud, hist, w);
            }
        }
    }

    // ---- composite --------------------------------------------------------
    // rgb is in-scattered radiance already weighted by the transmittance that
    // reached each sample, and a is what is left of the background, so the
    // composite is one multiply-add.
    float3 out_rgb = scene * cloud.a + cloud.rgb;
    return float4(out_rgb, 1.0);
}
