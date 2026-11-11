// bake.slang — the four compute passes that build the image-based lighting.
//
//   compSky         procedural sky -> level 0
//   compPrefilter   level 0 -> one GGX-prefiltered level (run once per level)
//   compIrradiance  level 0 -> cosine-convolved map
//   compDfg         the split-sum BRDF table; depends on nothing
//
// Every pass writes one RGBA16F storage image of the bindless table.
#include "pbr_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> muSampledImages[];
[[vk::binding(2, 0)]] SamplerState      muSamplers[];
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> muStorageImages[];

struct BakePush
{
    uint   source;      // bindless index of level 0 (sampled)
    uint   sampler;     // linear clamp
    uint   target;      // bindless index of the image written
    uint   size;        // its width = height
    float  roughness;   // compPrefilter
    uint   samples;
    uint   mode;        // skyRadiance mode
    uint   pad;
    float4 sun;         // xyz direction to the sun
};
[[vk::push_constant]] ConstantBuffer<BakePush> pc;

float3 environment(float3 dir)
{
    return muSampledImages[NonUniformResourceIndex(pc.source)]
        .SampleLevel(muSamplers[NonUniformResourceIndex(pc.sampler)], octEncode(dir), 0.0).rgb;
}

void store(uint2 texel, float4 value)
{
    muStorageImages[NonUniformResourceIndex(pc.target)][texel] = value;
}

float3 texelDirection(uint2 texel)
{
    return octDecode(((float2)texel + 0.5) / (float)pc.size);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compSky(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size)) return;
    store(id.xy, float4(skyRadiance(texelDirection(id.xy), pc.sun.xyz, pc.mode), 1.0));
}

// Split-sum, first half: the environment seen through a GGX lobe, assuming
// view = normal = reflection direction (Karis 2013). Weighted by NoL.
[shader("compute")]
[numthreads(8, 8, 1)]
void compPrefilter(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size)) return;
    float3 n     = texelDirection(id.xy);
    float  alpha = mu_roughness_to_alpha(pc.roughness);
    float3 sum   = float3(0.0);
    float  weight = 0.0;
    for (uint i = 0u; i < pc.samples; ++i)
    {
        float3 h   = mu_tangent_to_world(n, mu_sample_ggx(hammersley(i, pc.samples), alpha));
        float3 l   = 2.0 * dot(n, h) * h - n;
        float  nol = dot(n, l);
        if (nol > 0.0)
        {
            sum    += environment(l) * nol;
            weight += nol;
        }
    }
    store(id.xy, float4(sum / max(weight, 1e-4), 1.0));
}

// Mean radiance over the cosine-weighted hemisphere = irradiance / pi.
[shader("compute")]
[numthreads(8, 8, 1)]
void compIrradiance(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size)) return;
    float3 n   = texelDirection(id.xy);
    float3 sum = float3(0.0);
    for (uint i = 0u; i < pc.samples; ++i)
        sum += environment(mu_tangent_to_world(n, mu_sample_cosine_hemisphere(hammersley(i, pc.samples))));
    store(id.xy, float4(sum / (float)pc.samples, 1.0));
}

// Split-sum, second half: integral of the specular BRDF over the hemisphere
// for F0 = 1 split as  F0 * r + g.  x = NoV, y = perceptual roughness.
[shader("compute")]
[numthreads(8, 8, 1)]
void compDfg(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size)) return;
    float  nov   = ((float)id.x + 0.5) / (float)pc.size;
    float  alpha = mu_roughness_to_alpha(((float)id.y + 0.5) / (float)pc.size);
    float3 v     = float3(sqrt(1.0 - nov * nov), 0.0, nov);
    float2 sum   = float2(0.0);
    for (uint i = 0u; i < pc.samples; ++i)
    {
        float3 h   = mu_sample_ggx(hammersley(i, pc.samples), alpha);
        float  voh = dot(v, h);
        float3 l   = 2.0 * voh * h - v;
        if (l.z > 0.0 && voh > 0.0)
        {
            // BRDF * NoL / pdf, with pdf = D * NoH / (4 VoH): D cancels.
            float g  = mu_v_smith_ggx(nov, l.z, alpha) * 4.0 * voh * l.z / max(h.z, 1e-6);
            float fc = mu_pow5(1.0 - voh);
            sum += float2(1.0 - fc, fc) * g;
        }
    }
    store(id.xy, float4(sum / (float)pc.samples, 0.0, 1.0));
}
