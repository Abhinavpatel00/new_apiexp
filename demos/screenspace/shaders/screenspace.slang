// screenspace.slang — ambient occlusion and reflections from the depth buffer.
//
//   vertMain / fragScene   G-buffer: albedo (a = mirror), world normal, view depth.
//                          With MIRROR the scene is drawn reflected in the floor.
//   fragAO                 hemisphere samples around each pixel, counted as blocked
//                          when the depth buffer is nearer than they are
//   fragBlur               4 x 4 average over the same depth: removes the pattern of
//                          the per-pixel rotation
//   fragSSR                the reflected ray, marched through the depth buffer
//   fragPlanar             the true reflection, from the mirrored G-buffer (reference)
//   fragComposite          lighting, occlusion on the ambient part, reflection on the floor
//
// View space here: x right, y up, z forward (positive depth).
import mu_gfx;
import mu_mesh;

static const uint MIRROR = 1u, NO_AO = 2u, NO_BLUR = 4u;

struct Push
{
    float4 vp0, vp1, vp2, vp3;
    float4 eye;          // w: occlusion radius, m
    float4 right;        // w: tan(half fov x)
    float4 up;           // w: tan(half fov y)
    float4 forward;      // w: occlusion bias, m
    float4 size;         // width, height, reflection reach (m), thickness (m)
    float4 light;        // direction to the light, w intensity
    uint   vertex_offset, object_offset, flags, samples;
    uint   albedo, normal, depth, ao;
    uint   ao_raw, ssr, planar, sampler;
    uint   mode, view, steps, pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 color  : TEXCOORD2;
};

[shader("vertex")]
Varyings vertMain(uint vertexIndex : SV_VertexID)
{
    MeshVertex v = muLoadMeshVertex(pc.vertex_offset, vertexIndex);
    MeshObject o = muLoadMeshObject(pc.object_offset);
    Varyings r;
    r.world  = muMulPoint(o.model, v.position);
    r.normal = muMulDir(o.model, v.normal);
    if ((pc.flags & MIRROR) != 0u) { r.world.y = -r.world.y; r.normal.y = -r.normal.y; }
    r.color    = o.color;
    r.position = pc.vp0 * r.world.x + pc.vp1 * r.world.y + pc.vp2 * r.world.z + pc.vp3;
    return r;
}

struct SceneOut
{
    float4 albedo : SV_Target0;   // a: 1 = mirror (the floor)
    float4 normal : SV_Target1;   // world space
    float  depth  : SV_Target2;   // along the view direction, metres; 0 = nothing
};

[shader("fragment")]
SceneOut fragScene(Varyings i)
{
    SceneOut o;
    float3 albedo = i.color.rgb;
    if (i.color.a > 0.5)
    {
        float2 g = floor(i.world.xz);
        albedo *= lerp(0.75, 1.0, muMod(g.x + g.y, 2.0));
    }
    o.albedo = float4(albedo, i.color.a);
    o.normal = float4(normalize(i.normal), 0.0);
    o.depth  = dot(i.world - pc.eye.xyz, pc.forward.xyz);
    return o;
}

// ---- shared -------------------------------------------------------------------

float  depthAt(int2 px) { return muSampledImages[NonUniformResourceIndex(pc.depth)].Load(int3(px, 0)).r; }
float3 normalAt(int2 px) { return muSampledImages[NonUniformResourceIndex(pc.normal)].Load(int3(px, 0)).xyz; }
float4 albedoAt(int2 px) { return muSampledImages[NonUniformResourceIndex(pc.albedo)].Load(int3(px, 0)); }

float3 viewPos(float2 pixel, float depth)
{
    float2 ndc = float2(pixel.x / pc.size.x * 2.0 - 1.0, 1.0 - pixel.y / pc.size.y * 2.0);
    return float3(ndc.x * pc.right.w * depth, ndc.y * pc.up.w * depth, depth);
}
float2 toPixel(float3 p)
{
    float2 ndc = float2(p.x / (p.z * pc.right.w), p.y / (p.z * pc.up.w));
    return float2((ndc.x * 0.5 + 0.5) * pc.size.x, (0.5 - 0.5 * ndc.y) * pc.size.y);
}
float3 toView(float3 w) { return float3(dot(w, pc.right.xyz), dot(w, pc.up.xyz), dot(w, pc.forward.xyz)); }
float3 toWorld(float3 v) { return pc.right.xyz * v.x + pc.up.xyz * v.y + pc.forward.xyz * v.z; }

float3 sky(float3 dir) { return lerp(float3(0.55, 0.62, 0.70), float3(0.16, 0.30, 0.62), saturate(dir.y)); }
float3 ambient(float3 n) { return lerp(float3(0.07, 0.06, 0.05), float3(0.22, 0.27, 0.36), n.y * 0.5 + 0.5); }
float3 shade(float3 albedo, float3 n, float ao)
{
    return albedo * (float3(1.0, 0.95, 0.85) * (pc.light.w * saturate(dot(n, pc.light.xyz))) + ambient(n) * ao);
}

// ---- ambient occlusion --------------------------------------------------------

float radicalInverse(uint i, uint base)
{
    float f = 1.0, r = 0.0;
    while (i > 0u) { f /= float(base); r += f * float(i % base); i /= base; }
    return r;
}

// The share of a hemisphere of radius eye.w above the pixel that the depth buffer
// does not block. The sample set is the same distribution for any count, so the
// 256-sample run is what the 16-sample one estimates.
[shader("fragment")]
float4 fragAO(FullscreenVaryings i) : SV_Target0
{
    int2  px = int2(i.position.xy);
    float d  = depthAt(px);
    if (d <= 0.0) return float4(1.0);
    float3 p = viewPos(i.position.xy, d);
    float3 n = normalize(toView(normalAt(px)));
    float3 t = normalize(cross(n, abs(n.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
    float3 b = cross(n, t);
    // One of 16 rotations, repeating every 4 x 4 pixels; fragBlur averages exactly one such tile.
    float  angle = 6.2831853 * float(((px.x & 3) * 4 + (px.y & 3)) * 7 % 16) / 16.0;
    float3 tr = cos(angle) * t + sin(angle) * b, br = cross(n, tr);
    float  radius = pc.eye.w, blocked = 0.0;
    for (uint k = 0u; k < pc.samples; ++k)
    {
        float u = (float(k) + 0.5) / float(pc.samples), v = radicalInverse(k, 2u), w = radicalInverse(k, 3u);
        float r = sqrt(u), phi = 6.2831853 * v;
        float3 s = p + (tr * (r * cos(phi)) + br * (r * sin(phi)) + n * sqrt(1.0 - u)) * (radius * lerp(0.1, 1.0, w * w));
        if (s.z <= 0.01) continue;
        float2 q = toPixel(s);
        if (any(q < 0.0) || any(q >= pc.size.xy)) continue;      // off screen: unknown, counted as open
        float sd = depthAt(int2(q));
        if (sd <= 0.0) continue;
        // Blocked: the surface seen there is nearer than the sample, and it rises above this
        // pixel's own tangent plane. Without the second test a bare floor seen at a shallow
        // angle blocks itself (--check: 2 of 16 samples), because a neighbouring pixel's
        // depth is that of its centre, not of the sample.
        float3 q0 = viewPos(floor(q) + 0.5, sd);
        bool   hit = sd < s.z && dot(q0 - p, n) > pc.forward.w;
        // Something much nearer than the sample is probably not near the pixel at all.
        blocked += (hit ? 1.0 : 0.0) * smoothstep(0.0, 1.0, radius / max(abs(p.z - sd), 1e-4));
    }
    return float4(1.0 - blocked / float(pc.samples));
}

[shader("fragment")]
float4 fragBlur(FullscreenVaryings i) : SV_Target0
{
    int2  px = int2(i.position.xy);
    float d  = depthAt(px);
    if (d <= 0.0) return float4(1.0);
    float sum = 0.0, weight = 0.0;
    for (int y = -2; y < 2; ++y)
        for (int x = -2; x < 2; ++x)
        {
            int2  q  = clamp(px + int2(x, y), int2(0), int2(pc.size.xy) - 1);
            float qd = depthAt(q);
            float w  = (qd > 0.0 && abs(qd - d) < 0.02 * d + 0.05) ? 1.0 : 0.0;
            sum    += w * muSampledImages[NonUniformResourceIndex(pc.ao_raw)].Load(int3(q, 0)).r;
            weight += w;
        }
    return float4(sum / max(weight, 1.0));
}

// ---- reflections --------------------------------------------------------------

// rgb: what the mirror shows; a: 1 = something was hit, 0 = the sky.
[shader("fragment")]
float4 fragSSR(FullscreenVaryings i) : SV_Target0
{
    int2   px = int2(i.position.xy);
    float4 a  = albedoAt(px);
    float  d  = depthAt(px);
    if (d <= 0.0 || a.a < 0.5) return float4(0.0);
    float3 p = viewPos(i.position.xy, d);
    float3 n = normalize(toView(normalAt(px)));
    float3 r = reflect(normalize(p), n);
    float3 fallback = sky(toWorld(r));
    float  dt = pc.size.z / float(pc.steps), before = 0.0;
    for (uint k = 1u; k <= pc.steps; ++k)
    {
        float  t = dt * float(k);
        float3 s = p + r * t;
        if (s.z <= 0.05) break;
        float2 q = toPixel(s);
        if (any(q < 0.0) || any(q >= pc.size.xy)) break;         // left the screen: nothing known
        float sd = depthAt(int2(q));
        float behind = sd > 0.0 ? s.z - sd : -1.0;
        if (behind > 0.0 && behind < pc.size.w)
        {
            // Between `before` and t the ray went through the surface: halve the stretch 6 times.
            float lo = before, hi = t;
            for (uint j = 0u; j < 6u; ++j)
            {
                float  mid = 0.5 * (lo + hi);
                float3 m   = p + r * mid;
                float  md  = depthAt(int2(toPixel(m)));
                if (md > 0.0 && m.z > md) hi = mid; else lo = mid;
            }
            int2   h  = int2(toPixel(p + r * hi));
            float4 ha = albedoAt(h);
            if (depthAt(h) <= 0.0 || ha.a > 0.5) return float4(fallback, 0.0);   // slipped past the edge onto sky or floor
            return float4(shade(ha.rgb, normalAt(h), 1.0), 1.0);
        }
        // Behind something by more than its assumed thickness: hidden, keep going.
        before = t;
    }
    return float4(fallback, 0.0);
}

// The mirrored scene at the same pixel. Its normals are mirrored too.
[shader("fragment")]
float4 fragPlanar(FullscreenVaryings i) : SV_Target0
{
    int2  px = int2(i.position.xy);
    float d  = depthAt(px);                       // of the mirrored scene: these indices point at its G-buffer
    if (d > 0.0)
    {
        float3 n = normalAt(px);
        return float4(shade(albedoAt(px).rgb, float3(n.x, -n.y, n.z), 1.0), 1.0);
    }
    float3 v = normalize(viewPos(i.position.xy, 1.0));
    return float4(sky(toWorld(reflect(v, toView(float3(0.0, 1.0, 0.0))))), 0.0);
}

[shader("fragment")]
float4 fragComposite(FullscreenVaryings i) : SV_Target0
{
    int2   px = int2(i.position.xy);
    float  d  = depthAt(px);
    float3 v  = toWorld(normalize(viewPos(i.position.xy, 1.0)));
    float  ao = 1.0;
    if ((pc.flags & NO_AO) == 0u)
        ao = muSampledImages[NonUniformResourceIndex((pc.flags & NO_BLUR) != 0u ? pc.ao_raw : pc.ao)].Load(int3(px, 0)).r;
    if (pc.view == 1u) return float4(ao, ao, ao, 1.0);
    if (d <= 0.0) return float4(pc.view == 2u ? float3(0.0) : sky(v), 1.0);
    float4 a = albedoAt(px);
    float3 n = normalAt(px);
    float3 color = shade(a.rgb, n, ao);
    float4 mirror = float4(0.0);
    if (a.a > 0.5 && pc.mode != 0u)
    {
        mirror = muSampledImages[NonUniformResourceIndex(pc.mode == 1u ? pc.ssr : pc.planar)].Load(int3(px, 0));
        float f = lerp(0.25, 1.0, pow(1.0 - saturate(dot(-v, n)), 5.0));
        color = lerp(color, mirror.rgb, f);
    }
    if (pc.view == 2u) return float4(a.a > 0.5 ? mirror.rgb : float3(0.0), 1.0);
    return float4(color, 1.0);
}
