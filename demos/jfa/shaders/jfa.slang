// jfa.slang — distances to an arbitrary shape by jump flooding.
//
// Rong & Tan, "Jump Flooding in GPU with Applications to Voronoi Diagram and
// Distance Transform", I3D 2006. Every cell remembers the closest cell of the
// shape it has heard of. A pass with step s asks the eight cells s away (and
// itself) what they know and keeps the closest answer. Steps halve from half
// the grid down to 1, so news from any cell can reach any other in
// log2(size) passes. It is not exact: a cell can miss its true closest cell
// when none of the cells it asks has that one as ITS closest.
//
// One texel carries two answers, so one flood gives a signed distance:
//   rg = closest cell INSIDE the shape   (-1, -1: none known)
//   ba = closest cell OUTSIDE the shape
//
//   compInit   draws the shape: each cell names itself as inside or outside
//   compFlood  one pass with step pc.step
//   fragShow   distance bands / which cell is closest / the outline
import mu_gfx;
#include "shaders/format.slang"

[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> muStorageImages[];

struct Push
{
    uint   src, dst, view, pad;
    float2 size;
    float  step, time;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

float hash1(float n) { return frac(sin(n * 127.1) * 43758.5453); }

// The shape, in cells: a ring, a turning bar, a wavy line one cell thick,
// and scattered dots. Negative inside. Anything goes; that is the point.
float shape(float2 p)
{
    float2 size = pc.size;
    float  t    = pc.time;
    float2 c0   = size * float2(0.30 + 0.05 * sin(t * 0.5), 0.55);
    float  ring = abs(length(p - c0) - size.y * 0.22) - size.y * 0.035;

    float2 q   = p - size * float2(0.68, 0.42);
    float  a   = t * 0.3 + 0.4;
    q          = float2(cos(a) * q.x + sin(a) * q.y, -sin(a) * q.x + cos(a) * q.y);
    float2 e   = abs(q) - size.y * float2(0.30, 0.045);
    float  bar = length(max(e, 0.0)) + min(max(e.x, e.y), 0.0);

    float wave = abs(p.y - size.y * (0.86 + 0.05 * sin(p.x * 0.035 + t))) - 0.5;
    wave       = max(wave, abs(p.x - size.x * 0.5) - size.x * 0.42);

    float dots = 1e9;
    for (int i = 0; i < 24; ++i)
    {
        float2 c = float2(hash1((float)i + 0.5), hash1((float)i + 31.5)) * size;
        dots     = min(dots, length(p - c) - (1.0 + 2.5 * hash1((float)i + 77.0)));
    }
    return min(min(ring, bar), min(wave, dots));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compInit(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)pc.size.x || id.y >= (uint)pc.size.y) return;
    float2 self   = float2(id.xy);
    bool   inside = shape(self + 0.5) < 0.0;
    muStorageImages[NonUniformResourceIndex(pc.dst)][id.xy] =
        inside ? float4(self, -1.0, -1.0) : float4(-1.0, -1.0, self);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compFlood(uint3 id : SV_DispatchThreadID)
{
    int2 size = (int2)pc.size;
    int2 c    = (int2)id.xy;
    if (c.x >= size.x || c.y >= size.y) return;
    float2 self = float2(c);
    float4 best = muSampledImages[NonUniformResourceIndex(pc.src)].Load(int3(c, 0));
    float2 d2   = float2(best.x < 0.0 ? 1e12 : dot(best.xy - self, best.xy - self),
                         best.z < 0.0 ? 1e12 : dot(best.zw - self, best.zw - self));
    int s = (int)pc.step;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
        {
            if (i == 0 && j == 0) continue;
            int2 n = c + int2(i, j) * s;
            if (n.x < 0 || n.y < 0 || n.x >= size.x || n.y >= size.y) continue;
            float4 other = muSampledImages[NonUniformResourceIndex(pc.src)].Load(int3(n, 0));
            float  a = dot(other.xy - self, other.xy - self), b = dot(other.zw - self, other.zw - self);
            if (other.x >= 0.0 && a < d2.x) { d2.x = a; best.xy = other.xy; }
            if (other.z >= 0.0 && b < d2.y) { d2.y = b; best.zw = other.zw; }
        }
    muStorageImages[NonUniformResourceIndex(pc.dst)][id.xy] = best;
}

float3 cellColor(float2 cell)
{
    float n = cell.x * 0.37 + cell.y * 17.3;
    return 0.25 + 0.75 * float3(hash1(n), hash1(n + 3.1), hash1(n + 7.7));
}

[shader("fragment")]
float4 fragShow(FullscreenVaryings i) : SV_Target0
{
    float2 p    = i.uv * pc.size;
    int2   c    = min((int2)p, (int2)pc.size - 1);
    float4 near = muSampledImages[NonUniformResourceIndex(pc.src)].Load(int3(c, 0));
    float2 self = float2(c);
    // Distance between cell centres, moved half a cell so the zero line runs
    // between an inside cell and its outside neighbour.
    float to_inside  = near.x < 0.0 ? 1e6 : length(near.xy - self);
    float to_outside = near.z < 0.0 ? 1e6 : length(near.zw - self);
    float d          = to_inside > 0.0 ? to_inside - 0.5 : 0.5 - to_outside;
    float3 color;
    if (pc.view == 1u)
    {
        // Which cell of the shape is closest: a Voronoi diagram of its cells.
        color = cellColor(near.xy) * (to_inside > 0.0 ? 1.0 : 0.2);
    }
    else if (pc.view == 2u)
    {
        // What the field is for: an outline and a glow of any width.
        float glow = exp(-max(d, 0.0) * 0.06);
        color = float3(0.02, 0.03, 0.05) + float3(0.1, 0.5, 1.0) * glow * glow * step(0.0, d)
              + float3(1.0, 0.9, 0.6) * saturate(1.0 - abs(d - 6.0) / 1.5) + float3(0.9) * step(d, 0.0);
    }
    else
    {
        float3 base = d > 0.0 ? float3(0.85, 0.45, 0.15) : float3(0.2, 0.5, 0.9);
        float  band = 0.75 + 0.25 * cos(d * 0.785);            // one band per 8 cells
        color = base * band * (1.0 - exp(-abs(d) * 0.15) * 0.6);
        color = lerp(float3(1.0), color, saturate(abs(d) / 1.0));
    }
    return float4(mu_srgb_to_linear(color), 1.0);
}
