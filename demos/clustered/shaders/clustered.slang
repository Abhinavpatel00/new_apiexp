// clustered.slang — forward shading with tile bitmasks and z bins.
//
//   vertMain      geometry (also the depth pre-pass, which has no fragment stage)
//   fragForward   lights of this pixel's tile AND depth bin; with `brute`,
//                 every light; optionally a shadow ray per light through
//                 the scene's distance volume (sdf_shadow.slang)
import mu_gfx;
import mu_mesh;
#include "shaders/lighting.slang"
#include "shaders/format.slang"
#include "clustered_common.slang"
#include "sdf_shadow.slang"

struct Push
{
    uint vertex_offset, object_offset, frame_offset;
    uint view;    // 0 lit, 1 lights tested per pixel, 2 lights in the tile's mask, 3 lights in the
                  // z bin's range, 4 tested but out of range; --check: 5 raw counts, 6 one light's
                  // shadow ray through the volume, 7 through the exact function, 8 distance at the surface
    uint brute;   // 1 = ignore masks and bins
    uint pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

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

float3 heat(float t)
{
    t = saturate(t);
    return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}

[shader("fragment")]
float4 fragForward(Varyings i) : SV_Target0
{
    uint   o       = pc.frame_offset;
    float3 eye     = muScene.Load<float4>(o + F_EYE).xyz;
    float4 forward = muScene.Load<float4>(o + F_FORWARD);   // xyz, w ambient
    float4 proj    = muScene.Load<float4>(o + F_PROJ);      // tan x, tan y, bins per metre
    float4 size    = muScene.Load<float4>(o + F_SIZE);      // width, height, tile size
    uint4  grid    = muScene.Load<uint4>(o + F_GRID);       // tiles x, tiles y, words per tile, z bins
    uint4  data    = muScene.Load<uint4>(o + F_DATA);       // lights, light count, tile masks, z bins
    uint4  data2   = muScene.Load<uint4>(o + F_DATA2);      // cull spheres, types
    float4 moon    = muScene.Load<float4>(o + F_MOON);
    bool   brute   = pc.brute != 0u;

    float3 albedo = i.color.rgb;
    if (abs(i.world.y) < 1e-3 && i.normal.y > 0.9)
    {
        float2 g = floor(i.world.xz * 0.5);
        albedo *= lerp(0.7, 1.0, muMod(g.x + g.y, 2.0));
    }
    float3 n = normalize(i.normal);
    float3 v = normalize(eye - i.world);
    MuSurface s = mu_surface(albedo, n, v, max(i.params.x, 0.08), i.params.y);

    uint2 tile  = min((uint2)(i.position.xy / size.z), grid.xy - 1u);
    uint  masks = data.z + (tile.x + tile.y * grid.x) * grid.z * 4u;
    uint  bin   = min((uint)(max(dot(i.world - eye, forward.xyz), 0.0) * proj.z), grid.w - 1u);
    // Lowest and highest light index that reaches this depth bin. The lights
    // are sorted by depth, so the range is short. Empty bin: x > y.
    uint2 range = brute ? uint2(0u, data.y - 1u) : muScene.Load<uint2>(data.w + bin * 8u);

    uint4   ctl     = muScene.Load<uint4>(o + F_SDF_CTL);  // shadows on, light of views 6 and 7
    bool    shadows = ctl.x != 0u;
    SdfInfo sdf     = (SdfInfo)0;
    if (shadows || pc.view >= 6u) sdf = sdfInfo(o);
    float3  from    = i.world + n * (sdf.bias * sdf.voxel);

    float3 moonlight = mu_pbr_direct(s, moon.xyz, float3(0.55, 0.65, 1.0) * moon.w);
    if (shadows && dot(n, moon.xyz) > 0.0)
        moonlight *= sdfRay(sdf, from, normalize(moon.xyz), 1e3, sdf.moon_k, false, false).y;
    float3 color = albedo * forward.w * (0.6 + 0.4 * n.y) + moonlight;
    uint tested = 0u, reached = 0u;
    if (range.x <= range.y && data.y > 0u)
    {
        for (uint w = range.x >> 5u; w <= range.y >> 5u; ++w)
        {
            uint mask = brute ? 0xffffffffu : muScene.Load<uint>(masks + w * 4u);
            mask = maskRange(mask, range, w * 32u);
            uint spots = muScene.Load<uint>(data2.y + w * 4u);
            // The bit-scan loop: lowest set bit first, so lights are visited
            // in ascending order whichever way the mask was produced.
            while (mask != 0u)
            {
                uint bit = firstbitlow(mask);
                mask &= mask - 1u;
                uint   l  = data.x + (w * 32u + bit) * LIGHT_SIZE;
                float4 pr = muScene.Load<float4>(l);
                float3 to = pr.xyz - i.world;
                float  d2 = dot(to, to);
                ++tested;
                if (d2 >= pr.w * pr.w) continue;                   // out of range: exactly 0
                float3 dir      = to * rsqrt(max(d2, 1e-8));
                float3 radiance = muScene.Load<float4>(l + 16u).rgb
                                * mu_point_attenuation(max(d2, LAMP_RADIUS * LAMP_RADIUS), pr.w);
                if ((spots & (1u << bit)) != 0u)
                {
                    float4 axis = muScene.Load<float4>(l + 32u);   // xyz the way it points, w cos(outer)
                    float  c    = dot(-dir, axis.xyz);
                    if (c <= axis.w) continue;                     // outside the cone: exactly 0
                    radiance *= mu_spot_attenuation(c, axis.w, muScene.Load<float4>(l + 16u).w);
                }
                ++reached;
                if (shadows && dot(n, dir) > 0.0)
                {
                    float dist = sqrt(d2);
                    radiance *= sdfRay(sdf, from, dir, dist, dist / sdf.lamp, false, false).y;
                }
                color += mu_pbr_direct(s, dir, radiance);
            }
        }
    }

    float3 debug;
    switch (pc.view)
    {
    case 1: debug = heat((float)tested / 48.0); break;
    case 2:
    {
        uint bits = 0u;
        for (uint w = 0u; w < grid.z; ++w) bits += countbits(muScene.Load<uint>(masks + w * 4u));
        debug = heat((float)bits / 96.0);
        break;
    }
    case 3: debug = range.x <= range.y ? heat((float)(range.y - range.x + 1u) / 512.0) : float3(0.0); break;
    case 4: debug = heat((float)(tested - reached) / 48.0); break;
    case 5: return float4((float)tested, (float)reached, 0.0, 1.0);
    case 6:
    case 7:
    {
        // One light's shadow ray: smallest distance met, shadow term, and
        // whether this pixel faces the light and is in its reach.
        float3 dir = normalize(moon.xyz);
        float  len = 1e3, k = sdf.moon_k;
        bool   valid = true;
        if (ctl.y != 0xffffffffu)
        {
            float4 pr = muScene.Load<float4>(data.x + ctl.y * LIGHT_SIZE);
            float3 to = pr.xyz - i.world;
            len   = length(to);
            dir   = to / max(len, 1e-4);
            k     = len / sdf.lamp;
            valid = len < pr.w;
        }
        valid = valid && dot(n, dir) > 0.05;
        if (pc.view == 7u) return float4(sdfRay(sdf, from, dir, len, k, true, true), valid ? 1.0 : 0.0, 1.0);
        return float4(sdfRay(sdf, from, dir, len, k, false, true).x, sdfRay(sdf, from, dir, len, k, false, false).y,
                      valid ? 1.0 : 0.0, 1.0);
    }
    case 8: return float4(sceneDistance(sdf, i.world), 0.0, 0.0, 1.0);
    default: return float4(color, 1.0);
    }
    return float4(mu_srgb_to_linear(saturate(debug * (0.45 + 0.55 * mu_linear_to_srgb(albedo)))), 1.0);
}
