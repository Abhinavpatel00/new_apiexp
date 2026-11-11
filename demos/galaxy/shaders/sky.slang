// sky.slang — procedural deep-space backdrop: gradient, two fbm nebula layers,
// a warm bulge towards the galactic core, and a hashed star field.
//
// The vertex stage is the shared full-screen triangle
// (demos/common/shaders/fullscreen.slang -> fullscreen.vert.spv).
// Push (64 B) must match SkyPush in demos/galaxy/main.c.
import mu_gfx;

struct SkyPush
{
    float4 right;
    float4 up;
    float4 forward;
    float4 params; // aspect, tan-half-fov, time, intensity
};
[[vk::push_constant]] ConstantBuffer<SkyPush> pc;

float hash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

float noise3(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(i + float3(0, 0, 0));
    float n100 = hash31(i + float3(1, 0, 0));
    float n010 = hash31(i + float3(0, 1, 0));
    float n110 = hash31(i + float3(1, 1, 0));
    float n001 = hash31(i + float3(0, 0, 1));
    float n101 = hash31(i + float3(1, 0, 1));
    float n011 = hash31(i + float3(0, 1, 1));
    float n111 = hash31(i + float3(1, 1, 1));
    float x00 = lerp(n000, n100, f.x);
    float x10 = lerp(n010, n110, f.x);
    float x01 = lerp(n001, n101, f.x);
    float x11 = lerp(n011, n111, f.x);
    return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}

float fbm(float3 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; ++i)
    {
        sum += amp * noise3(p);
        p *= 2.03;
        amp *= 0.5;
    }
    return sum;
}

[shader("fragment")]
float4 fragMain(FullscreenVaryings i) : SV_Target0
{
    float2 ndc = i.uv * 2.0 - 1.0;
    float3 rd  = normalize(pc.forward.xyz + pc.right.xyz * (ndc.x * pc.params.x * pc.params.y)
                           + pc.up.xyz * (ndc.y * pc.params.y));
    float  t   = pc.params.z;
    float3 col = lerp(float3(0.003, 0.005, 0.014), float3(0.012, 0.020, 0.052),
                      saturate(rd.y * 0.5 + 0.5));

    float3 q    = rd * 3.2 + float3(0.0, t * 0.009, 0.0);
    float  n1   = fbm(q);
    float  n2   = fbm(q * 2.6 + float3(5.2, 1.3, 2.8));
    float  band = exp(-abs(rd.y) * 2.8);
    col += (float3(0.075, 0.025, 0.105) * n1 * n1
          + float3(0.012, 0.052, 0.085) * n2 * band) * band * 2.0;
    col += float3(0.025, 0.050, 0.10) * pow(n2, 3.0) * 0.8;

    float3 to_core = normalize(pc.forward.xyz);
    float  bulge   = pow(max(dot(rd, to_core), 0.0), 7.0);
    col += float3(0.20, 0.12, 0.06) * bulge * 0.55;

    float3 g    = rd * 420.0;
    float3 cell = floor(g);
    float  h    = hash31(cell);
    float  d    = length(frac(g) - 0.5);
    float  star = smoothstep(0.16, 0.015, d) * step(0.997, h);
    col += star * (0.8 + 4.0 * pow(h, 16.0))
         * lerp(float3(0.62, 0.76, 1.0), float3(1.0, 0.82, 0.62), h * h);

    return float4(col * pc.params.w, 1.0);
}
