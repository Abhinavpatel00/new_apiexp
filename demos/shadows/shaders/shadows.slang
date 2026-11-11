// shadows.slang — shadow filtering lab.
//
//   vertShadow / fragShadow   depth-only caster pass (one map per pass)
//   vertMain / fragMain       forward pass: six filters, cascades, debug views
//
// The filters are external/mu_lib/mu/shaders/shadows.slang: mu_pcf3x3,
// mu_pcf_poisson, mu_pcss_blocker, mu_depth_bias, mu_normal_offset,
// mu_cascade_blend_weight. This file chooses between them and feeds them.
//
// Conventions: directional light, orthographic maps, depth 0 (near the light)
// to 1. The maps are rendered through vk.c's negative-height viewport, so
// v = 0.5 - 0.5 * ndc.y.
import mu_gfx;
import mu_mesh;
import mu_depth;
#include "shaders/shadows.slang"

static const uint MAPS = 4u; // 0 = whole scene, 1..3 = cascades

struct FrameData
{
    MuMat4 view_proj;
    MuMat4 light_vp[4];
    float4 eye;          // xyz
    float4 forward;      // xyz camera forward, w output width in pixels
    float4 light;        // xyz direction TO the light, w angular diameter (tan)
    float4 splits;       // xyz far view depth of cascades 1..3, w blend width
    float4 bias;         // x constant, y slope, z normal offset - all in shadow texels
    float4 filter;       // x PCF radius (texels), y blocker search radius (texels), z PCSS rotations
    float4 texel_world;  // world size of one texel, per map
    float4 depth_range;  // world length of depth 0..1, per map
    float4 inv_size;     // 1 / resolution, per map
    uint4  map;          // bindless index, per map
    uint4  samplers;     // x point, y compare nearest, z compare linear
};
static const uint FRAME_SIZE = 496u;

FrameData loadFrame(uint o)
{
    FrameData f;
    f.view_proj   = muLoadMat4(o);
    f.light_vp[0] = muLoadMat4(o + 64u);
    f.light_vp[1] = muLoadMat4(o + 128u);
    f.light_vp[2] = muLoadMat4(o + 192u);
    f.light_vp[3] = muLoadMat4(o + 256u);
    f.eye         = muScene.Load<float4>(o + 320u);
    f.forward     = muScene.Load<float4>(o + 336u);
    f.light       = muScene.Load<float4>(o + 352u);
    f.splits      = muScene.Load<float4>(o + 368u);
    f.bias        = muScene.Load<float4>(o + 384u);
    f.filter      = muScene.Load<float4>(o + 400u);
    f.texel_world = muScene.Load<float4>(o + 416u);
    f.depth_range = muScene.Load<float4>(o + 432u);
    f.inv_size    = muScene.Load<float4>(o + 448u);
    f.map         = muScene.Load<uint4>(o + 464u);
    f.samplers    = muScene.Load<uint4>(o + 480u);
    return f;
}

struct DrawPush
{
    uint  vertex_offset;  // byte offset of this draw's first vertex
    uint  object_offset;
    uint  frame_offset;
    uint  map;            // shadow pass: which light matrix
    uint  technique_a;    // left of the split
    uint  technique_b;    // right of the split
    float split;          // 0..1 of the width; 1 = technique_a everywhere
    uint  view;           // 0 shaded, 1 factor, 2 cascades, 3 measure, 4 seam
    uint  flags;          // bit 0: cascades
    uint  pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<DrawPush> pc;

// ------------------------------------------------------------ shadow pass --
[shader("vertex")]
float4 vertShadow(uint vertexIndex : SV_VertexID) : SV_Position
{
    MeshVertex v = muLoadMeshVertex(pc.vertex_offset, vertexIndex);
    MeshObject o = muLoadMeshObject(pc.object_offset);
    MuMat4 light = muLoadMat4(pc.frame_offset + 64u + pc.map * 64u);
    return muMul(light, float4(muMulPoint(o.model, v.position), 1.0));
}

// ----------------------------------------------------------- forward pass --
struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float3 albedo : TEXCOORD2;
};

[shader("vertex")]
Varyings vertMain(uint vertexIndex : SV_VertexID)
{
    MeshVertex v = muLoadMeshVertex(pc.vertex_offset, vertexIndex);
    MeshObject o = muLoadMeshObject(pc.object_offset);
    Varyings r;
    r.world    = muMulPoint(o.model, v.position);
    r.normal   = muMulDir(o.model, v.normal);
    r.albedo   = o.color.rgb;
    r.position = muMul(muLoadMat4(pc.frame_offset), float4(r.world, 1.0));
    return r;
}

// MU_POISSON16 fills the square [-1,1]^2, not the unit disc: its farthest
// point is 1.234 from the centre and its mean squared radius is 0.854,
// against 0.5 for a uniform unit disc. A disc light of diameter W needs a
// uniform disc kernel of radius W/2, so the PCSS radius is scaled to give the
// table the same variance: sqrt(0.5 / 0.854). Without this the penumbrae come
// out 33 % too wide (measured by --check, and predicted from the table).
static const float POISSON16_AS_DISC = 0.7652;
static const float POISSON16_REACH   = 1.2339;

// One map, one technique. Returns the lit fraction.
float shadowFactor(FrameData f, uint m, uint technique, float3 world, float3 n, float2 pixel)
{
    float3 L   = f.light.xyz;
    float  ndl = dot(n, L);

    // Receiver-side bias, all of it in units of this map's texel so that it
    // follows the resolution (a far cascade needs more world-space bias):
    //   1. push the lookup position along the normal (mu_normal_offset);
    //   2. a constant depth bias;
    //   3. a slope bias. A filter that reaches r texels sideways reads depths
    //      of a planar receiver that differ by up to r * texel * tan(theta),
    //      theta = angle between normal and light - so the slope term is
    //      scaled by the reach of the filter in use. (mu_depth_bias's
    //      (1 - NoL) proxy underestimates tan at low sun; tan is used here.)
    float3 p     = mu_normal_offset(world, n, L, f.texel_world[m], f.bias.z);
    float4 clip  = muMul(f.light_vp[m], float4(p, 1.0));
    float2 uv    = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    float  c     = clamp(ndl, 0.05, 1.0);
    float  slope = min(sqrt(1.0 - c * c) / c, 8.0);
    float  unit  = f.texel_world[m] / f.depth_range[m];        // one texel, in depth
    float  bias0 = f.bias.x * unit;
    float  bias1 = f.bias.y * slope * unit;                    // per texel of reach
    if (mu_shadow_in_bounds(uv, clip.z) < 0.5) return 1.0;
    if (ndl <= 0.0) return 0.0;                                // faces away: unlit anyway

    Texture2D<float>       map    = muDepthImages[NonUniformResourceIndex(f.map[m])];
    SamplerState           point  = muSamplers[NonUniformResourceIndex(f.samplers.x)];
    SamplerComparisonState cmpNear = muCompareSamplers[NonUniformResourceIndex(f.samplers.y)];
    SamplerComparisonState cmpLin  = muCompareSamplers[NonUniformResourceIndex(f.samplers.z)];
    float2 inv = float2(f.inv_size[m]);

    switch (technique)
    {
    case 0: // one hard tap
        return map.SampleCmpLevelZero(cmpNear, uv, clip.z - bias0 - bias1);
    case 1: // 3x3 hard taps: ten grey levels, blocky
        return mu_pcf3x3(map, cmpNear, uv, clip.z - bias0 - bias1 * 1.5, inv);
    case 2: // one hardware bilinear comparison (2x2 texels)
        return map.SampleCmpLevelZero(cmpLin, uv, clip.z - bias0 - bias1);
    case 3: // 3x3 hardware bilinear comparisons (4x4 texels)
        return mu_pcf3x3(map, cmpLin, uv, clip.z - bias0 - bias1 * 2.0, inv);
    case 4: // 16 Poisson taps, rotated per pixel
        return mu_pcf_poisson(map, point, uv, clip.z - bias0 - bias1 * (f.filter.x * POISSON16_REACH + 1.0), inv, f.filter.x, pixel, 1.0);
    default:
    {
        // PCSS: average blocker depth -> penumbra width -> filter radius.
        // Directional light: the penumbra grows linearly with the world
        // distance between blocker and receiver, W = distance * angular size.
        // The search is biased for its own reach, so the receiver's own
        // plane is never counted as a blocker (that would drag the average
        // towards zero distance and narrow every penumbra).
        float searchRef = clip.z - bias0 - bias1 * (f.filter.y * POISSON16_REACH + 1.0);
        float blocker   = mu_pcss_blocker(map, point, uv, searchRef, inv, f.filter.y, 1.0);
        // Nothing found further than the search bias: either open ground, or
        // right at a contact where the penumbra is zero anyway. A small
        // filter answers both.
        if (blocker < 0.0) return mu_pcf3x3(map, cmpLin, uv, clip.z - bias0 - bias1 * 2.0, inv);
        float distance = (clip.z - blocker) * f.depth_range[m];
        float radius   = max(0.5 * distance * f.light.w / f.texel_world[m] * POISSON16_AS_DISC, 1.0);
        float ref = clip.z - bias0 - bias1 * (radius * POISSON16_REACH + 1.0);
        float sum = 0.0;
        int   n   = clamp((int)f.filter.z, 1, 8);
        for (int k = 0; k < n; ++k)
            sum += mu_pcf_poisson(map, point, uv, ref, inv, radius, pixel + float2(17.0 * k, 5.0 * k), 1.0);
        return sum / (float)n;
    }
    }
}

static const float3 CASCADE_TINT[4] = {
    float3(1.0, 1.0, 1.0), float3(1.0, 0.45, 0.4), float3(0.45, 1.0, 0.5), float3(0.45, 0.55, 1.0)};

[shader("fragment")]
float4 fragMain(Varyings i) : SV_Target0
{
    FrameData f = loadFrame(pc.frame_offset);
    float3 n = normalize(i.normal);
    float3 L = f.light.xyz;
    uint technique = i.position.x < pc.split * f.forward.w ? pc.technique_a : pc.technique_b;

    // Cascade selection by view depth, blending into the next one near the
    // far end of each slice so the resolution change is not a visible line.
    uint  m0 = 0u, m1 = 0u;
    float w  = 0.0;
    bool  inBand = false;
    if ((pc.flags & 1u) != 0u)
    {
        float depth = dot(i.world - f.eye.xyz, f.forward.xyz);
        uint  c     = depth < f.splits.x ? 0u : depth < f.splits.y ? 1u : 2u;
        m0 = c + 1u;
        m1 = min(c + 2u, 3u);
        w  = c < 2u ? mu_cascade_blend_weight(depth, f.splits[c], f.splits.w) : 0.0;
        inBand = w > 0.0 && w < 1.0;
    }
    float s0 = shadowFactor(f, m0, technique, i.world, n, i.position.xy);
    float s1 = (w > 0.0 || pc.view == 4u) ? shadowFactor(f, m1, technique, i.world, n, i.position.xy) : s0;
    float shadow = lerp(s0, s1, w);

    // Checker on upward faces near y = 0 (the ground).
    float3 albedo = i.albedo;
    bool ground = n.y > 0.9 && abs(i.world.y) < 1e-3;
    if (ground)
    {
        float2 g = floor(i.world.xz);
        albedo *= lerp(0.82, 1.0, muMod(g.x + g.y, 2.0));
    }

    float  ndl     = saturate(dot(n, L));
    float3 sun     = float3(1.0, 0.94, 0.84) * 1.7;
    float3 ambient = lerp(float3(0.10, 0.09, 0.08), float3(0.20, 0.27, 0.42), n.y * 0.5 + 0.5);
    float3 color   = albedo * (sun * ndl * shadow + ambient);

    switch (pc.view)
    {
    case 1:  color = float3(shadow); break;
    case 2:  color *= CASCADE_TINT[m0]; if (inBand) color = lerp(color, float3(1.0, 1.0, 0.2), 0.5); break;
    // Data for --check: factor and world position on the ground; flags in a.
    case 3:  return float4(shadow, i.world.x, i.world.z, ground ? 1.0 : 0.0);
    case 4:  return float4(s0, s1, w, inBand ? 1.0 : 0.0);
    default: break;
    }
    if (pc.split < 1.0 && abs(i.position.x - pc.split * f.forward.w) < 1.0) color = float3(0.0);
    return float4(color, 1.0);
}
