// world.slang — drawing the volume.
//
//   fragPrepass   1/8 resolution: a safe start depth per 8 x 8 pixel block
//   fragTrace     full resolution: trace, shade, composite the falling balls
import mu_gfx;
#include "shaders/lighting.slang"
#include "shaders/format.slang"
#include "mu_volume.slang"
#include "world_common.slang"
#include "volume.slang"

struct Push { uint frame_offset, pad0, pad1, pad2; };
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct Frame
{
    float3 eye, right, up, forward;
    float4 sun, size;
    uint4  index, options, balls;
    Volume volume;
};

Frame loadFrame(uint o)
{
    Frame f;
    f.eye     = muScene.Load<float4>(o + F_EYE).xyz;
    f.right   = muScene.Load<float4>(o + F_RIGHT).xyz;     // times tan(half fov x)
    f.up      = muScene.Load<float4>(o + F_UP).xyz;        // times tan(half fov y)
    f.forward = muScene.Load<float4>(o + F_FORWARD).xyz;
    f.sun     = muScene.Load<float4>(o + F_SUN);           // direction to it, radiance
    f.size    = muScene.Load<float4>(o + F_SIZE);          // width, height, tan of one pixel's half-angle, dt
    f.index   = muScene.Load<uint4>(o + F_INDEX);          // fine, coarse, linear sampler, pre-pass image
    f.options = muScene.Load<uint4>(o + F_OPTIONS);        // view, use coarse level, use pre-pass, step budget
    f.balls   = muScene.Load<uint4>(o + F_BALLS);
    float4 vol = muScene.Load<float4>(o + F_VOL);
    f.volume.fine = f.index.x; f.volume.coarse = f.index.y; f.volume.linear = f.index.z; f.volume.pad = 0u;
    f.volume.dims = vol.xyz; f.volume.band = vol.w;
    return f;
}

// The ray through full-resolution pixel position `pixel` (rows run down).
float3 rayThrough(Frame f, float2 pixel)
{
    float2 ndc = float2(pixel.x / f.size.x * 2.0 - 1.0, 1.0 - pixel.y / f.size.y * 2.0);
    return normalize(f.forward + ndc.x * f.right + ndc.y * f.up);
}

static const float BLOCK = 8.0;

[shader("fragment")]
float4 fragPrepass(FullscreenVaryings i) : SV_Target0
{
    Frame  f  = loadFrame(pc.frame_offset);
    float3 rd = rayThrough(f, i.position.xy * BLOCK);      // the block's centre
    // The cone must contain every pixel ray of the block: half a block's
    // diagonal, in pixel half-angles (two per pixel), plus a little. And one
    // more pixel half-angle, because a ray counts as a hit when it passes
    // closer to the surface than it is wide: "empty" has to mean "nothing
    // within a pixel's width". (The argument needs it; in the test scene the
    // start depths are safe without it as well.)
    float coneTan = f.size.z * (BLOCK * 1.4143 * 1.05 + 1.0);
    // Nothing is further than the box's farthest corner.
    float far = length(max(abs(f.eye), abs(f.eye - f.volume.dims)));
    float steps;
    float t = volumeConeStart(f.volume, f.eye, rd, far, coneTan, f.options.y != 0u, 96u, steps);
    return float4(t, steps, 0.0, 0.0);
}

float3 heat(float t)
{
    t = saturate(t);
    return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}

float3 skyColor(float3 rd)
{
    return lerp(float3(0.62, 0.70, 0.80), float3(0.20, 0.38, 0.70), saturate(rd.y * 1.6 + 0.1));
}

[shader("fragment")]
float4 fragTrace(FullscreenVaryings i) : SV_Target0
{
    Frame  f  = loadFrame(pc.frame_offset);
    Volume v  = f.volume;
    float3 rd = rayThrough(f, i.position.xy);
    float3 face;
    float2 box = volumeBox(v, f.eye, rd, face);

    VolumeHit h;
    h.t = 1e6; h.steps = 0.0; h.hit = false;
    float start = box.x;
    if (box.x < box.y)
    {
        if (f.options.z != 0u)
        {
            float pre = muSampledImages[NonUniformResourceIndex(f.index.w)].Load(int3(int2(i.position.xy / BLOCK), 0)).r;
            // A point at length t on any ray of the block is no deeper along
            // the axis than t, so `pre` itself is safe; half a voxel is slack.
            start = max(box.x, min(pre - 0.5, box.y));
        }
        h = volumeTrace(v, f.eye, rd, start, box.y, f.size.z, f.options.y != 0u, f.options.w,
                        muScene.Load<float4>(pc.frame_offset + F_RELAX).x);
    }

    // The balls in the air: spheres, nearest first wins against the volume.
    float  tBall = h.hit ? h.t : 1e6;
    int    ball  = -1;
    for (uint b = 0u; b < f.balls.y; ++b)
    {
        float4 pr = muScene.Load<float4>(f.balls.x + b * BALL_SIZE);
        if (muScene.Load<float4>(f.balls.x + b * BALL_SIZE + 16u).w > 0.5) continue;   // it has landed
        float3 oc = f.eye - pr.xyz;
        float  bq = dot(oc, rd), cq = dot(oc, oc) - pr.w * pr.w, disc = bq * bq - cq;
        if (disc <= 0.0) continue;
        float t = -bq - sqrt(disc);
        if (t > 0.0 && t < tBall) { tBall = t; ball = (int)b; }
    }

    if (f.options.x == 7u)                                  // --check: depth (whole voxels, rest), steps, hit
        return float4(h.hit ? floor(h.t) : 0.0, h.steps, h.hit ? frac(h.t) : 0.0, h.hit ? 1.0 : 0.0);
    if (f.options.x == 8u)                                  // --check: where the ray started
        return float4(floor(start), 0.0, frac(start), box.x < box.y ? 1.0 : 0.0);
    if (f.options.x == 1u)
        return float4(mu_srgb_to_linear(heat(h.steps / 64.0)), 1.0);
    if (f.options.x == 4u)
        return float4(mu_srgb_to_linear(box.x < box.y ? float3(frac(start / 64.0), start / 400.0, 0.3) : float3(0.0)), 1.0);
    if (!h.hit && ball < 0) return float4(skyColor(rd), 0.0);

    float3 p, n, albedo;
    float  ao = 1.0;
    if (ball >= 0)
    {
        float4 pr = muScene.Load<float4>(f.balls.x + (uint)ball * BALL_SIZE);
        p = f.eye + rd * tBall;
        n = normalize(p - pr.xyz);
        albedo = ballColor((uint)ball);
    }
    else
    {
        p = f.eye + rd * h.t;
        float4 s = volumeSample(v, p);
        albedo = s.gba;
        // Entered through a face of the box into solid: a cut. Flat normal.
        bool cut = h.t <= box.x + 1e-3 && s.r < 0.0;
        n  = cut ? face : volumeNormal(v, p);
        ao = cut ? 1.0 : volumeAO(v, p, n);
    }
    if (f.options.x == 2u) return float4(mu_srgb_to_linear(n * 0.5 + 0.5), 1.0);

    float shadow = volumeShadow(v, p + n * 1.5, f.sun.xyz, 8.0, f.options.x != 5u);
    if (f.options.x == 3u || f.options.x == 5u) albedo = float3(0.6);   // light only; 5: plain shadow ratio
    MuSurface surf = mu_surface(albedo, n, -rd, 0.8, 0.0);
    float3 color = mu_pbr_direct(surf, f.sun.xyz, float3(1.0, 0.94, 0.84) * f.sun.w) * shadow
                 + albedo * ao * lerp(float3(0.16, 0.14, 0.12), float3(0.26, 0.34, 0.48), n.y * 0.5 + 0.5);
    return float4(color, 1.0);
}
