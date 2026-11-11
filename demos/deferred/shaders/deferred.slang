// deferred.slang — G-buffer, clustered lighting, and a forward reference.
//
//   vertMain / fragGBuffer   geometry -> two colour targets + depth
//   fragLighting             full screen: G-buffer + depth -> lit HDR
//   fragForward              the same shading straight from the geometry,
//                            looping over every light (the reference)
//
// G-buffer, 12 bytes per pixel plus depth:
//   target 0  RGBA8 sRGB  albedo rgb (sRGB-encoded by the attachment)
//   target 1  R32_UINT    normal, octahedral 16 + 16 bits
//   target 2  RGBA8       roughness r, metalness g
// Position is not stored: it is the pixel's ray times the depth buffer.
import mu_gfx;
import mu_mesh;
import mu_depth;
import mu_uint;
#include "shaders/lighting.slang"
#include "shaders/quantize.slang"
#include "shaders/format.slang"
#include "deferred_common.slang"

struct FrameData
{
    MuMat4 view_proj, inv_view_proj, view;
    float4 eye;
    float4 forward;   // xyz, w ambient
    float4 proj;      // tan x, tan y, cluster near, cluster far
    float4 size;      // output width, height, depth of the far plane (0: reversed depth)
    uint4  grid;      // nx, ny, nz, capacity
    uint4  data;      // lights offset, light count, clusters offset
    uint4  gbuffer;   // bindless: albedo, normal word, depth, material
    float4 moon;      // xyz direction to it, w radiance
};

FrameData loadFrame(uint o)
{
    FrameData f;
    f.view_proj     = muLoadMat4(o + F_VIEW_PROJ);
    f.inv_view_proj = muLoadMat4(o + F_INV_VIEW_PROJ);
    f.view          = muLoadMat4(o + F_VIEW);
    f.eye           = muScene.Load<float4>(o + F_EYE);
    f.forward       = muScene.Load<float4>(o + F_FORWARD);
    f.proj          = muScene.Load<float4>(o + F_PROJ);
    f.size          = muScene.Load<float4>(o + F_SIZE);
    f.grid          = muScene.Load<uint4>(o + F_GRID);
    f.data          = muScene.Load<uint4>(o + F_DATA);
    f.gbuffer       = muScene.Load<uint4>(o + F_GBUFFER);
    f.moon          = muScene.Load<float4>(o + F_MOON);
    return f;
}

struct Push
{
    uint vertex_offset, object_offset, frame_offset;
    uint view;    // fragLighting: 0 lit, 1 albedo, 2 normal, 3 roughness/metal, 4 depth,
                  //               5 lights per cluster, 6 clusters; --check: 7/8 normal packing error
                  //               (fragForward), 9 position, 10 normal error of the G-buffer's 16+16 bits
    uint brute;   // fragLighting: 1 = ignore the clusters, loop over every light
    uint pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

// ---- a thinner G-buffer, for comparison --------------------------------------
// 8 bytes: normal 12 + 12 bits and roughness in one word, metalness in the
// albedo's alpha. --check measures what the 12-bit normal costs (view 7).
// mu_lib's own recipe for such a word, mu_pack_nrm_rough_metal, is not
// usable: it packs the normal into all 32 bits (16 + 16) and then overwrites
// the upper 16 with the material bytes, losing the normal's second
// coordinate. --check measures that too (view 8).
uint packNormalRoughness(float3 n, float roughness)
{
    uint2 q = (uint2)(saturate(mu_oct_encode(n) * 0.5 + 0.5) * 4095.0 + 0.5);
    uint  r = (uint)(saturate(roughness) * 255.0 + 0.5);
    return q.x | (q.y << 12u) | (r << 24u);
}
float3 unpackNormal12(uint w)
{
    return mu_oct_decode(float2(w & 4095u, (w >> 12u) & 4095u) * (2.0 / 4095.0) - 1.0);
}

// ---- geometry ---------------------------------------------------------------
struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 color  : TEXCOORD2;
    [[vk::location(3)]] float4 params : TEXCOORD3;   // roughness, metalness
};

[shader("vertex")]
Varyings vertMain(uint vertexIndex : SV_VertexID)
{
    MeshVertex v = muLoadMeshVertex(pc.vertex_offset, vertexIndex);
    MeshObject o = muLoadMeshObject(pc.object_offset);
    Varyings r;
    r.world    = muMulPoint(o.model, v.position);
    r.normal   = muMulDir(o.model, v.normal);
    r.color    = o.color;
    r.params   = o.params;
    r.position = muMul(muLoadMat4(pc.frame_offset + F_VIEW_PROJ), float4(r.world, 1.0));
    return r;
}

float3 surfaceAlbedo(Varyings i)
{
    // A checker on the ground (upward faces at y = 0), so the pools of light
    // have something to fall on.
    float3 albedo = i.color.rgb;
    if (abs(i.world.y) < 1e-3 && i.normal.y > 0.9)
    {
        float2 g = floor(i.world.xz * 0.5);
        albedo *= lerp(0.7, 1.0, muMod(g.x + g.y, 2.0));
    }
    return albedo;
}

struct GBufferOut
{
    float4 albedo   : SV_Target0;
    uint   normal   : SV_Target1;
    float4 material : SV_Target2;
};

[shader("fragment")]
GBufferOut fragGBuffer(Varyings i)
{
    GBufferOut o;
    o.albedo   = float4(surfaceAlbedo(i), 1.0);
    o.normal   = mu_pack_normal_oct16(normalize(i.normal));
    o.material = float4(i.params.x, i.params.y, 0.0, 0.0);
    return o;
}

// ---- lighting ---------------------------------------------------------------
static const float LAMP_RADIUS = 0.25;

uint clusterOf(FrameData f, float2 pixel, float depth)
{
    // Tiles are laid out in NDC, where +y is up; pixel rows run downwards.
    float2 ndc01 = float2(pixel.x / f.size.x, 1.0 - pixel.y / f.size.y);
    uint2  tile  = min((uint2)(ndc01 * float2(f.grid.xy)), f.grid.xy - 1u);
    uint   slice = clusterSlice(depth, f.proj.z, f.proj.w, f.grid.z);
    return tile.x + tile.y * f.grid.x + slice * f.grid.x * f.grid.y;
}

// Every light of the pixel's cluster - or, with `brute`, every light there
// is. Both walk the lights in ascending index order and skip the ones out of
// range, so the two sums add the same numbers in the same order.
float3 shade(FrameData f, float3 albedo, float3 n, float roughness, float metalness, float3 world,
             float2 pixel, bool brute)
{
    float3 v = normalize(f.eye.xyz - world);
    MuSurface s = mu_surface(albedo, n, v, max(roughness, 0.08), metalness);

    uint base  = f.data.z + clusterOf(f, pixel, dot(world - f.eye.xyz, f.forward.xyz)) * (f.grid.w + 1u) * 4u;
    uint count = brute ? f.data.y : min(muScene.Load<uint>(base), f.grid.w);

    float3 color = albedo * f.forward.w * (0.6 + 0.4 * n.y)          // ambient
                 + mu_pbr_direct(s, f.moon.xyz, float3(0.55, 0.65, 1.0) * f.moon.w);
    for (uint k = 0u; k < count; ++k)
    {
        uint   index = brute ? k : muScene.Load<uint>(base + 4u + k * 4u);
        float4 pr    = muScene.Load<float4>(f.data.x + index * LIGHT_SIZE);
        float3 to    = pr.xyz - world;
        float  d2    = dot(to, to);
        if (d2 >= pr.w * pr.w) continue;                             // out of range: contributes exactly 0
        // 1/d^2 has no ceiling: a point light a centimetre off a wall is a
        // white-hot dot whose size depends on the last bits of the position.
        // Real lamps have a size; closer than LAMP_RADIUS the falloff stops.
        float3 radiance = muScene.Load<float4>(f.data.x + index * LIGHT_SIZE + 16u).rgb
                        * mu_point_attenuation(max(d2, LAMP_RADIUS * LAMP_RADIUS), pr.w);
        color += mu_pbr_direct(s, to * rsqrt(max(d2, 1e-8)), radiance);
    }
    return color;
}

float3 heat(float t)
{
    t = saturate(t);
    return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}

[shader("fragment")]
float4 fragLighting(FullscreenVaryings i) : SV_Target0
{
    FrameData f = loadFrame(pc.frame_offset);
    int3   texel = int3((int2)i.position.xy, 0);
    float  depth = muDepthImages[NonUniformResourceIndex(f.gbuffer.z)].Load(texel);
    if (depth == f.size.z)
    {
        // Sky: nothing was drawn here.
        float up = 1.0 - i.position.y / f.size.y;
        return float4(lerp(float3(0.010, 0.012, 0.022), float3(0.002, 0.003, 0.008), up), 0.0);
    }
    float3 albedo = muSampledImages[NonUniformResourceIndex(f.gbuffer.x)].Load(texel).rgb;
    float3 n      = mu_unpack_normal_oct16(muUintImages[NonUniformResourceIndex(f.gbuffer.y)].Load(texel));
    float2 rm     = muSampledImages[NonUniformResourceIndex(f.gbuffer.w)].Load(texel).rg;

    // Position from depth: un-project the pixel centre. NDC +y is up.
    float2 ndc   = float2(i.position.x / f.size.x * 2.0 - 1.0, 1.0 - i.position.y / f.size.y * 2.0);
    float4 h     = muMul(f.inv_view_proj, float4(ndc, depth, 1.0));
    float3 world = h.xyz / h.w;
    float  viewDepth = dot(world - f.eye.xyz, f.forward.xyz);

    // The debug views are display colours: the host turns tone mapping off
    // for them and Present encodes to sRGB, so they are un-encoded here.
    float3 debug = float3(0.0);
    switch (pc.view)
    {
    case 1: return float4(albedo, 1.0);                       // albedo is a colour already
    case 2: debug = n * 0.5 + 0.5; break;
    case 3: debug = float3(rm, 0.0); break;
    case 4: debug = float3(viewDepth / 40.0); break;
    case 5:
    {
        // Lights listed for this pixel's cluster; magenta = list overflowed.
        uint count = muScene.Load<uint>(f.data.z + clusterOf(f, i.position.xy, viewDepth) * (f.grid.w + 1u) * 4u);
        debug = count > f.grid.w ? float3(1.0, 0.0, 1.0) : heat((float)count / 96.0);
        debug *= 0.45 + 0.55 * mu_linear_to_srgb(albedo);
        break;
    }
    case 6:
    {
        uint c = clusterOf(f, i.position.xy, viewDepth);
        debug = (mu_hash03(uint3(c, 11u, 3u)) * 0.6 + 0.4) * (0.4 + 0.6 * mu_linear_to_srgb(albedo));
        break;
    }
    // --check: the rebuilt position, to compare with the rasterizer's. A
    // half float cannot hold 20 m to a tenth of a millimetre, so it carries
    // the position modulo 1/16 m.
    case 9: return float4(frac(world * 16.0), 1.0);
    default: return float4(shade(f, albedo, n, rm.x, rm.y, world, i.position.xy, pc.brute != 0u), 1.0);
    }
    return float4(mu_srgb_to_linear(saturate(debug)), 1.0);
}

// The reference: no G-buffer, no clusters. Shades the interpolated surface
// with every light.
[shader("fragment")]
float4 fragForward(Varyings i) : SV_Target0
{
    FrameData f = loadFrame(pc.frame_offset);
    float3 n = normalize(i.normal);
    // --check: how far a normal moves through each packing, and the
    // position the lighting pass has to rebuild.
    if (pc.view == 7u) return float4(float3(length(unpackNormal12(packNormalRoughness(n, 0.5)) - n)), 1.0);
    if (pc.view == 10u) return float4(float3(length(mu_unpack_normal_oct16(mu_pack_normal_oct16(n)) - n)), 1.0);
    if (pc.view == 8u) return float4(float3(length(mu_unpack_nrm_rough_metal(mu_pack_nrm_rough_metal(n, 0.5, 0.5)) - n)), 1.0);
    if (pc.view == 9u) return float4(frac(i.world * 16.0), 1.0);
    return float4(shade(f, surfaceAlbedo(i), n, i.params.x, i.params.y, i.world,
                        i.position.xy, true), 1.0);
}
