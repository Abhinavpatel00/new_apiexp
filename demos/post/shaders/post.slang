// post.slang — bloom chain and display transform lab.
//
//   fragScene      a procedural HDR test scene (values far above 1)
//   fragDown       13-tap downsample; the first level also thresholds and can
//                  apply the Karis firefly weight
//   fragUp         3x3 tent upsample, drawn with additive blending onto the
//                  next larger level
//   fragComposite  scene + bloom, sharpen, tone map (one, or all eight side by
//                  side), vignette, grain, encode, dither
//
// The filter kernels, the threshold, the tone mappers and the sharpening are
// external/mu_lib/mu/shaders/post.slang; this file does the texture fetches
// around them, which that library deliberately leaves to the caller.
import mu_gfx;
#include "shaders/post.slang"
#include "shaders/format.slang"

// ---------------------------------------------------------------- scene ----
struct ScenePush
{
    float time;
    float aspect;
    float kind;        // 0 = demo scene, 1 = calibration (zero at the borders)
    float brightness;
};

float disk(float2 p, float2 c, float r, float aa) { return 1.0 - smoothstep(r - aa, r + aa, length(p - c)); }

float3 sceneColor(float2 p, ScenePush s)
{
    // p: x in [-aspect, aspect], y in [-1, 1], y up.
    const float aa = 0.003;
    if (s.kind > 0.5)
    {
        // Compact sources well inside the frame, nothing else: every filter
        // tap that matters stays inside the image, so the chain can be
        // checked for energy conservation.
        float3 c = float3(0.0);
        c += float3(4.0, 1.0, 0.25) * disk(p, float2(-0.5, 0.2), 0.08, aa);
        c += float3(0.3, 2.0, 6.0) * disk(p, float2(0.45, -0.25), 0.05, aa);
        c += float3(40.0, 40.0, 40.0) * disk(p, float2(0.1, 0.35), 0.006, 0.002);
        c += float3(1.0, 1.0, 1.0) * exp(-dot(p - float2(0.0, -0.1), p - float2(0.0, -0.1)) * 60.0);
        return c;
    }

    float3 c = lerp(float3(0.010, 0.012, 0.020), float3(0.030, 0.022, 0.045), saturate(p.y * 0.5 + 0.5));

    // Exposure ramp: seven hues, eight stops each. Shows where each tone
    // mapper clips, desaturates or shifts hue.
    float2 g = float2((p.x / s.aspect) * 0.5 + 0.5, p.y * 0.5 + 0.5);
    if (g.y < 0.34 && g.x > 0.04 && g.x < 0.96 && g.y > 0.05)
    {
        float2 cell = float2((g.x - 0.04) / 0.92 * 8.0, (g.y - 0.05) / 0.29 * 7.0);
        float2 f    = frac(cell);
        if (f.x > 0.08 && f.x < 0.92 && f.y > 0.12 && f.y < 0.88)
        {
            static const float3 hues[7] = {
                float3(1.0, 1.0, 1.0), float3(1.0, 0.05, 0.03), float3(1.0, 0.45, 0.02),
                float3(0.9, 0.9, 0.04), float3(0.05, 1.0, 0.08), float3(0.04, 0.5, 1.0),
                float3(0.6, 0.06, 1.0)};
            c = hues[(int)cell.y] * (0.0625 * exp2(floor(cell.x)));
        }
    }

    // A neon ring and a rotating bar.
    float2 q  = p - float2(-0.85, 0.42);
    float  rd = abs(length(q) - 0.33);
    c += float3(0.15, 1.0, 0.75) * 9.0 * (1.0 - smoothstep(0.012, 0.012 + aa, rd));
    float  a  = s.time * 0.6;
    float2 rq = float2(q.x * cos(a) + q.y * sin(a), -q.x * sin(a) + q.y * cos(a));
    c += float3(1.0, 0.25, 0.6) * 7.0
       * (1.0 - smoothstep(0.010, 0.010 + aa, abs(rq.y))) * (1.0 - smoothstep(0.24, 0.24 + aa, abs(rq.x)));

    // A sun.
    c += float3(1.0, 0.72, 0.35) * 30.0 * disk(p, float2(0.95, 0.52), 0.11, aa);

    // Fireflies: nearly single-pixel sources, extremely bright. Without the
    // Karis weight each one becomes a flickering square of bloom.
    for (int i = 0; i < 6; ++i)
    {
        float  fi = (float)i;
        float2 fp = float2(-0.1 + 0.16 * fi, 0.55 + 0.25 * sin(fi * 2.4 + 1.0));
        float  tw = 0.5 + 0.5 * sin(s.time * (2.0 + fi) + fi * 1.7);
        c += float3(1.0, 0.9, 0.8) * (300.0 * tw) * disk(p, fp, 0.0025, 0.0012);
    }
    return c * s.brightness;
}

[[vk::push_constant]] ConstantBuffer<ScenePush> scenePush;

[shader("fragment")]
float4 fragScene(FullscreenVaryings i) : SV_Target0
{
    float2 p = (i.uv * 2.0 - 1.0) * float2(scenePush.aspect, 1.0);
    return float4(sceneColor(p, scenePush), 1.0);
}

// ----------------------------------------------------------- downsample ----
struct DownPush
{
    uint  source;
    uint  sampler;
    float inv_w, inv_h;   // 1 / source size
    float threshold, knee;
    uint  flags;          // bit 0: threshold (first level), bit 1: Karis weight
    uint  pad;
};
[[vk::push_constant]] ConstantBuffer<DownPush> downPush;

float3 tap(uint tex, uint smp, float2 uv) { return muSampleLevel(tex, smp, uv, 0.0).rgb; }

[shader("fragment")]
float4 fragDown(FullscreenVaryings i) : SV_Target0
{
    DownPush pc = downPush;
    float2 uv = muFlipV(i.uv);
    float2 t  = float2(pc.inv_w, pc.inv_h);
    // Every tap is a bilinear fetch between four source texels.
    float3 a = tap(pc.source, pc.sampler, uv + t * float2(-2.0, -2.0));
    float3 b = tap(pc.source, pc.sampler, uv + t * float2( 0.0, -2.0));
    float3 c = tap(pc.source, pc.sampler, uv + t * float2( 2.0, -2.0));
    float3 d = tap(pc.source, pc.sampler, uv + t * float2(-2.0,  0.0));
    float3 e = tap(pc.source, pc.sampler, uv);
    float3 f = tap(pc.source, pc.sampler, uv + t * float2( 2.0,  0.0));
    float3 g = tap(pc.source, pc.sampler, uv + t * float2(-2.0,  2.0));
    float3 h = tap(pc.source, pc.sampler, uv + t * float2( 0.0,  2.0));
    float3 k = tap(pc.source, pc.sampler, uv + t * float2( 2.0,  2.0));
    float3 j0 = tap(pc.source, pc.sampler, uv + t * float2(-1.0, -1.0));
    float3 j1 = tap(pc.source, pc.sampler, uv + t * float2( 1.0, -1.0));
    float3 j2 = tap(pc.source, pc.sampler, uv + t * float2(-1.0,  1.0));
    float3 j3 = tap(pc.source, pc.sampler, uv + t * float2( 1.0,  1.0));

    if ((pc.flags & 1u) != 0u)
    {
        a = mu_bloom_threshold(a, pc.threshold, pc.knee); b = mu_bloom_threshold(b, pc.threshold, pc.knee);
        c = mu_bloom_threshold(c, pc.threshold, pc.knee); d = mu_bloom_threshold(d, pc.threshold, pc.knee);
        e = mu_bloom_threshold(e, pc.threshold, pc.knee); f = mu_bloom_threshold(f, pc.threshold, pc.knee);
        g = mu_bloom_threshold(g, pc.threshold, pc.knee); h = mu_bloom_threshold(h, pc.threshold, pc.knee);
        k = mu_bloom_threshold(k, pc.threshold, pc.knee);
        j0 = mu_bloom_threshold(j0, pc.threshold, pc.knee); j1 = mu_bloom_threshold(j1, pc.threshold, pc.knee);
        j2 = mu_bloom_threshold(j2, pc.threshold, pc.knee); j3 = mu_bloom_threshold(j3, pc.threshold, pc.knee);
    }
    if ((pc.flags & 2u) != 0u)
    {
        // Karis average: weight the five 2x2 boxes of the kernel by
        // 1 / (1 + luma) and renormalise, so one very bright texel cannot
        // dominate the level it lands in.
        float3 b0 = (j0 + j1 + j2 + j3) * 0.25;
        float3 b1 = (a + b + d + e) * 0.25;
        float3 b2 = (b + c + e + f) * 0.25;
        float3 b3 = (d + e + g + h) * 0.25;
        float3 b4 = (e + f + h + k) * 0.25;
        float w0 = 0.5   / (1.0 + mu_luma709(b0));
        float w1 = 0.125 / (1.0 + mu_luma709(b1));
        float w2 = 0.125 / (1.0 + mu_luma709(b2));
        float w3 = 0.125 / (1.0 + mu_luma709(b3));
        float w4 = 0.125 / (1.0 + mu_luma709(b4));
        return float4((b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4) / (w0 + w1 + w2 + w3 + w4), 1.0);
    }
    return float4(mu_bloom_downsample13(a, b, c, d, e, f, g, h, k, j0, j1, j2, j3), 1.0);
}

// ------------------------------------------------------------- upsample ----
struct UpPush
{
    uint  source;
    uint  sampler;
    float inv_w, inv_h;   // 1 / source (smaller level) size
    float radius;         // in source texels
    float pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<UpPush> upPush;

[shader("fragment")]
float4 fragUp(FullscreenVaryings i) : SV_Target0
{
    UpPush pc = upPush;
    float2 uv = muFlipV(i.uv);
    float2 t  = float2(pc.inv_w, pc.inv_h) * pc.radius;
    float3 s0 = tap(pc.source, pc.sampler, uv + t * float2(-1.0, -1.0));
    float3 s1 = tap(pc.source, pc.sampler, uv + t * float2( 0.0, -1.0));
    float3 s2 = tap(pc.source, pc.sampler, uv + t * float2( 1.0, -1.0));
    float3 s3 = tap(pc.source, pc.sampler, uv + t * float2(-1.0,  0.0));
    float3 s4 = tap(pc.source, pc.sampler, uv);
    float3 s5 = tap(pc.source, pc.sampler, uv + t * float2( 1.0,  0.0));
    float3 s6 = tap(pc.source, pc.sampler, uv + t * float2(-1.0,  1.0));
    float3 s7 = tap(pc.source, pc.sampler, uv + t * float2( 0.0,  1.0));
    float3 s8 = tap(pc.source, pc.sampler, uv + t * float2( 1.0,  1.0));
    // Blended additively onto the larger level by the pass state.
    return float4(mu_bloom_upsample9(s0, s1, s2, s3, s4, s5, s6, s7, s8), 1.0);
}

// ------------------------------------------------------------ composite ----
struct CompositePush
{
    uint  scene;
    uint  bloom;
    uint  sampler;
    uint  tonemap;     // DemoTonemap (demo.h)
    float bloom_gain;  // strength / number of levels
    float exposure_ev;
    float sharpen;
    float vignette;
    float grain;
    float time;
    float inv_w, inv_h;
    uint  flags;       // 1 encode sRGB, 2 dither, 4 all tone mappers side by side, 8 bloom only
    uint  pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<CompositePush> compositePush;

float3 applyTonemap(float3 c, uint mode)
{
    switch (mode)
    {
    case 1:  return mu_tonemap_reinhard(c);
    case 2:  return mu_tonemap_aces_fitted(c);
    case 3:  return mu_tonemap_aces(c);
    case 4:  return mu_tonemap_neutral(c);
    case 5:  return mu_tonemap_uchimura(c);
    case 6:  return mu_tonemap_hable(c);
    case 7:  return mu_tonemap_agx(c);
    default: return c;
    }
}

[shader("fragment")]
float4 fragComposite(FullscreenVaryings i) : SV_Target0
{
    CompositePush pc = compositePush;
    float2 uv = muFlipV(i.uv);
    float2 t  = float2(pc.inv_w, pc.inv_h);

    float3 c = tap(pc.scene, pc.sampler, uv);
    if (pc.sharpen > 0.0)
    {
        float3 n = tap(pc.scene, pc.sampler, uv + float2(0.0, -t.y));
        float3 s = tap(pc.scene, pc.sampler, uv + float2(0.0,  t.y));
        float3 e = tap(pc.scene, pc.sampler, uv + float2( t.x, 0.0));
        float3 w = tap(pc.scene, pc.sampler, uv + float2(-t.x, 0.0));
        c = max(mu_cas_sharpen(c, n, s, e, w, pc.sharpen), 0.0);
    }
    float3 bloom = tap(pc.bloom, pc.sampler, uv) * pc.bloom_gain;
    c = (pc.flags & 8u) != 0u ? bloom : c + bloom;
    c *= exp2(pc.exposure_ev);

    uint mode = pc.tonemap;
    bool edge = false;
    if ((pc.flags & 4u) != 0u)
    {
        float x = i.uv.x * 8.0;
        mode = min((uint)x, 7u);
        edge = frac(x) < 8.0 * t.x * 1.5 && x > 0.5;
    }
    c = saturate(applyTonemap(c, mode));
    if (pc.vignette > 0.0) c = mu_vignette(c, i.uv, 1.0 - 0.45 * pc.vignette, 0.55);
    if (edge) c = float3(0.0);

    // Display encoding. Grain and dither are applied in display space.
    const bool encode = (pc.flags & 1u) != 0u;
    c = mu_linear_to_srgb(c);
    if (pc.grain > 0.0) c = mu_film_grain(c, i.position.xy, pc.grain, pc.time);
    if ((pc.flags & 2u) != 0u) c = mu_dither_ign(c, i.position.xy, 1.0 / 255.0);
    c = saturate(c);
    return float4(encode ? c : mu_srgb_to_linear(c), 1.0);
}
