// bake_weather.slang — the weather map, written by compute and re-written when
// the weather actually changes.
//
//   compWeather   512 x 512 RGBA8, one texel per 96 m of world, tiling the
//                 world on FS_WEATHER_TILE and scrolled by the wind at march
//                 time
//
//   R coverage      how much of the sky this texel fills, 0 = clear
//   G cloud type    0 = stratus, a flat grey deck; 1 = cumulonimbus, tall
//   B precipitation how much of the deck is raining, darkens the base
//   A twist         a per-texel offset into the detail volume
//
// Two things make this cheap. The per-frame motion is not here at all: the march
// adds wind * time to the sample coordinate, so a static map already produces
// weather that moves. And the slow evolution — the cells drifting, a front
// building — is a re-bake throttled to a few hertz, not a per-frame cost.
//
// Coverage is a periodic fbm remapped by a single CPU slider, so "clear" to
// "overcast" is one number rather than a re-authoring. The cells on top are the
// dynamic part: the flight code advects them with the wind, spawns them at the
// map's leading edge and retires them at the trailing one, and this pass turns
// that table into the field the march reads.
#include "fs_common.slang"

import mu_gfx;

// A storm cell, 16 bytes. The CPU owns the table and this pass only reads it.
struct WeatherCell
{
    float4 center;    // x world X, y world Z, z radius in metres, w intensity 0..1
};

struct WeatherPush
{
    uint4  dims;     // x width, y height, z output bindless index, w cell count
    uint4  cells;    // x byte offset of the cell table, y stride, z unused, w unused
    float4 params;   // x coverage, y type bias, z precipitation, w detail twist
    float4 lattice;  // x period X, y period Y, z octaves, w gain
};
[[vk::push_constant]] ConstantBuffer<WeatherPush> pc;

[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> fsWeatherOut[];

[numthreads(8, 8, 1)]
void compWeather(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.dims.xy)) return;

    // Texel centre in map space [0,1) and in lattice units.
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.dims.xy);
    float2 p  = uv * float2(pc.lattice.xy);
    int2   per = int2(pc.lattice.xy);
    int    oct = int(pc.lattice.z);

    // Three uncorrelated fields from one hash family. The offsets are large
    // enough that the lattices do not line up, which is what stops coverage and
    // type from producing the same blobs in the same places.
    float  raw_cov  = fsPeriodicFbm2(p, per, oct, pc.lattice.w);
    float  raw_type = fsPeriodicFbm2(p + float2(137.3, 41.9), per * 2, oct, pc.lattice.w);
    float  raw_twist = fsPeriodicFbm2(p + float2(271.1, 313.7), per * 3, 3, pc.lattice.w);

    // Coverage: one slider from clear to overcast. Remapping against the
    // slider instead of scaling it keeps the cloud *edges* soft at every
    // setting; scaling would turn "less cloud" into "thinner cloud".
    float coverage = fsRemap(raw_cov * 0.5 + 0.5, 1.0 - pc.params.x, 1.0);
    coverage       = pow(coverage, 1.15);

    // Type: stratus where the deck is thin and low, cumulus where it is thick.
    // Correlated with coverage so a heavy cell is also a tall one, then biased.
    float type     = saturate(raw_type * 0.5 + 0.5 + coverage * 0.35 - 0.18 + pc.params.y);

    float precip   = saturate(fsRemap(coverage, 0.45, 0.95) * pc.params.z * (0.4 + 0.6 * type));
    float twist    = raw_twist * pc.params.w;

    // Storm cells. The map tiles, so the distance to a cell is the wrapped one:
    // a cell whose centre sits just off the right edge still has to build its
    // anvil over the left edge, or the player watches weather pop into
    // existence at a fixed line in the world.
    uint count = pc.dims.w;
    for (uint i = 0u; i < count; ++i) {
        WeatherCell c   = muScene.Load<WeatherCell>(pc.cells.x + i * pc.cells.y);
        float2 center_uv = float2(c.center.x, c.center.y) / FS_WEATHER_TILE;
        float2 d         = uv - center_uv;
        d                = (d - floor(d + 0.5)) * FS_WEATHER_TILE;   // wrapped, in metres
        float  dist      = length(d);
        float  w         = saturate(1.0 - dist / max(c.center.z, 1.0));
        w                = w * w * (3.0 - 2.0 * w) * c.center.w;     // smoothstep by intensity
        coverage         = saturate(coverage + w);
        type             = lerp(type, 1.0, w * 0.85);                // a cell is a towering one
        precip           = saturate(precip + w * 0.7);
    }

    fsWeatherOut[NonUniformResourceIndex(pc.dims.z)][id.xy] = float4(coverage, type, precip, twist * 0.5 + 0.5);
}
