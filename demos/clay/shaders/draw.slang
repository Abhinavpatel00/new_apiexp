// draw.slang — sphere tracing the tray (written down) and the clay (the baked volume).
import mu_gfx;
#include "mu_volume.slang"
#include "clay_common.slang"

[[vk::push_constant]] ConstantBuffer<Push> pc;

float sdBox(float3 p, float3 h)
{
    float3 q = abs(p) - h;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

// Floor, tray walls (10 cm thick, 80 cm high, open top), paddle.
float tray(float3 p, out uint what)
{
    float ground = p.y;
    float walls  = max(sdBox(p - float3(0, 0.4, 0), float3(HALF + 0.1, 0.4, HALF + 0.1)),
                       -sdBox(p - float3(0, 0.6, 0), float3(HALF, 0.6, HALF)));
    float paddle = pc.paddle.w > 0.0 ? length(p - pc.paddle.xyz) - pc.paddle.w : 1e9;
    what = 0u;
    float d = ground;
    if (walls < d) { d = walls; what = 1u; }
    if (paddle < d) { d = paddle; what = 2u; }
    return d;
}

// The clay never leaves the tray's inner column. Outside it: the distance to the column.
float clay(float3 p)
{
    float box = sdBox(p - float3(0, 1.6, 0), float3(HALF, 1.6, HALF));
    if (box > 0.0) return box + VOXEL;
    float3 uvw = (p - ORIGIN) / (VOXEL * float(VOXELS));
    return muSampledVolumes[NonUniformResourceIndex(pc.volume)]
               .SampleLevel(muSamplers[NonUniformResourceIndex(pc.sampler)], uvw, 0.0).r - pc.look.y;
}

float scene(float3 p, out uint what)
{
    float t = tray(p, what);
    float c = clay(p);
    if (c < t) { what = 3u; return c; }
    return t;
}

float3 normalAt(float3 p)
{
    uint w;
    const float e = 0.5 * VOXEL;
    return normalize(float3(scene(p + float3(e, 0, 0), w) - scene(p - float3(e, 0, 0), w),
                            scene(p + float3(0, e, 0), w) - scene(p - float3(0, e, 0), w),
                            scene(p + float3(0, 0, e), w) - scene(p - float3(0, 0, e), w)));
}

[shader("fragment")]
float4 fragDraw(FullscreenVaryings i) : SV_Target0
{
    float2 ndc = float2(i.position.x / pc.size.x * 2.0 - 1.0, 1.0 - i.position.y / pc.size.y * 2.0);
    float3 rd  = normalize(pc.forward.xyz + ndc.x * pc.right.xyz + ndc.y * pc.up.xyz);
    float3 ro  = pc.eye.xyz;
    float  t   = 0.0;
    uint   what = 0u, steps = 0u;
    bool   hit  = false;
    for (; steps < 200u && t < 40.0; ++steps)
    {
        float d = scene(ro + rd * t, what);
        if (d < 0.002 + 0.0005 * t) { hit = true; break; }
        t += d;
    }
    if (pc.view == 1u)                                    // steps taken, blue 0 .. red 100
    {
        float h = saturate(float(steps) / 100.0);
        return float4(h, 0.2, 1.0 - h, 1.0);
    }
    if (pc.view == 3u) return float4(hit ? float(what) : -1.0, t, 0.0, 1.0);   // --check: what the ray hit
    if (!hit) return float4(0.30, 0.42, 0.60, 0.0);
    float3 p = ro + rd * t;
    float3 n = normalAt(p);
    if (pc.view == 2u) return float4(n * 0.5 + 0.5, 1.0);
    float3 albedo = what == 3u ? float3(0.80, 0.36, 0.22) : what == 2u ? float3(0.25, 0.45, 0.85)
                  : what == 1u ? float3(0.75, 0.72, 0.66) : float3(0.45, 0.47, 0.50);
    if (what == 0u) albedo *= lerp(0.8, 1.0, muMod(floor(p.x * 2.0) + floor(p.z * 2.0), 2.0));
    // Openness along the normal: three taps of the field (Quilez's cheap occlusion).
    float open = 1.0;
    uint  w;
    for (int k = 1; k <= 3; ++k)
    {
        float h = 0.06 * float(k);
        open -= max(h - scene(p + n * h, w), 0.0) * (0.9 / float(k)) / 0.06 * 0.3;
    }
    open = saturate(open);
    float3 l = normalize(float3(0.40, 0.80, 0.45));
    return float4(albedo * (0.30 * open + 0.75 * saturate(dot(n, l)) * lerp(0.5, 1.0, open)), 1.0);
}
