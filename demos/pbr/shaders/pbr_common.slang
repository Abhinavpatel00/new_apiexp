// pbr_common.slang — shared by bake.slang and pbr.slang.
//
// Environment maps are octahedral: the whole sphere of directions in one
// square 2D target, because vk.h has neither cube targets nor per-mip
// writes. One target per roughness level:
//
//   level 0          the sky itself (256^2)
//   levels 1..4      GGX-prefiltered for roughness 0.25, 0.5, 0.75, 1 (128^2 .. 32^2)
//   irradiance       cosine-weighted mean radiance, i.e. E / pi (32^2)
//   DFG              split-sum scale and bias over (NoV, roughness) (64^2)
#include "shaders/lighting.slang"

static const uint ENV_LEVELS = 5u;

// Direction -> octahedral uv. +Y is the centre of the square; the fold (the
// square's edge) is the horizon's far side, straight down at the corners.
float2 octEncode(float3 d)
{
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    float2 p = d.xz;
    if (d.y < 0.0)
        p = (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}

float3 octDecode(float2 uv)
{
    float2 p = uv * 2.0 - 1.0;
    float3 d = float3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    float  t = max(-d.y, 0.0);
    d.x += d.x >= 0.0 ? -t : t;
    d.z += d.z >= 0.0 ? -t : t;
    return normalize(d);
}

// The environment. `mode` 0 is the sky; 1 and 2 are for --check.
// The sun's disc is not part of it: the sun is an analytic directional
// light, and a 30000-nit dot in a 256^2 map would only prefilter into noise.
float3 skyRadiance(float3 dir, float3 sun, uint mode)
{
    if (mode == 1u) return float3(1.0);                       // white furnace
    if (mode == 2u) return float3(dir.y > 0.0 ? 1.0 : 0.0);   // white above, black below

    float  up      = dir.y;
    float3 zenith  = float3(0.05, 0.14, 0.42);
    float3 horizon = float3(0.38, 0.47, 0.60);
    float3 sky     = lerp(horizon, zenith, pow(saturate(up), 0.45));
    // Warm, wide glow around the sun; stronger near the horizon.
    float  toSun   = saturate(dot(dir, sun));
    sky += float3(1.0, 0.72, 0.42) * (0.35 * pow(toSun, 6.0) + 1.4 * pow(toSun, 90.0));
    // Ground: a lit, slightly warm plane fading into the horizon haze.
    float3 ground  = float3(0.15, 0.13, 0.11) * (0.35 + 0.65 * saturate(sun.y));
    float3 below   = lerp(horizon * 0.75, ground, saturate(-up * 6.0));
    return up >= 0.0 ? sky : below;
}

// Hammersley point i of n.
float2 hammersley(uint i, uint n)
{
    uint b = i;
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return float2(((float)i + 0.5) / (float)n, (float)b * 2.3283064365386963e-10);
}
