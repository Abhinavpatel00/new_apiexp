// draw.slang — sphere tracing the distance of pat_common.slang.
//
// A ray walks the baked volume until it is within a voxel of the level set,
// then takes its last steps with pointwise queries, so where it stops and
// the normal there come from the tori and not from the volume. --direct
// leaves the volume out. The slice view asks once per pixel.
import mu_gfx;
#define PAT_SCENE muScene
#include "mu_volume.slang"
#include "pat_common.slang"

struct Push
{
    float4 eye;          // w: offset, the level drawn
    float4 right;        // w: tan(fov / 2)
    float4 up;           // w: aspect
    float4 forward;      // w: where the slice cuts z
    uint   header, combine, method, view;
    uint   volume, sampler, direct, points;   // points: byte offset of the first cloud's points
    float  half_size, voxel, dot_size;
    uint   count;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

float coarse(float3 p)
{
    return muSampledVolumes[NonUniformResourceIndex(pc.volume)]
               .SampleLevel(muSamplers[NonUniformResourceIndex(pc.sampler)], p / (2.0 * pc.half_size) + 0.5, 0.0).r - pc.eye.w;
}

// Outside orange, inside blue, a line every 0.05 and a white one at zero.
float3 bands(float d)
{
    float3 c = d > 0.0 ? float3(0.9, 0.55, 0.25) : float3(0.3, 0.5, 0.85);
    c *= 1.0 - exp(-8.0 * abs(d)) * 0.35;
    c *= 0.8 + 0.2 * cos(2.0 * 3.14159265 * d / 0.05);
    return lerp(c, float3(1.0), 1.0 - smoothstep(0.0, 0.006, abs(d)));
}

[shader("fragment")]
float4 fragDraw(FullscreenVaryings i) : SV_Target0
{
    float2 ndc = i.uv * 2.0 - 1.0;
    float3 gradient = float3(0.0, 0.0, 1.0);

    if (pc.view == 1u || pc.view == 8u)
    {
        // The plane z = slice, looked at straight on: the picture's height is the volume's.
        float3 x = float3(ndc.x * pc.up.w * pc.half_size, ndc.y * pc.half_size, pc.forward.w);
        float  d = patScene(pc.header, pc.combine, x, pc.method, gradient) - pc.eye.w;
        if (pc.view == 8u) return float4(d, gradient);     // --check
        return float4(bands(d), 1.0);
    }

    float3 ro = pc.eye.xyz;
    float3 rd = normalize(pc.forward.xyz + ndc.x * pc.right.w * pc.up.w * pc.right.xyz + ndc.y * pc.right.w * pc.up.xyz);
    float3 sky = lerp(float3(0.045, 0.05, 0.065), float3(0.11, 0.12, 0.15), 0.5 + 0.5 * ndc.y);

    // The part of the ray inside the volume's box.
    float3 inv = 1.0 / rd;
    float3 t0 = (-pc.half_size - ro) * inv, t1 = (pc.half_size - ro) * inv;
    float  enter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), max(min(t0.z, t1.z), 0.0));
    float  leave = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    float  t = enter, d = 1.0;
    uint   steps = 0u;
    bool   hit = false;
    if (enter < leave)
    {
        // A ray steps by the baked volume while that says the surface is more than a voxel away, and
        // by the points themselves from there on; --direct asks the points every time.
        for (; steps < 200u && t < leave; ++steps)
        {
            d = pc.direct == 0u ? coarse(ro + rd * t) : 0.0;
            if (d > pc.voxel) { t += d - 0.5 * pc.voxel; continue; }
            d = patScene(pc.header, pc.combine, ro + rd * t, pc.method, gradient) - pc.eye.w;
            if (d < 1e-4) { hit = true; break; }
            t += 0.9 * d;
        }
        // The blend is not a true distance (it can fall faster than 1 per unit), so a step can end
        // behind the surface. Then the ray comes back, by Newton's rule along the ray.
        for (uint k = 0u; k < 4u && hit && d < -1e-4; ++k)
        {
            t += d / max(-dot(normalize(gradient), rd), 0.1);
            d = patScene(pc.header, pc.combine, ro + rd * t, pc.method, gradient) - pc.eye.w;
        }
    }
    if (pc.view == 9u) return float4(hit ? t : -1.0, d, float(steps), 0.0);     // --check
    if (!hit) return float4(sky, 1.0);

    float3 n = normalize(gradient);
    if (pc.view == 2u) return float4(n * 0.5 + 0.5, 1.0);
    float3 key   = normalize(pc.right.xyz * 0.5 + pc.up.xyz * 0.7 - pc.forward.xyz * 0.6);
    float  light = 0.18 + 0.82 * max(dot(n, key), 0.0) + 0.25 * max(dot(n, -pc.right.xyz), 0.0);
    float  rim   = pow(1.0 - max(dot(n, -rd), 0.0), 3.0);
    float3 base  = pc.method == 1u ? float3(0.55, 0.6, 0.7) : float3(0.8, 0.62, 0.45);
    return float4(base * light + 0.25 * rim, 1.0);
}

// ---- the input points, as dots ---------------------------------------------

struct DotVaryings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv    : TEXCOORD0;
    [[vk::location(1)]] float  shade : TEXCOORD1;
};

static const float2 CORNER[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};

[shader("vertex")]
DotVaryings vertDots(uint vertexIndex : SV_VertexID)
{
    PatPoint    p = muScene.Load<PatPoint>(pc.points + vertexIndex / 6u * POINT_SIZE);
    float2      corner = CORNER[vertexIndex % 6u];
    float3      d = p.pos - pc.eye.xyz;
    float3      cam = float3(dot(d, pc.right.xyz), dot(d, pc.up.xyz), dot(d, pc.forward.xyz));
    DotVaryings o;
    cam.xy += corner * pc.dot_size;
    o.uv    = corner;
    o.shade = dot(p.normal, -normalize(d));
    // Points facing away are dropped: there is no depth buffer to hide them.
    o.position = o.shade > 0.0 ? float4(cam.x / (pc.right.w * pc.up.w), cam.y / pc.right.w, 0.5 * cam.z, cam.z) : float4(0, 0, -1, 1);
    return o;
}

[shader("fragment")]
float4 fragDots(DotVaryings i) : SV_Target0
{
    if (dot(i.uv, i.uv) > 1.0) discard;
    return float4(float3(0.35, 0.9, 1.0) * (0.4 + 0.6 * i.shade), 1.0);
}
