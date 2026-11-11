// ocean.slang — an ocean patch from its spectrum, by an inverse FFT in compute.
//
//   compSpectrum   the spectrum at time t from the fixed random one:
//                  H(k, t) = h0(k) e^(-i w t) + conj(h0(-k)) e^(+i w t),  w = sqrt(g |k|)
//                  and the spectra of the horizontal displacement, -i k/|k| H
//   compFFT        one butterfly stage along one axis; log2(N) of them per axis
//   vertOcean / fragOcean   a grid of N x N quads per tile, displaced by the result
//
// Tessendorf, "Simulating Ocean Water" (SIGGRAPH course notes, 2001).
//
// Three real fields are needed (height, displacement x and z) and an inverse FFT
// of a Hermitian spectrum is real, so two of them share one complex transform:
// xy = H + i Dx gives height in x and displacement x in y; zw = Dz.
// Frequencies are in DFT order: index j stands for wave number j if j < N/2, else j - N.
import mu_gfx;

[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> muStorageImages32[];

struct Push
{
    float4 vp0, vp1, vp2, vp3;
    float4 eye;
    float4 sun;          // direction to it
    uint   h0, src, dst, stage;      // buffer offset; bindless read, written; butterfly stage
    uint   n, log_n, vertical, tiles;
    float  time, patch, chop, pad0;
    uint   view, pad1, pad2, pad3;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
float2 cis(float a) { return float2(cos(a), sin(a)); }

[shader("compute")]
[numthreads(8, 8, 1)]
void compSpectrum(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.n)) return;
    int2   wave = int2(id.xy) - int2(select(id.xy >= pc.n / 2u, int2(pc.n), int2(0)));
    float2 k    = 6.2831853 * float2(wave) / pc.patch;
    float  len  = length(k);
    float4 h0   = muScene.Load<float4>(pc.h0 + (id.x + id.y * pc.n) * 16u);    // h0(k), h0(-k)
    float2 turn = cis(-sqrt(9.81 * len) * pc.time);
    float2 h    = cmul(h0.xy, turn) + cmul(float2(h0.z, -h0.w), float2(turn.x, -turn.y));
    // -i k/|k| H. The wave number -N/2 has no partner +N/2, so a derivative there would not be real: left out.
    bool   odd  = len > 0.0 && all(id.xy != pc.n / 2u);
    float2 dir  = odd ? k / len : float2(0.0);
    float2 dx   = float2(h.y, -h.x) * dir.x, dz = float2(h.y, -h.x) * dir.y;
    muStorageImages32[NonUniformResourceIndex(pc.dst)][id.xy] = float4(h.x - dx.y, h.y + dx.x, dz.x, dz.y);
}

uint reverseBits(uint v, uint bits)
{
    uint r = 0u;
    for (uint i = 0u; i < bits; ++i) { r = (r << 1u) | (v & 1u); v >>= 1u; }
    return r;
}

float4 fetch(uint2 at, uint index)
{
    // The first stage reads its input in bit-reversed order; that is the whole permutation.
    if (pc.stage == 0u) index = reverseBits(index, pc.log_n);
    if (pc.vertical != 0u) at.y = index; else at.x = index;
    return muSampledImages[NonUniformResourceIndex(pc.src)].Load(int3(at, 0));
}

// out[j]     = a[j] + w^j a[j + m]
// out[j + m] = a[j] - w^j a[j + m]      w = e^(+2 pi i / 2m), m = 2^stage, in blocks of 2m
[shader("compute")]
[numthreads(8, 8, 1)]
void compFFT(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.n)) return;
    uint   m     = 1u << pc.stage;
    uint   index = pc.vertical != 0u ? id.y : id.x;
    uint   j     = index & (2u * m - 1u);
    bool   upper = j >= m;
    uint   lo    = index - j + (upper ? j - m : j);
    float4 a = fetch(id.xy, lo), b = fetch(id.xy, lo + m);
    float2 w = cis(3.14159265 * float(upper ? j - m : j) / float(m));
    float4 wb = float4(cmul(w, b.xy), cmul(w, b.zw));
    muStorageImages32[NonUniformResourceIndex(pc.dst)][id.xy] = upper ? a - wb : a + wb;
}

// ---- drawing ------------------------------------------------------------------

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 extra  : TEXCOORD2;   // height, folding (area kept by the horizontal displacement)
};

// The displaced position of grid point c (any integer: the patch repeats).
float3 surfaceAt(int2 c)
{
    int    n = int(pc.n);
    float4 f = muSampledImages[NonUniformResourceIndex(pc.src)].Load(int3(((c % n) + n) % n, 0));
    float  cell = pc.patch / float(pc.n);
    return float3(float(c.x) * cell + pc.chop * f.y, f.x, float(c.y) * cell + pc.chop * f.z);
}

[shader("vertex")]
Varyings vertOcean(uint vertexIndex : SV_VertexID)
{
    static const int2 k_corner[6] = {int2(0, 0), int2(0, 1), int2(1, 0), int2(1, 0), int2(0, 1), int2(1, 1)};
    int  side = int(pc.n * pc.tiles);
    int  quad = int(vertexIndex / 6u);
    int2 c    = int2(quad % side, quad / side) + k_corner[vertexIndex % 6u] - side / 2;
    Varyings r;
    r.world = surfaceAt(c);
    float3 ax = surfaceAt(c + int2(1, 0)) - surfaceAt(c - int2(1, 0));
    float3 az = surfaceAt(c + int2(0, 1)) - surfaceAt(c - int2(0, 1));
    r.normal = normalize(cross(az, ax));
    float cell2 = 2.0 * pc.patch / float(pc.n);
    r.extra = float2(r.world.y, (ax.x * az.z - ax.z * az.x) / (cell2 * cell2));
    r.position = pc.vp0 * r.world.x + pc.vp1 * r.world.y + pc.vp2 * r.world.z + pc.vp3;
    return r;
}

float3 sky(float3 d)
{
    float3 c = lerp(float3(0.62, 0.70, 0.78), float3(0.15, 0.32, 0.62), pow(saturate(d.y), 0.5));
    return c + float3(1.0, 0.85, 0.6) * (8.0 * pow(saturate(dot(d, pc.sun.xyz)), 600.0) + 0.25 * pow(saturate(dot(d, pc.sun.xyz)), 12.0));
}

[shader("fragment")]
float4 fragOcean(Varyings i) : SV_Target0
{
    if (pc.view == 1u) { float h = i.extra.x; return float4(saturate(float3(0.5 + h, 0.5 - abs(h), 0.5 - h)), 1.0); }
    if (pc.view == 2u) { float j = i.extra.y; return float4(j < 0.0 ? float3(1.0, 0.1, 0.1) : float3(saturate(j * 0.5)), 1.0); }
    float3 n = normalize(i.normal);
    float3 v = normalize(pc.eye.xyz - i.world);
    if (dot(n, v) < 0.0) n = reflect(n, v);                      // the far side of a crest: do not reflect the water below
    float  f = 0.02 + 0.98 * pow(1.0 - saturate(dot(n, v)), 5.0);
    float3 water = float3(0.01, 0.07, 0.11) + float3(0.02, 0.12, 0.10) * saturate(0.5 + i.extra.x);
    float3 color = lerp(water, sky(reflect(-v, n)), f);
    color += float3(0.75) * smoothstep(0.45, 0.0, i.extra.y);    // foam where the surface is squeezed together
    float  dist = length(pc.eye.xyz - i.world);
    return float4(lerp(color, float3(0.62, 0.70, 0.78), 1.0 - exp(-dist * 0.004)), 1.0);
}
