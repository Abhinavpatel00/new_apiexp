// schrodinger.slang — a quantum particle on a grid.
//
// The wave function psi = R + i I of one particle obeys (hbar = m = 1, one
// cell = one unit of length)
//
//     i dpsi/dt = H psi,     H = -1/2 lap + V
//
// which is two real equations: dR/dt = H I and dI/dt = -H R. Visscher's
// scheme (Computers in Physics 5, 1991) keeps R at whole time steps and I at
// half steps and lets them take turns:
//
//     R(t + dt)     = R(t)        + dt H I(t + dt/2)
//     I(t + 3dt/2)  = I(t + dt/2) - dt H R(t + dt)
//
// Each half is one compute pass that reads the other part and updates its own
// in place. The sum over the grid of R(t) R(t + dt) + I(t + dt/2)^2 stays
// constant, so the particle is neither lost nor made.
//
// lap is the fourth-order stencil (-1/12, 4/3, -5/2, 4/3, -1/12) along each
// axis. Outside the grid psi = 0. The scheme is stable while
// dt < 2 / (16/3 + V).
//
// The R image keeps R in x and, in y, the time integral of |psi|^2: what a
// photographic plate at that cell would have collected.
//
//   compSeed    a Gaussian wave packet with momentum (k_x, k_y)
//   compStep    one half step: part 0 updates R from I, part 1 updates I from R
//   fragShade   the picture
import mu_gfx;

[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> muStorageImages[];

struct Push
{
    uint  real, imag, part, slits;           // image indices; part 0 = R, 1 = I; slits 0 (no wall), 1, 2
    uint  nx, ny, wall_x, screen_x;
    float dt, k_x, k_y, wall_v;
    float start_x, start_y, sigma_x, sigma_y;
    float slit_sep, slit_width, absorb, view; // absorb: damping rate at the edge of the grid, 0 = hard walls
    float gain, plate_gain, aspect, wall_thick;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

static const int kBorder = 24;   // width of the absorbing border, cells

// The wall with its slits.
float potential(int2 c)
{
    if (pc.slits == 0u || c.x < (int)pc.wall_x || (float)c.x >= (float)pc.wall_x + pc.wall_thick) return 0.0;
    float y = 0.5 * (float)pc.ny - ((float)c.y + 0.5);   // row 0 is the top
    bool  upper = abs(y - 0.5 * pc.slit_sep) < 0.5 * pc.slit_width;
    bool  lower = abs(y + 0.5 * pc.slit_sep) < 0.5 * pc.slit_width;
    return (upper || (lower && pc.slits == 2u)) ? 0.0 : pc.wall_v;
}

float load(uint image, int2 c)
{
    if (c.x < 0 || c.y < 0 || c.x >= (int)pc.nx || c.y >= (int)pc.ny) return 0.0;
    return muSampledImages[NonUniformResourceIndex(image)].Load(int3(c, 0)).x;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compSeed(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.nx || id.y >= pc.ny) return;
    float2 p = float2(id.xy) + 0.5 - float2(pc.start_x, pc.start_y);
    float  envelope = exp(-0.25 * (p.x * p.x / (pc.sigma_x * pc.sigma_x) + p.y * p.y / (pc.sigma_y * pc.sigma_y)));
    float  phase = pc.k_x * p.x + pc.k_y * p.y;
    muStorageImages[NonUniformResourceIndex(pc.real)][id.xy] = float4(envelope * cos(phase), 0.0, 0.0, 1.0);
    muStorageImages[NonUniformResourceIndex(pc.imag)][id.xy] = float4(envelope * sin(phase), 0.0, 0.0, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compStep(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.nx || id.y >= pc.ny) return;
    int2 c     = (int2)id.xy;
    uint other = pc.part == 0u ? pc.imag : pc.real;
    uint mine  = pc.part == 0u ? pc.real : pc.imag;

    float s   = load(other, c);
    float lap = (4.0 / 3.0) * (load(other, c + int2(1, 0)) + load(other, c - int2(1, 0)) + load(other, c + int2(0, 1)) + load(other, c - int2(0, 1)))
              - (1.0 / 12.0) * (load(other, c + int2(2, 0)) + load(other, c - int2(2, 0)) + load(other, c + int2(0, 2)) + load(other, c - int2(0, 2)))
              - 5.0 * s;
    float h = -0.5 * lap + potential(c) * s;

    float4 texel = muStorageImages[NonUniformResourceIndex(mine)][id.xy];
    texel.x += (pc.part == 0u ? pc.dt : -pc.dt) * h;

    // Near the edge the wave is damped away, so that what leaves does not come back.
    int   edge = min(min(c.x, c.y), min((int)pc.nx - 1 - c.x, (int)pc.ny - 1 - c.y));
    float ramp = saturate(1.0 - (float)edge / (float)kBorder);
    texel.x *= 1.0 - pc.absorb * ramp * ramp * abs(pc.dt);

    if (pc.part == 0u) texel.y += pc.dt * (texel.x * texel.x + s * s);
    muStorageImages[NonUniformResourceIndex(mine)][id.xy] = texel;
}

float3 hue(float turn)
{
    return saturate(abs(frac(turn + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

[shader("fragment")]
float4 fragShade(FullscreenVaryings i) : SV_Target0
{
    // The grid fills the picture's height and is centred across it.
    float2 grid = float2((float)pc.nx, (float)pc.ny);
    float2 at   = float2((i.uv.x - 0.5) * pc.aspect * grid.y + 0.5 * grid.x, (1.0 - i.uv.y) * grid.y);
    if (at.x < 0.0 || at.x >= grid.x) return float4(0.01, 0.01, 0.012, 1.0);
    int2 c = (int2)at;

    // Right of the plate: what the plate has collected, as brightness.
    if (pc.slits != 0u && c.x > (int)pc.screen_x)
    {
        float got = muSampledImages[NonUniformResourceIndex(pc.real)].Load(int3((int)pc.screen_x, c.y, 0)).y * pc.plate_gain;
        float bar = (at.x - (float)pc.screen_x) / (grid.x - (float)pc.screen_x);
        float3 film = float3(0.95, 0.93, 0.85) * (1.0 - exp(-3.0 * got));
        return float4(bar < got ? lerp(film, float3(0.2, 0.9, 0.6), 0.6) : film, 1.0);
    }

    float re = load(pc.real, c), im = load(pc.imag, c);
    float density = re * re + im * im;
    float bright  = 1.0 - exp(-pc.gain * density);
    float3 colour;
    if (pc.view > 1.5)      colour = re > 0.0 ? float3(1.0, 0.45, 0.2) * saturate(re * sqrt(pc.gain)) : float3(0.2, 0.5, 1.0) * saturate(-re * sqrt(pc.gain));
    else if (pc.view > 0.5) colour = float3(bright, bright, bright);
    else                    colour = lerp(float3(1.0, 1.0, 1.0), hue(atan2(im, re) / 6.2831853), 0.85) * bright;   // colour = phase

    if (potential(c) > 0.0) colour = float3(0.32, 0.33, 0.36);
    if (pc.slits != 0u && c.x == (int)pc.screen_x) colour = float3(0.5, 0.5, 0.45);
    return float4(colour * colour, 1.0);   // the colours above were chosen by eye on a display
}
