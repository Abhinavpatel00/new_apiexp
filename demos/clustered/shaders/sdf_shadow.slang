// sdf_shadow.slang — shadows for any number of lights from one distance
// volume of the scene. Include only (after `import mu_gfx`).
//
// The scene is boxes, spheres and rings on a floor, so its distance function
// can be written down (sceneDistance). A compute pass samples it once into a
// 3D texture; shading then walks from the surface towards each light through
// that texture, with steps as long as the distance read, and keeps the
// narrowest gap it passes:
//
//     shadow = min over the ray of  k * distance(t) / t
//
// (Quilez, "Soft shadows in raymarched SDFs"; the same cone test as Claybook's
// and Unreal's distance-field shadows.) k = distance to the light / radius of
// the light: the cone that sees the whole lamp. No shadow map per light.
//
// sceneDistance is also the reference: --check walks the same rays through
// the exact function in small steps and compares.
#include "shaders/sdf.slang"
#include "mu_volume.slang"

static const uint  SHAPE_SIZE = 48u;   // float4 centre + bounding radius; cos, sin, kind, 0; three sizes, 0
static const uint3 SDF_DIMS   = uint3(288u, 32u, 288u);

struct SdfInfo
{
    float3 origin;        // corner of voxel (0, 0, 0)
    float  voxel;         // metres
    uint   volume, sampler, shapes, count;
    float  lamp;          // radius of a lamp, metres
    float  moon_k;        // 1 / tan(angular radius of the moon)
    float  bias;          // start this many voxels off the surface, along the normal
};

SdfInfo sdfInfo(uint frame)
{
    SdfInfo s;
    float4 box    = muScene.Load<float4>(frame + F_SDF_BOX);
    uint4  data   = muScene.Load<uint4>(frame + F_SDF_DATA);
    float4 params = muScene.Load<float4>(frame + F_SDF_PARAMS);
    s.origin = box.xyz; s.voxel = box.w;
    s.volume = data.x; s.sampler = data.y; s.shapes = data.z; s.count = data.w;
    s.lamp = params.x; s.moon_k = params.y; s.bias = params.z;
    return s;
}

// Exact distance to the nearest surface (negative inside): the floor y = 0
// and every shape. kind 0 box (half sizes), 1 sphere (radius), 2 ring lying
// flat (major, minor radius).
float sceneDistance(SdfInfo s, float3 p)
{
    float d = p.y;
    for (uint i = 0u; i < s.count; ++i)
    {
        uint   base   = s.shapes + i * SHAPE_SIZE;
        float4 centre = muScene.Load<float4>(base);
        float3 q      = p - centre.xyz;
        if (length(q) - centre.w >= d) continue;       // its bounding sphere is already further
        float4 turn = muScene.Load<float4>(base + 16u);
        float3 size = muScene.Load<float4>(base + 32u).xyz;
        // World to the shape's own axes: the inverse of demo_object()'s yaw.
        float3 l = float3(turn.x * q.x - turn.y * q.z, q.y, turn.y * q.x + turn.x * q.z);
        float  shape = turn.z < 0.5 ? mu_sd_box(l, size)
                     : (turn.z < 1.5 ? length(l) - size.x : mu_sd_torus(l, size.xy));
        d = min(d, shape);
    }
    return d;
}

float volumeDistance(SdfInfo s, float3 p)
{
    float3 uvw = (p - s.origin) / (s.voxel * float3(SDF_DIMS));
    return muSampledVolumes[NonUniformResourceIndex(s.volume)]
        .SampleLevel(muSamplers[NonUniformResourceIndex(s.sampler)], uvw, 0.0).r;
}

// Walks from `from` along `dir` for `len` metres, inside the volume's box.
// x: the smallest distance met (negative: how deep the ray went into
//    something); y: the shadow term, 0 dark .. 1 lit.
// exact: the function instead of the texture. fine: 1 cm steps and no early
// exit, for measuring; otherwise at most 40 steps of at least half a voxel.
float2 sdfRay(SdfInfo s, float3 from, float3 dir, float len, float k, bool exact, bool fine)
{
    float3 lo  = s.origin + 0.5 * s.voxel, hi = s.origin + (float3(SDF_DIMS) - 0.5) * s.voxel;
    float3 inv = 1.0 / select(abs(dir) < 1e-6, float3(1e-6), dir);
    float3 a   = (lo - from) * inv, b = (hi - from) * inv;
    float3 near = min(a, b), far = max(a, b);
    float  t    = max(max(near.x, max(near.y, near.z)), 0.0);
    float  end  = min(min(far.x, min(far.y, far.z)), len);
    float  smallest = 1e9, soft = 1.0;
    float  min_step = fine ? 0.01 : 0.5 * s.voxel;
    uint   steps    = fine ? 600u : 40u;
    for (uint i = 0u; i < steps && t < end; ++i)
    {
        float3 p = from + dir * t;
        float  d = exact ? sceneDistance(s, p) : volumeDistance(s, p);
        smallest = min(smallest, d);
        soft     = min(soft, k * d / max(t, 1e-3));
        if (!fine && d <= 0.0) return float2(smallest, 0.0);
        t += clamp(abs(d), min_step, 1.0);
    }
    soft = saturate(soft);
    return float2(smallest, soft * soft * (3.0 - 2.0 * soft));
}
