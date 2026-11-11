// pbr.slang — metallic/roughness shading with image-based lighting.
//
//   fragSky            background: the sky, or one of its prefiltered levels
//   vertMain/fragMain  the spheres: sun + split-sum IBL, clearcoat, sheen
//
// BRDF terms are external/mu_lib/mu/shaders/lighting.slang. The image-based
// part is assembled here, because the furnace test (--check) showed that
// mu_multi_scatter_fresnel over-brightens metals; see multiScatter() below.
import mu_gfx;
import mu_mesh;
#include "pbr_common.slang"

struct FrameData
{
    MuMat4 view_proj;
    float4 eye;
    float4 sun;        // xyz direction to the sun, w radiance scale (0 = off)
    float4 right;      // camera right * tan(fov/2) * aspect
    float4 up;         // camera up * tan(fov/2)
    float4 forward;    // xyz, w = background level 0..4 (0 = the sky itself)
    uint4  level0123;  // bindless indices of environment levels 0..3
    uint4  maps;       // level 4, irradiance, DFG, linear-clamp sampler
    uint4  options;    // x energy mode, y DFG source, z view, w flags
    float4 size;       // xy output size in pixels, z environment mode
};
static const uint MODE_SINGLE = 0u, MODE_MULTI = 1u, MODE_MU_LIB = 2u;
static const uint FLAG_FURNACE = 1u, FLAG_SHOW_MAPS = 2u;

FrameData loadFrame(uint o)
{
    FrameData f;
    f.view_proj = muLoadMat4(o);
    f.eye       = muScene.Load<float4>(o + 64u);
    f.sun       = muScene.Load<float4>(o + 80u);
    f.right     = muScene.Load<float4>(o + 96u);
    f.up        = muScene.Load<float4>(o + 112u);
    f.forward   = muScene.Load<float4>(o + 128u);
    f.level0123 = muScene.Load<uint4>(o + 144u);
    f.maps      = muScene.Load<uint4>(o + 160u);
    f.options   = muScene.Load<uint4>(o + 176u);
    f.size      = muScene.Load<float4>(o + 192u);
    return f;
}

struct DrawPush { uint vertex_offset, object_offset, frame_offset, object_id; };
[[vk::push_constant]] ConstantBuffer<DrawPush> pc;

float3 sampleMap(FrameData f, uint index, float3 dir)
{
    return muSampleLevel(index, f.maps.w, octEncode(dir), 0.0).rgb;
}

uint levelIndex(FrameData f, uint level) { return level < 4u ? f.level0123[level] : f.maps.x; }

// Prefiltered radiance for a roughness: the levels sit at 0, 0.25, ... 1.
float3 prefiltered(FrameData f, float3 dir, float roughness)
{
    float x  = saturate(roughness) * 4.0;
    uint  lo = min((uint)x, 3u);
    return lerp(sampleMap(f, levelIndex(f, lo), dir), sampleMap(f, levelIndex(f, lo + 1u), dir), x - (float)lo);
}

// ---- background -----------------------------------------------------------
[shader("fragment")]
float4 fragSky(FullscreenVaryings i) : SV_Target0
{
    FrameData f = loadFrame(pc.frame_offset);
    float2 ndc  = i.uv * 2.0 - 1.0;   // uv.y grows upwards on screen (negative-height viewport)
    float3 dir  = normalize(f.forward.xyz + f.right.xyz * ndc.x + f.up.xyz * ndc.y);
    float3 color;
    if (f.forward.w <= 0.0)
    {
        color = skyRadiance(dir, f.sun.xyz, (uint)f.size.z);
        // The disc the environment map leaves out.
        if ((uint)f.size.z == 0u) color += float3(1.0, 0.9, 0.75) * 40.0 * smoothstep(0.99985, 0.9999, dot(dir, f.sun.xyz));
    }
    else
        color = prefiltered(f, dir, f.forward.w * 0.25);

    // The baked maps as tiles along the bottom: levels 0..4, irradiance, DFG.
    if ((f.options.w & FLAG_SHOW_MAPS) != 0u)
    {
        const float tile = 80.0, gap = 8.0;
        float2 p = float2(i.position.x - gap, f.size.y - gap - i.position.y);   // from the bottom-left
        uint   k = (uint)(p.x / (tile + gap));
        float2 q = float2(p.x - (float)k * (tile + gap), p.y) / tile;
        if (p.x >= 0.0 && p.y >= 0.0 && k < 7u && q.x < 1.0 && q.y < 1.0)
        {
            uint index = k < 5u ? levelIndex(f, k) : k == 5u ? f.maps.y : f.maps.z;
            color = muSampleLevel(index, f.maps.w, float2(q.x, 1.0 - q.y), 0.0).rgb;
        }
    }
    return float4(color, 0.0);
}

// ---- spheres ----------------------------------------------------------------
struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 color  : TEXCOORD2;
    [[vk::location(3)]] float4 params : TEXCOORD3;   // roughness, metallic, clearcoat, sheen
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
    r.position = muMul(muLoadMat4(pc.frame_offset), float4(r.world, 1.0));
    return r;
}

// Energy of the specular lobe, split as (single scattering, multiple
// scattering). Fdez-Aguera 2019, "A Multiple-Scattering Microfacet Model for
// Real-Time Image-based Lighting":
//
//     FssEss = F0 * dfg.x + dfg.y            what one bounce reflects
//     Ems    = 1 - (dfg.x + dfg.y)           what one bounce loses
//     FmsEms = Ems * FssEss * Favg / (1 - Favg * Ems)
//
// mu_multi_scatter_fresnel has (1 - Ems) where the denominator has Ems. For
// F0 = 1 that returns 2 * Ess instead of 1 - a white metal sphere in a white
// room comes out up to twice as bright as the room.
void multiScatter(float3 f0, float2 dfg, out float3 single, out float3 multi)
{
    float  ess  = dfg.x + dfg.y;
    float  ems  = max(1.0 - ess, 0.0);
    float3 favg = f0 + (1.0 - f0) * (1.0 / 21.0);
    single = f0 * dfg.x + dfg.y;
    multi  = ems * single * favg / max(1.0 - favg * ems, 1e-4);
}

[shader("fragment")]
float4 fragMain(Varyings i) : SV_Target0
{
    FrameData f = loadFrame(pc.frame_offset);
    bool   furnace   = (f.options.w & FLAG_FURNACE) != 0u;
    float3 albedo    = furnace ? float3(1.0) : i.color.rgb;
    float  roughness = i.params.x;
    float  clearcoat = furnace ? 0.0 : i.params.z;
    float  sheen     = furnace ? 0.0 : i.params.w;

    float3 n = normalize(i.normal);
    float3 v = normalize(f.eye.xyz - i.world);
    MuSurface s = mu_surface(albedo, n, v, roughness, i.params.y);
    s.clearcoat           = clearcoat;
    s.clearcoat_roughness = 0.06;
    s.sheen_color         = float3(0.55, 0.5, 0.7) * sheen;
    s.sheen_roughness     = max(roughness, 0.25);

    float  nov = max(dot(n, v), 1e-4);
    float3 r   = reflect(-v, n);

    // ---- image-based ----
    float2 dfg = f.options.y == 0u
        ? muSampleLevel(f.maps.z, f.maps.w, float2(nov, s.roughness), 0.0).xy
        : mu_env_brdf_approx(nov, s.roughness);
    float3 radiance   = prefiltered(f, r, s.roughness);
    float3 irradiance = sampleMap(f, f.maps.y, n);            // already divided by pi

    float3 specular, diffuse;
    if (f.options.x == MODE_MU_LIB)
    {
        // The library's own indirect path, as is (analytic DFG fit inside).
        specular = mu_pbr_indirect(s, irradiance * MU_PI, radiance, 1.0, 1.0);
        diffuse  = float3(0.0);
    }
    else
    {
        // Metalness mixes two materials: a dielectric (F0 = 0.04) over a
        // diffuse base, and a metal (F0 = albedo). Evaluating the two lobes
        // separately keeps the mix exact; folding them into one F0 first
        // would let the diffuse base see the metal's reflectance.
        float3 single0, multi0, single1, multi1;
        multiScatter(float3(MU_F0_DIELECTRIC), dfg, single0, multi0);
        multiScatter(albedo, dfg, single1, multi1);
        if (f.options.x == MODE_SINGLE) { multi0 = float3(0.0); multi1 = float3(0.0); }
        // The multiply-scattered part has lost its direction: light it with
        // the cosine-convolved environment.
        float3 dielectric = single0 * radiance + multi0 * irradiance;
        float3 metal      = single1 * radiance + multi1 * irradiance;
        specular = lerp(dielectric, metal, s.metalness);
        // What the dielectric's lobe did not reflect reaches its diffuse base.
        diffuse  = s.diffuse_color * (1.0 - single0 - multi0) * irradiance;
    }

    // ---- sun ----
    float3 sunLight = float3(1.0, 0.94, 0.84) * f.sun.w;
    float3 direct   = mu_pbr_direct(s, f.sun.xyz, sunLight);

    // ---- layers ----
    // Clearcoat: a smooth dielectric film. What it reflects never reaches
    // the base.
    float  coatF  = clearcoat * (MU_F0_DIELECTRIC + (1.0 - MU_F0_DIELECTRIC) * mu_pow5(1.0 - nov));
    float3 coat   = coatF * prefiltered(f, r, s.clearcoat_roughness) + mu_clearcoat_brdf(s, f.sun.xyz) * sunLight;
    // Sheen: Charlie lobe for the sun; for the environment a grazing-angle
    // rim of the irradiance (an approximation - there is no Charlie DFG table).
    float3 fuzz   = mu_sheen_brdf(s, f.sun.xyz) * sunLight
                  + s.sheen_color * irradiance * (0.15 + 0.85 * mu_pow5(1.0 - nov));

    float3 color = (diffuse + specular + direct) * (1.0 - coatF) + coat + fuzz;
    switch (f.options.z)
    {
    case 1:  color = specular; break;
    case 2:  color = diffuse; break;
    case 3:  color = direct + mu_clearcoat_brdf(s, f.sun.xyz) * sunLight; break;
    case 4:  return float4(color, (float)(pc.object_id + 1u));   // --check
    default: break;
    }
    return float4(color, 1.0);
}
