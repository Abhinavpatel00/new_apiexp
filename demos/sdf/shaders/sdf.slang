// sdf.slang — a sphere-traced scene in one full-screen pass.
//
// Everything geometric comes from external/mu_lib/mu/shaders/sdf.slang: the
// primitives, the smooth minimum, mu_sdf_raymarch_stats(), mu_sdf_normal(),
// mu_sdf_soft_shadow() and mu_sdf_ao(). This file supplies the scene (one
// struct implementing IMuSdf3), a material lookup and the shading.
//
// Output (RGBA16F): rgb = scene-linear colour,
//                   a   = step count, plus 1024 when the ray hit something.
// The alpha channel is what `sdf --check` reads back.
import mu_gfx;
#include "shaders/sdf.slang"

struct TracePush
{
    float4 eye_time;       // xyz eye, w seconds
    float4 right_tan;      // xyz camera right, w tan(fov_y / 2)
    float4 up_aspect;      // xyz camera up, w aspect
    float4 forward_tmax;   // xyz camera forward, w far distance
    float4 sun_k;          // xyz direction TO the sun, w soft-shadow hardness
    float4 params;         // x AO strength, y fog density, z calibration radius, w blend k
    uint   scene;          // 0 = demo scene, 1 = calibration sphere
    uint   view;           // 0 shaded, 1 normals, 2 steps, 3 AO, 4 shadow
    uint   max_steps;
    uint   shadow_steps;
};
[[vk::push_constant]] ConstantBuffer<TracePush> pc;

static const float GROUND = 1.0;
static const float BLOB   = 2.0;
static const float RING   = 3.0;
static const float PILLAR = 4.0;

struct Scene : IMuSdf3
{
    float time;
    float blend;
    float radius;
    uint  kind;

    // Distance in x, material id in y.
    float2 map(float3 p)
    {
        if (kind == 1u)
            return float2(mu_sd_sphere(p, radius), BLOB);

        float2 best = float2(mu_sd_plane(p, float3(0.0, 1.0, 0.0), 0.0), GROUND);

        // A sphere, a rounded box and a small orbiting sphere, smoothly joined.
        float3 c  = float3(0.0, 1.0, 0.0);
        float  s  = mu_sd_sphere(p - c - float3(0.0, 0.35 * sin(time * 1.3), 0.0), 0.75);
        float  b  = mu_sd_round_box(p - c + float3(0.0, 0.55, 0.0), float3(0.9, 0.3, 0.9), 0.12);
        float3 o  = c + float3(1.15 * cos(time), 0.25, 1.15 * sin(time));
        float  m  = mu_sd_sphere(p - o, 0.32);
        float  d  = mu_sd_smin(mu_sd_smin(s, b, blend), m, blend);
        if (d < best.x) best = float2(d, BLOB);

        // A tilted torus around it.
        float3 q = p - float3(0.0, 1.15, 0.0);
        q.yz = mu_sd_rotate2_cs(q.yz, float2(cos(0.5), sin(0.5)));
        q.xy = mu_sd_rotate2_cs(q.xy, float2(cos(time * 0.4), sin(time * 0.4)));
        float t = mu_sd_torus(q, float2(1.75, 0.07));
        if (t < best.x) best = float2(t, RING);

        // A ring of capsules, by polar repetition.
        // mu_sd_polar returns (radius, angle folded into one sector).
        float2 ra = mu_sd_polar(p.xz, 8.0);
        float3 r  = float3(ra.x * cos(ra.y), p.y, ra.x * sin(ra.y));
        float  k  = mu_sd_capsule(r, float3(3.4, 0.0, 0.0), float3(3.4, 1.1, 0.0), 0.16);
        if (k < best.x) best = float2(k, PILLAR);
        return best;
    }
    float dist(float3 p) { return map(p).x; }
};

float3 albedoOf(float id, float3 p)
{
    if (id == GROUND)
    {
        float2 g = floor(p.xz);
        float  c = muMod(g.x + g.y, 2.0);
        return lerp(float3(0.30, 0.31, 0.34), float3(0.52, 0.53, 0.56), c);
    }
    if (id == BLOB)   return float3(0.86, 0.32, 0.22);
    if (id == RING)   return float3(0.95, 0.74, 0.25);
    return float3(0.25, 0.55, 0.80);
}

float3 skyColor(float3 rd, float3 sun)
{
    float  h   = saturate(rd.y * 0.5 + 0.5);
    float3 sky = lerp(float3(0.50, 0.60, 0.74), float3(0.10, 0.22, 0.52), saturate(h * 2.0 - 1.0));
    float  s   = saturate(dot(rd, sun));
    return sky + float3(1.0, 0.82, 0.55) * (pow(s, 64.0) * 0.6 + pow(s, 900.0) * 6.0);
}

// Blue -> green -> red, for the step-count view.
float3 heat(float x)
{
    x = saturate(x);
    return saturate(float3(1.5 - abs(4.0 * x - 3.0), 1.5 - abs(4.0 * x - 2.0), 1.5 - abs(4.0 * x - 1.0)));
}

[shader("fragment")]
float4 fragTrace(FullscreenVaryings i) : SV_Target0
{
    // uv.y = 1 is the top of the image (see muFullscreenTriangle).
    float2 ndc = i.uv * 2.0 - 1.0;
    float3 ro  = pc.eye_time.xyz;
    float3 rd  = normalize(pc.forward_tmax.xyz
                         + pc.right_tan.xyz * (ndc.x * pc.up_aspect.w * pc.right_tan.w)
                         + pc.up_aspect.xyz * (ndc.y * pc.right_tan.w));
    float3 sun = pc.sun_k.xyz;
    float  tmax = pc.forward_tmax.w;

    Scene scene;
    scene.time   = pc.eye_time.w;
    scene.blend  = pc.params.w;
    scene.radius = pc.params.z;
    scene.kind   = pc.scene;

    // uv covers 0..1 over the image height, so |ddy(uv.y)| is 1 / height and
    // `pixel` is the width of one pixel at unit distance.
    float pixel = 2.0 * pc.right_tan.w * abs(ddy(i.uv.y));
    float eps   = pc.scene == 1u ? 1e-4 : 2e-4;

    float2 hit   = mu_sdf_raymarch_stats(scene, ro, rd, 0.02, tmax, pc.max_steps, eps);
    float  t     = hit.x;
    float  steps = round(hit.y * (float)pc.max_steps) + 1.0;
    bool   found = t < tmax;

    float3 color = skyColor(rd, sun);
    float3 n = float3(0.0, 1.0, 0.0);
    float  ao = 1.0, shadow = 1.0;
    if (found)
    {
        float3 p  = ro + rd * t;
        float  e  = max(pixel * t, 1e-4);   // normal epsilon = pixel footprint
        n         = mu_sdf_normal(scene, p, e);
        float  id = scene.map(p).y;
        float3 albedo = albedoOf(id, p);

        float3 origin = p + n * (2.0 * eps + 1e-3);
        shadow = mu_sdf_soft_shadow(scene, origin, sun, 0.02, 12.0, pc.sun_k.w, pc.shadow_steps);
        ao     = lerp(1.0, mu_sdf_ao(scene, p, n, 0.09), pc.params.x);

        float  ndl  = saturate(dot(n, sun));
        float3 h    = normalize(sun - rd);
        float  spec = pow(saturate(dot(n, h)), 48.0) * (id == GROUND ? 0.05 : 0.35);
        float3 sunLight = float3(1.0, 0.93, 0.82) * 2.2;
        float3 skyLight = lerp(float3(0.10, 0.095, 0.09), float3(0.20, 0.27, 0.40), n.y * 0.5 + 0.5);
        color = albedo * (sunLight * ndl * shadow + skyLight * ao) + sunLight * spec * ndl * shadow;

        // Distance fog towards the horizon colour.
        float fog = 1.0 - exp(-pc.params.y * t * t);
        color = lerp(color, skyColor(rd, sun), fog);
    }

    switch (pc.view)
    {
    case 1:  color = found ? n * 0.5 + 0.5 : float3(0.0); color *= color; break;
    case 2:  color = heat(steps / (float)pc.max_steps); break;
    case 3:  color = float3(found ? ao : 1.0); break;
    case 4:  color = float3(found ? shadow : 1.0); break;
    default: break;
    }
    return float4(color, steps + (found ? 1024.0 : 0.0));
}
