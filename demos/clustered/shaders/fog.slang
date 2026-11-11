// fog.slang — fog lit by the clustered lights, through a view-aligned volume.
//
//   compInject      every froxel: extinction at its centre and the light scattered
//                   towards the eye there (moon with its shadow from the distance
//                   volume, ambient, and the lamps of the froxel's tile and depth bin)
//   compIntegrate   every column, front to back: light gathered and share of the
//                   background left, up to the far face of each slice
//   fragFog         one filtered fetch at the pixel's depth, laid over the picture;
//                   or (mode 2) the same medium marched per pixel, as the reference
//
// Froxels (Wronski, "Volumetric fog", SIGGRAPH 2014) with the slice integral that
// stays correct in thick fog (Hillaire, "Physically based and unified volumetric
// rendering in Frostbite", SIGGRAPH 2015). Slices are spaced quadratically in view
// depth: slice face z lies at far * (z / 64)^2.
import mu_gfx;
#include "shaders/lighting.slang"
#include "clustered_common.slang"
#include "sdf_shadow.slang"

struct Push { uint frame_offset, pad0, pad1, pad2; };
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct Fog
{
    float3 eye, forward, right, up;   // right and up already scaled by tan(half fov)
    float  density, height, far, ambient, g, albedo, znear, zfar;
    uint   injected, integrated, mode, output;
    float2 size;
};

Fog fogInfo(uint o)
{
    Fog    f;
    float4 proj = muScene.Load<float4>(o + F_PROJ), a = muScene.Load<float4>(o + F_FOG), b = muScene.Load<float4>(o + F_FOG_PARAMS);
    uint4  data = muScene.Load<uint4>(o + F_FOG_DATA);
    f.eye     = muScene.Load<float4>(o + F_EYE).xyz;
    f.forward = muScene.Load<float4>(o + F_FORWARD).xyz;
    f.right   = muScene.Load<float4>(o + F_RIGHT).xyz * proj.x;
    f.up      = muScene.Load<float4>(o + F_UP).xyz * proj.y;
    f.density = a.x; f.height = a.y; f.far = a.z; f.ambient = a.w;
    f.g = b.x; f.albedo = b.y; f.znear = b.z; f.zfar = b.w;
    f.injected = data.x; f.integrated = data.y; f.mode = data.z; f.output = data.w;
    f.size = muScene.Load<float4>(o + F_SIZE).xy;
    return f;
}

// The ray through a pixel, with a forward component of 1: eye + ray * view depth.
float3 fogRay(Fog f, float2 pixel)
{
    float2 ndc = float2(pixel.x / f.size.x * 2.0 - 1.0, 1.0 - pixel.y / f.size.y * 2.0);
    return f.forward + ndc.x * f.right + ndc.y * f.up;
}

float fogExtinction(Fog f, float3 p) { return f.density * (f.height > 0.0 ? exp(-max(p.y, 0.0) / f.height) : 1.0); }

// Henyey-Greenstein times 4 pi: 1 for g = 0, so that the light's colour is what an
// isotropic fog of albedo 1 scatters per unit of optical depth.
float fogPhase(float g, float c)
{
    float k = 1.0 + g * g - 2.0 * g * c;
    return (1.0 - g * g) / (k * sqrt(k));
}

// Light scattered towards the eye at p, before albedo and extinction. The lamps
// are those of p's screen tile and depth bin (or all of them); only the moon is
// shadowed.
float3 fogLight(uint o, Fog f, SdfInfo sdf, float3 p, float2 pixel, bool brute)
{
    float4 proj  = muScene.Load<float4>(o + F_PROJ);
    float4 size  = muScene.Load<float4>(o + F_SIZE);
    uint4  grid  = muScene.Load<uint4>(o + F_GRID);
    uint4  data  = muScene.Load<uint4>(o + F_DATA);
    uint4  data2 = muScene.Load<uint4>(o + F_DATA2);
    float4 moon  = muScene.Load<float4>(o + F_MOON);
    float3 toEye = normalize(f.eye - p);
    float3 light = float3(0.55, 0.65, 1.0) * f.ambient;
    if (moon.w > 0.0)
    {
        float3 dir = normalize(moon.xyz);
        light += float3(0.55, 0.65, 1.0) * (moon.w * fogPhase(f.g, dot(-dir, toEye))
                                           * sdfRay(sdf, p, dir, 1e3, sdf.moon_k, false, false).y);
    }
    if (data.y == 0u) return light;

    uint2 tile  = min((uint2)(pixel / size.z), grid.xy - 1u);
    uint  masks = data.z + (tile.x + tile.y * grid.x) * grid.z * 4u;
    uint  bin   = min((uint)(max(dot(p - f.eye, f.forward), 0.0) * proj.z), grid.w - 1u);
    uint2 range = brute ? uint2(0u, data.y - 1u) : muScene.Load<uint2>(data.w + bin * 8u);
    if (range.x > range.y) return light;
    for (uint w = range.x >> 5u; w <= range.y >> 5u; ++w)
    {
        uint mask = brute ? 0xffffffffu : muScene.Load<uint>(masks + w * 4u);
        mask = maskRange(mask, range, w * 32u);
        uint spots = muScene.Load<uint>(data2.y + w * 4u);
        while (mask != 0u)
        {
            uint bit = firstbitlow(mask);
            mask &= mask - 1u;
            uint   l  = data.x + (w * 32u + bit) * LIGHT_SIZE;
            float4 pr = muScene.Load<float4>(l);
            float3 to = pr.xyz - p;
            float  d2 = dot(to, to);
            if (d2 >= pr.w * pr.w) continue;
            float3 dir      = to * rsqrt(max(d2, 1e-8));
            float4 colour   = muScene.Load<float4>(l + 16u);
            float3 radiance = colour.rgb * mu_point_attenuation(max(d2, LAMP_RADIUS * LAMP_RADIUS), pr.w);
            if ((spots & (1u << bit)) != 0u)
            {
                float4 axis = muScene.Load<float4>(l + 32u);
                float  c    = dot(-dir, axis.xyz);
                if (c <= axis.w) continue;
                radiance *= mu_spot_attenuation(c, axis.w, colour.w);
            }
            light += radiance * fogPhase(f.g, dot(-dir, toEye));
        }
    }
    return light;
}

// One stretch of `length` metres with constant extinction and light: adds what it
// scatters towards the eye, then dims what lies behind it.
void fogStep(inout float3 gathered, inout float through, float3 scattering, float extinction, float length)
{
    float t = exp(-extinction * length);
    gathered += through * scattering * (extinction > 1e-6 ? (1.0 - t) / extinction : length);
    through  *= t;
}

float sliceDepth(Fog f, float slice) { float s = slice / float(FROXELS.z); return f.far * s * s; }

[shader("compute")]
[numthreads(4, 4, 4)]
void compInject(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= FROXELS)) return;
    uint    o   = pc.frame_offset;
    Fog     f   = fogInfo(o);
    SdfInfo sdf = sdfInfo(o);
    float2  pixel = (float2(id.xy) + 0.5) / float2(FROXELS.xy) * f.size;
    float3  p     = f.eye + fogRay(f, pixel) * sliceDepth(f, float(id.z) + 0.5);
    float   e     = fogExtinction(f, p);
    muStorageVolumes[NonUniformResourceIndex(f.injected)][id] =
        float4(fogLight(o, f, sdf, p, pixel, false) * (f.albedo * e), e);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compIntegrate(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FROXELS.xy)) return;
    Fog    f = fogInfo(pc.frame_offset);
    float2 pixel = (float2(id.xy) + 0.5) / float2(FROXELS.xy) * f.size;
    float  len = length(fogRay(f, pixel));
    float3 gathered = float3(0.0);
    float  through  = 1.0;
    for (uint z = 0u; z < FROXELS.z; ++z)
    {
        float4 v = muSampledVolumes[NonUniformResourceIndex(f.injected)].Load(int4(id.xy, z, 0));
        fogStep(gathered, through, v.rgb, v.a, (sliceDepth(f, float(z + 1u)) - sliceDepth(f, float(z))) * len);
        muStorageVolumes[NonUniformResourceIndex(f.integrated)][uint3(id.xy, z)] = float4(gathered, through);
    }
}

[shader("fragment")]
float4 fragFog(FullscreenVaryings i) : SV_Target0
{
    uint   o     = pc.frame_offset;
    Fog    f     = fogInfo(o);
    uint4  scene = muScene.Load<uint4>(o + F_FOG_SCENE);
    int2   px    = int2(i.position.xy);
    float4 hdr   = muSampledImages[NonUniformResourceIndex(scene.x)].Load(int3(px, 0));
    if (f.mode == 0u) return hdr;

    // Reversed depth: 0 is the far plane.
    float  depth = muSampledImages[NonUniformResourceIndex(scene.y)].Load(int3(px, 0)).r;
    float  reach = min(f.zfar * f.znear / (depth * (f.zfar - f.znear) + f.znear), f.far);
    float3 ray   = fogRay(f, i.position.xy);
    float3 gathered = float3(0.0);
    float  through  = 1.0;
    if (f.mode == 1u)
    {
        // Texel z holds the state at the far face of slice z, which is slice coordinate z + 1.
        SdfInfo sdf = sdfInfo(o);
        float   s   = float(FROXELS.z) * sqrt(reach / f.far);
        float4  v   = muSampledVolumes[NonUniformResourceIndex(f.integrated)].SampleLevel(
            muSamplers[NonUniformResourceIndex(sdf.sampler)],
            float3(i.position.xy / f.size, (max(s, 1.0) - 0.5) / float(FROXELS.z)), 0.0);
        if (s < 1.0) v = lerp(float4(0.0, 0.0, 0.0, 1.0), v, s);   // inside the first slice: from "no fog yet"
        gathered = v.rgb; through = v.a;
    }
    else
    {
        uint4 r = muScene.Load<uint4>(o + F_FOG_REF);              // steps, every n-th pixel, every light
        if (r.y > 1u && (uint(px.x) % r.y != 0u || uint(px.y) % r.y != 0u)) return float4(0.0);
        SdfInfo sdf = sdfInfo(o);
        float   len = length(ray), dz = reach / float(r.x);
        for (uint k = 0u; k < r.x; ++k)
        {
            float3 p = f.eye + ray * (dz * (float(k) + 0.5));
            float  e = fogExtinction(f, p);
            fogStep(gathered, through, fogLight(o, f, sdf, p, i.position.xy, r.z != 0u) * (f.albedo * e), e, dz * len);
        }
    }
    if (f.output == 1u) return float4(gathered.r, through, reach * length(ray), 1.0);   // --check
    if (f.output == 2u) return float4(gathered, through);                                // --check
    return float4(hdr.rgb * through + gathered, hdr.a);
}
