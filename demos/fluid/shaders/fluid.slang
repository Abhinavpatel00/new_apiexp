// fluid.slang — Stam's "Stable Fluids" (SIGGRAPH 1999) on a staggered grid.
//
// A cell (i, j) covers [i, i+1] x [j, j+1]. Its texel holds
//   r = horizontal velocity at the middle of its LEFT edge   (i,       j + 0.5)
//   g = vertical velocity at the middle of its BOTTOM edge   (i + 0.5, j)
// (Harlow & Welch's MAC grid). Pressure and dye live at cell centres. With
// velocities on edges, "what flows out of a cell" is four differences of
// stored numbers, and subtracting the pressure gradient can make it exactly
// zero; with everything at centres it cannot.
//
//   compAdvect      move velocity (kind 0) or dye (kind 1) along the velocity
//                   field, then add what the emitters add
//   compDivergence  what flows out of each cell
//   compJacobi      one relaxation step of  laplacian(p) = divergence
//   compRelax       the same in place, one checkerboard colour at a time
//   compProject     velocity -= gradient(p); walls closed
//   compCopy        dst = src
//   fragShow        the picture
//
// Lengths are in cells, times in seconds. The box is closed: no flow through
// its walls.
import mu_gfx;
#include "shaders/format.slang"

// Dye is RGBA16F. Velocity, divergence and pressure are RGBA32F: the
// projection subtracts neighbouring pressures, and 11 bits of mantissa leave
// too little of that difference. One descriptor array, two declared formats.
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> muStorageImages[];
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> muStorageImages32[];

struct Push
{
    uint   src, aux, dst, sampler;    // bindless: read, second read, written (storage), linear clamp
    float  size, dt, time, kind;
    float4 fade;                      // x: velocity kept per step, y: dye kept per step, z: view, w: omega
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

float4 load(uint image, int2 c) { return muSampledImages[NonUniformResourceIndex(image)].Load(int3(c, 0)); }

// Velocity at a point: each component is bilinear around its own edge
// positions. Outside the box the clamped sampler repeats the wall values.
float2 velocityAt(uint image, float2 p)
{
    float u = muSampledImages[NonUniformResourceIndex(image)].SampleLevel(
        muSamplers[NonUniformResourceIndex(pc.sampler)], (p + float2(0.5, 0.0)) / pc.size, 0.0).r;
    float v = muSampledImages[NonUniformResourceIndex(image)].SampleLevel(
        muSamplers[NonUniformResourceIndex(pc.sampler)], (p + float2(0.0, 0.5)) / pc.size, 0.0).g;
    return float2(u, v);
}

// Two jets that swing, one from each side.
struct Jet { float2 at, dir; float3 color; };
Jet jet(uint k)
{
    Jet j;
    float swing = 0.6 * sin(pc.time * (k == 0u ? 0.7 : 0.9) + (float)k * 2.0);
    j.at    = float2(k == 0u ? 0.12 : 0.88, k == 0u ? 0.35 : 0.65) * pc.size;
    j.dir   = float2((k == 0u ? 1.0 : -1.0) * cos(swing), sin(swing));
    j.color = k == 0u ? float3(1.0, 0.35, 0.10) : float3(0.10, 0.45, 1.0);
    return j;
}
static const float JET_RADIUS = 0.035;   // of the box
static const float JET_SPEED  = 0.9;     // boxes per second

[shader("compute")]
[numthreads(8, 8, 1)]
void compAdvect(uint3 id : SV_DispatchThreadID)
{
    int2  c = (int2)id.xy;
    float n = pc.size;
    if (id.x >= (uint)n || id.y >= (uint)n) return;
    float4 result;
    if (pc.kind < 0.5)
    {
        // Each component is carried from where the flow at ITS position came from.
        float2 pu = float2(c) + float2(0.0, 0.5), pv = float2(c) + float2(0.5, 0.0);
        float2 from_u = pu - pc.dt * velocityAt(pc.src, pu);
        float2 from_v = pv - pc.dt * velocityAt(pc.src, pv);
        float2 vel = float2(velocityAt(pc.src, from_u).x, velocityAt(pc.src, from_v).y) * pc.fade.x;
        for (uint k = 0u; k < 2u; ++k)
        {
            Jet j = jet(k);
            float r = JET_RADIUS * n;
            vel.x = lerp(vel.x, j.dir.x * JET_SPEED * n, saturate(1.0 - length(pu - j.at) / r));
            vel.y = lerp(vel.y, j.dir.y * JET_SPEED * n, saturate(1.0 - length(pv - j.at) / r));
        }
        muStorageImages32[NonUniformResourceIndex(pc.dst)][id.xy] = float4(vel, 0.0, 0.0);
        return;
    }
    else
    {
        float2 p    = float2(c) + 0.5;
        float2 from = p - pc.dt * velocityAt(pc.aux, p);
        float3 dye  = muSampledImages[NonUniformResourceIndex(pc.src)].SampleLevel(
            muSamplers[NonUniformResourceIndex(pc.sampler)], from / n, 0.0).rgb * pc.fade.y;
        for (uint k = 0u; k < 2u; ++k)
        {
            Jet j = jet(k);
            dye = lerp(dye, j.color, saturate(1.0 - length(p - j.at) / (JET_RADIUS * n)));
        }
        result = float4(dye, 1.0);
    }
    muStorageImages[NonUniformResourceIndex(pc.dst)][id.xy] = result;
}

// Velocity through the four edges of cell c; walls let nothing through.
float divergenceAt(uint image, int2 c, int n)
{
    float2 here  = load(image, c).rg;
    float  left  = c.x == 0 ? 0.0 : here.x;
    float  below = c.y == 0 ? 0.0 : here.y;
    float  right = c.x == n - 1 ? 0.0 : load(image, c + int2(1, 0)).r;
    float  above = c.y == n - 1 ? 0.0 : load(image, c + int2(0, 1)).g;
    return right - left + above - below;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compDivergence(uint3 id : SV_DispatchThreadID)
{
    int n = (int)pc.size;
    if (id.x >= (uint)n || id.y >= (uint)n) return;
    muStorageImages32[NonUniformResourceIndex(pc.dst)][id.xy] = float4(divergenceAt(pc.src, (int2)id.xy, n), 0.0, 0.0, 0.0);
}

// p = (sum of neighbours - divergence) / (number of neighbours). A wall is
// not a neighbour: nothing is pushed through it.
[shader("compute")]
[numthreads(8, 8, 1)]
void compJacobi(uint3 id : SV_DispatchThreadID)
{
    int  n = (int)pc.size;
    int2 c = (int2)id.xy;
    if (id.x >= (uint)n || id.y >= (uint)n) return;
    float sum = 0.0, count = 0.0;
    if (c.x > 0)     { sum += load(pc.src, c - int2(1, 0)).r; count += 1.0; }
    if (c.x < n - 1) { sum += load(pc.src, c + int2(1, 0)).r; count += 1.0; }
    if (c.y > 0)     { sum += load(pc.src, c - int2(0, 1)).r; count += 1.0; }
    if (c.y < n - 1) { sum += load(pc.src, c + int2(0, 1)).r; count += 1.0; }
    muStorageImages32[NonUniformResourceIndex(pc.dst)][id.xy] = float4((sum - load(pc.aux, c).r) / count, 0.0, 0.0, 0.0);
}

// The same equation, relaxed in place (successive over-relaxation, red-black
// ordering): cells of one colour of a checkerboard only have neighbours of
// the other colour, so a whole colour can be updated at once from values
// that are already new. kind = the colour this pass updates; fade.w = omega,
// how far past the Jacobi value to go.
[shader("compute")]
[numthreads(8, 8, 1)]
void compRelax(uint3 id : SV_DispatchThreadID)
{
    int  n = (int)pc.size;
    int2 c = (int2)id.xy;
    if (id.x >= (uint)n || id.y >= (uint)n) return;
    if (((id.x + id.y) & 1u) != (uint)pc.kind) return;
    float sum = 0.0, count = 0.0;
    if (c.x > 0)     { sum += muStorageImages32[NonUniformResourceIndex(pc.dst)][c - int2(1, 0)].r; count += 1.0; }
    if (c.x < n - 1) { sum += muStorageImages32[NonUniformResourceIndex(pc.dst)][c + int2(1, 0)].r; count += 1.0; }
    if (c.y > 0)     { sum += muStorageImages32[NonUniformResourceIndex(pc.dst)][c - int2(0, 1)].r; count += 1.0; }
    if (c.y < n - 1) { sum += muStorageImages32[NonUniformResourceIndex(pc.dst)][c + int2(0, 1)].r; count += 1.0; }
    float old = muStorageImages32[NonUniformResourceIndex(pc.dst)][c].r;
    float p   = old + pc.fade.w * ((sum - load(pc.aux, c).r) / count - old);
    muStorageImages32[NonUniformResourceIndex(pc.dst)][c] = float4(p, 0.0, 0.0, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compProject(uint3 id : SV_DispatchThreadID)
{
    int  n = (int)pc.size;
    int2 c = (int2)id.xy;
    if (id.x >= (uint)n || id.y >= (uint)n) return;
    float2 vel = load(pc.src, c).rg;
    float  p   = load(pc.aux, c).r;
    vel.x = c.x == 0 ? 0.0 : vel.x - (p - load(pc.aux, c - int2(1, 0)).r);
    vel.y = c.y == 0 ? 0.0 : vel.y - (p - load(pc.aux, c - int2(0, 1)).r);
    muStorageImages32[NonUniformResourceIndex(pc.dst)][id.xy] = float4(vel, 0.0, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compCopy(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)pc.size || id.y >= (uint)pc.size) return;
    muStorageImages[NonUniformResourceIndex(pc.dst)][id.xy] = load(pc.src, (int2)id.xy);
}

float3 heat(float t)
{
    t = saturate(t);
    return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}

// src = dye, aux = velocity; dst = pressure (read here, despite the name).
[shader("fragment")]
float4 fragShow(FullscreenVaryings i) : SV_Target0
{
    float2 uv = i.uv;                 // uv.y = 1 is the top, as in the grid
    float2 p  = uv * pc.size;
    float3 color;
    if (pc.fade.z < 0.5)
    {
        color = muSampledImages[NonUniformResourceIndex(pc.src)].SampleLevel(
            muSamplers[NonUniformResourceIndex(pc.sampler)], uv, 0.0).rgb;
        color = color / (1.0 + 0.4 * color);
    }
    else if (pc.fade.z < 1.5)
        color = mu_srgb_to_linear(heat(length(velocityAt(pc.aux, p)) / (JET_SPEED * pc.size)));
    else
    {
        float pr = muSampledImages[NonUniformResourceIndex(pc.dst)].SampleLevel(
            muSamplers[NonUniformResourceIndex(pc.sampler)], uv, 0.0).r;
        color = mu_srgb_to_linear(pr > 0.0 ? float3(pr * 0.5, 0.0, 0.0) : float3(0.0, 0.0, -pr * 0.5) + 0.02);
    }
    return float4(color, 1.0);
}
