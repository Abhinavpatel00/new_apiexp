// turing.slang — two chemicals that react and diffuse (the Gray-Scott model).
//
// A grid cell holds the amounts u and v of two chemicals. Each step,
//
//     u += dt (Du lap(u) - u v^2 + F (1 - u))
//     v += dt (Dv lap(v) + u v^2 - (F + k) v)
//
// v feeds on u (u + 2v -> 3v), u is topped up at rate F, v drains away at
// rate F + k, and both spread, u twice as fast. From almost nothing this
// grows spots, stripes, mazes or cells that divide, depending on F and k.
// The idea is Turing's (1952); Turk and Witkin and Kass made textures this
// way at SIGGRAPH 1991, with other reactions than this one.
//
// lap() is the nine-point stencil: 0.2 for the four neighbours, 0.05 for the
// four diagonals, -1 for the cell. The grid wraps, so the texture tiles.
//
//   compSeed    the starting state
//   compStep    one step, from one image into the other
//   fragShade   the picture
import mu_gfx;

[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> muStorageImages[];

struct Push
{
    uint   src, dst, n, flags;     // flags: 1 = F and k vary across the grid, 2 = diffusion only
    float  feed, kill, du, dv;
    float  dt;
    uint   seed, mode_x, mode_y;   // seed: 0 blobs, 1 one Fourier mode, 2 u = 1 and v = 0 everywhere
    float4 look;                   // x view, y aspect, z tiles across the picture's height
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

float2 load(int2 c)
{
    int n = (int)pc.n;
    // The grid wraps, except as a map: there the two edges have different F and k.
    c = (pc.flags & 1u) != 0u ? clamp(c, 0, n - 1) : (c + n) % n;
    return muSampledImages[NonUniformResourceIndex(pc.src)].Load(int3(c, 0)).xy;
}

// F and k of a cell. With flag 1, k runs left to right and F bottom to top,
// which lays every kind of pattern the model has side by side (Pearson 1993).
float2 feedKill(int2 c)
{
    if ((pc.flags & 1u) == 0u) return float2(pc.feed, pc.kill);
    float2 at = (float2(c) + 0.5) / (float)pc.n;
    return float2(lerp(0.070, 0.012, at.y), lerp(0.052, 0.068, at.x));   // row 0 is the top
}

uint hash(uint x)
{
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compSeed(uint3 id : SV_DispatchThreadID)
{
    int   n = (int)pc.n;
    int2  c = (int2)id.xy;
    float2 state = float2(1.0, 0.0);
    if (pc.seed == 0u)
    {
        // Patches of v scattered over a grid full of u, and a little noise.
        uint patches = (pc.flags & 1u) != 0u ? 500u : 28u;   // every region of the map needs a start
        for (uint k = 0u; k < patches; ++k)
        {
            int2  centre = int2((int)(hash(k * 3u + 1u) % pc.n), (int)(hash(k * 3u + 2u) % pc.n));
            int2  d = abs(c - centre);
            d = min(d, n - d);
            float radius = (pc.flags & 1u) != 0u ? 2.5 : 3.0 + (float)(hash(k * 3u + 3u) % 5u);
            if (dot(float2(d), float2(d)) < radius * radius) state = float2(0.5, 0.25);
        }
        state.x -= 0.02 * (float)(hash(id.x + id.y * pc.n) & 0xffffu) / 65535.0;
    }
    else if (pc.seed == 1u)
    {
        float wave = cos(6.2831853 * ((float)(pc.mode_x * id.x) + (float)(pc.mode_y * id.y)) / (float)pc.n);
        state = float2(0.5 + 0.25 * wave, 0.5 + 0.25 * wave);
    }
    muStorageImages[NonUniformResourceIndex(pc.dst)][id.xy] = float4(state, 0.0, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compStep(uint3 id : SV_DispatchThreadID)
{
    int2   c = (int2)id.xy;
    float2 s = load(c);
    float2 lap = 0.2 * (load(c + int2(1, 0)) + load(c - int2(1, 0)) + load(c + int2(0, 1)) + load(c - int2(0, 1)))
               + 0.05 * (load(c + int2(1, 1)) + load(c + int2(-1, 1)) + load(c + int2(1, -1)) + load(c - int2(1, 1)))
               - s;
    float2 fk    = (pc.flags & 2u) != 0u ? float2(0.0, 0.0) : feedKill(c);
    float  react = (pc.flags & 2u) != 0u ? 0.0 : s.x * s.y * s.y;
    float  u = s.x + pc.dt * (pc.du * lap.x - react + fk.x * (1.0 - s.x));
    float  v = s.y + pc.dt * (pc.dv * lap.y + react - (fk.x + fk.y) * s.y);
    muStorageImages[NonUniformResourceIndex(pc.dst)][id.xy] = float4(u, v, 0.0, 1.0);
}

// The state between cell centres, wrapping.
float2 smooth(float2 cells)
{
    float2 p = cells - 0.5;
    int2   c = (int2)floor(p);
    float2 f = p - floor(p);
    return lerp(lerp(load(c), load(c + int2(1, 0)), f.x), lerp(load(c + int2(0, 1)), load(c + int2(1, 1)), f.x), f.y);
}

[shader("fragment")]
float4 fragShade(FullscreenVaryings i) : SV_Target0
{
    float  n     = (float)pc.n;
    float  tiles = pc.look.z;
    float2 at    = (i.uv - 0.5) * float2(pc.look.y, 1.0) * tiles + 0.5;      // 0..1 = one tile
    if ((pc.flags & 1u) != 0u && (at.x < 0.0 || at.x > 1.0)) return float4(0.01, 0.01, 0.012, 1.0);
    float2 cells = at * n;
    float2 s = smooth(cells);
    if (pc.look.x > 1.5) return float4(s.x, s.x, s.x, 1.0);
    if (pc.look.x > 0.5) return float4(s.y, s.y, s.y, 1.0) * 2.5;

    // v as a height field, lit from the upper left.
    float  e  = 0.75;
    float  dx = smooth(cells + float2(e, 0.0)).y - smooth(cells - float2(e, 0.0)).y;
    float  dy = smooth(cells + float2(0.0, e)).y - smooth(cells - float2(0.0, e)).y;
    float3 normal = normalize(float3(-dx, -dy, 2.0 * e / 14.0));
    float3 light  = normalize(float3(-0.5, 0.6, 0.65));
    float  t = saturate(s.y * 3.0);
    float3 deep = float3(0.012, 0.035, 0.075), mid = float3(0.05, 0.42, 0.45), top = float3(0.95, 0.80, 0.50);
    float3 base = lerp(lerp(deep, mid, smoothstep(0.0, 0.45, t)), top, smoothstep(0.4, 0.95, t));
    float  diffuse  = saturate(dot(normal, light));
    float  specular = pow(saturate(dot(normal, normalize(light + float3(0.0, 0.0, 1.0)))), 48.0);
    return float4(base * (0.25 + 0.95 * diffuse) + specular * 0.6 * smoothstep(0.05, 0.3, t), 1.0);
}
