// blackhole.slang — what a camera sees next to a Schwarzschild black hole.
//
// Units: G = c = 1 and lengths are in units of the hole's mass M, so the
// horizon is at r = 2, light can circle at r = 3 and matter can circle down
// to r = 6.
//
// The path of light. In the plane of its orbit, with u = 1 / r, a light ray
// obeys
//
//     d2u / dphi2 + u = 3 M u^2
//
// (the right side is all that general relativity adds: without it the
// solution is a straight line). That is the orbit equation of a particle
// pulled towards the centre by a force 3 M h^2 / r^4, h being its angular
// momentum |x cross v|, which the pull does not change. So a ray is traced
// by stepping a particle under that force in ordinary 3-D coordinates; no
// orbital plane has to be found and nothing special happens at the horizon.
//
// Output (RGBA16F, or RGBA32F for --check):
//   view 0, 1   rgb = scene-linear colour
//   view 2      rgb = the direction the ray left in (0 if it did not leave)
//   view 3      r = redshift factor g of the first disc crossing, g = its radius, b = steps
//   a           0 left, 1 fell in, 2 ran out of steps; + 4 if it crossed the disc
import mu_gfx;
#include "shaders/noise.slang"

struct Push
{
    float4 eye_time;         // xyz camera position, w seconds
    float4 right_tan;        // xyz camera right, w tan(fov_y / 2)
    float4 up_aspect;        // xyz camera up, w aspect
    float4 forward_escape;   // xyz camera forward, w radius beyond which a ray has left
    float4 disc;             // x inner radius, y outer radius, z brightness, w mass M (0 = flat space)
    float4 look;             // x disc temperature at its hottest (kelvin), y step as a fraction of r
    uint   view, max_steps, flags, pad;   // flags: 1 disc, 2 Doppler + redshift, 4 lat/long sky
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

static const float PI = 3.14159265;

float3 pull(float3 x, float h2)
{
    float r2 = dot(x, x);
    return x * (-3.0 * pc.disc.w * h2 / (r2 * r2 * sqrt(r2)));
}

// Colour of a black body, scene-linear, brightest channel near 1
// (Tanner Helland's fit to the CIE data, 1000 K .. 40000 K).
float3 blackbody(float kelvin)
{
    float t = clamp(kelvin, 1000.0, 40000.0) / 100.0;
    float r = t <= 66.0 ? 1.0 : saturate(1.292936 * pow(t - 60.0, -0.1332048));
    float g = t <= 66.0 ? saturate(0.390082 * log(t) - 0.631841) : saturate(1.129891 * pow(t - 60.0, -0.0755148));
    float b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : saturate(0.543207 * log(t - 10.0) - 1.196254));
    return pow(float3(r, g, b), 2.2);
}

// Streaks that go round with the gas. Inner orbits are faster, so a pattern
// carried along forever winds up into noise; two copies restart in turn and
// cross-fade instead.
float streaks(float2 p, float radius, float omega, float time)
{
    const float PERIOD = 48.0;
    float sum = 0.0;
    for (uint k = 0u; k < 2u; ++k)
    {
        float phase = frac(time / PERIOD + 0.5 * (float)k);
        float angle = atan2(p.y, p.x) - omega * (phase - 0.5) * PERIOD * 6.0;
        float3 q = float3(radius * 0.9, cos(angle) * 1.6, sin(angle) * 1.6) + float3(0.0, 0.0, 17.0 * (float)k);
        float n = 0.55 * mu_value_noise3(q) + 0.30 * mu_value_noise3(q * 2.1) + 0.15 * mu_value_noise3(q * 4.3);
        sum += n * (1.0 - abs(2.0 * phase - 1.0));
    }
    return sum;
}

// The sky a ray ends in: stars on the faces of a cube, a band of haze, or
// (flag 4) lines of latitude and longitude, which show the bending best.
float3 sky(float3 d)
{
    if ((pc.flags & 4u) != 0u)
    {
        float lat = asin(clamp(d.y, -1.0, 1.0)), lon = atan2(d.z, d.x);
        float2 cell = float2(lon, lat) * (18.0 / PI);
        float2 line = abs(frac(cell) - 0.5);
        float  check = fmod(floor(cell.x) + floor(cell.y) + 64.0, 2.0);
        float3 base = lerp(float3(0.02, 0.05, 0.12), float3(0.10, 0.04, 0.02), check);
        return max(line.x, line.y) > 0.47 ? float3(0.9, 0.9, 0.8) : base;
    }
    float3 a = abs(d);
    float2 uv;
    uint   face;
    if (a.x >= a.y && a.x >= a.z)      { uv = d.yz / a.x; face = d.x > 0.0 ? 0u : 1u; }
    else if (a.y >= a.z)               { uv = d.xz / a.y; face = d.y > 0.0 ? 2u : 3u; }
    else                               { uv = d.xy / a.z; face = d.z > 0.0 ? 4u : 5u; }
    float3 color = float3(0.0, 0.0, 0.0);
    for (uint layer = 0u; layer < 2u; ++layer)
    {
        float  cells = layer == 0u ? 36.0 : 110.0;
        float2 g = (uv * 0.5 + 0.5) * cells;
        float2 c = floor(g);
        float3 rnd = mu_hash03(uint3((uint)c.x, (uint)c.y, face + 6u * layer));
        float2 at = c + 0.15 + 0.7 * rnd.xy;
        float  dist = length(g - at) / cells;
        float  bright = pow(rnd.z, layer == 0u ? 7.0 : 5.0) * (layer == 0u ? 9.0 : 1.5);
        float3 tint = blackbody(3000.0 + 9000.0 * frac(rnd.z * 91.7));
        color += tint * bright * exp(-dist * dist * (layer == 0u ? 5.0e5 : 1.2e6));
    }
    // A hazy band, tilted against the disc.
    float3 axis = normalize(float3(0.35, 0.8, 0.5));
    float  band = exp(-pow(dot(d, axis), 2.0) * 14.0);
    float  haze = mu_value_noise3(d * 3.0) * 0.6 + mu_value_noise3(d * 9.0) * 0.4;
    color += band * haze * haze * float3(0.030, 0.034, 0.055);
    return color;
}

[shader("fragment")]
float4 fragTrace(FullscreenVaryings i) : SV_Target0
{
    float2 ndc  = i.uv * 2.0 - 1.0;
    float3 eye  = pc.eye_time.xyz;
    float3 look = normalize(pc.forward_escape.xyz + pc.right_tan.xyz * (ndc.x * pc.up_aspect.w * pc.right_tan.w)
                            + pc.up_aspect.xyz * (ndc.y * pc.right_tan.w));
    float  mass = pc.disc.w;

    // The camera hovers at a fixed radius. Its rulers along the radius are
    // stretched by 1 / sqrt(1 - 2M/r) compared with the coordinate r, so the
    // direction it sees a ray arrive from is not the ray's direction in
    // coordinates: the radial part shrinks by sqrt(1 - 2M/r).
    float  r0     = length(eye);
    float3 radial = eye / r0;
    float  f0     = 1.0 - 2.0 * mass / r0;
    float  along  = dot(look, radial);
    float3 x = eye;
    float3 v = normalize(radial * (along * sqrt(f0)) + (look - radial * along));
    float3 turn = cross(x, v);
    float  h2   = dot(turn, turn);
    // Angular momentum per unit energy of the light, about the disc's axis.
    // The light travels against the ray we trace, hence the sign.
    float  light_spin = -cross(eye, look).y / sqrt(f0);

    float3 color = float3(0.0, 0.0, 0.0);
    float  through = 1.0;            // how much of what lies behind still shows
    float  first_g = 0.0, first_radius = 0.0;
    uint   status = 2u, crossed = 0u, steps = 0u;
    for (; steps < pc.max_steps; ++steps)
    {
        float r = length(x);
        if (r < 2.0 * mass) { status = 1u; break; }
        if (r > pc.forward_escape.w && dot(x, v) > 0.0) { status = 0u; break; }

        // Fourth-order Runge-Kutta, step in proportion to the radius.
        float  dl  = pc.look.y * r;
        float3 k1x = v,                   k1v = pull(x, h2);
        float3 k2x = v + 0.5 * dl * k1v,  k2v = pull(x + 0.5 * dl * k1x, h2);
        float3 k3x = v + 0.5 * dl * k2v,  k3v = pull(x + 0.5 * dl * k2x, h2);
        float3 k4x = v + dl * k3v,        k4v = pull(x + dl * k3x, h2);
        float3 nx = x + (dl / 6.0) * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
        float3 nv = v + (dl / 6.0) * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);

        // The disc is the plane y = 0 between two radii.
        if ((pc.flags & 1u) != 0u && x.y * nx.y < 0.0)
        {
            float3 p = lerp(x, nx, x.y / (x.y - nx.y));
            float  radius = length(p.xz);
            if (radius > pc.disc.x && radius < pc.disc.y)
            {
                // Gas on a circular orbit: angular velocity sqrt(M / r^3), clock
                // slowed by sqrt(1 - 3M/r). g = frequency seen / frequency sent.
                float omega = sqrt(max(mass, 1e-6) / (radius * radius * radius));
                float g = (pc.flags & 2u) != 0u
                              ? sqrt(1.0 - 3.0 * mass / radius) / (sqrt(f0) * (1.0 - omega * light_spin))
                              : 1.0;
                // A thin disc radiates T^4 ~ r^-3 (1 - sqrt(r_in / r)), hottest at 49/36 r_in.
                float rel = radius / pc.disc.x;
                float t4  = (1.0 - sqrt(1.0 / rel)) / (rel * rel * rel) / 0.05665;
                float temperature = pc.look.x * pow(max(t4, 1e-6), 0.25);
                float gas = streaks(p.xz, radius, omega, pc.eye_time.w);
                float edge = smoothstep(pc.disc.x, pc.disc.x * 1.08, radius) * (1.0 - smoothstep(pc.disc.y * 0.75, pc.disc.y, radius));
                float opacity = edge * saturate(0.45 + 1.0 * gas);
                // Brightness seen goes as g^4; the colour is that of a body at g T.
                float3 light = blackbody(g * temperature) * (t4 * g * g * g * g) * pc.disc.z * (0.3 + 1.4 * gas);
                color   += through * opacity * light;
                through *= 1.0 - opacity;
                if (crossed == 0u) { first_g = g; first_radius = radius; }
                crossed = 1u;
            }
        }
        x = nx;
        v = nv;
    }
    float3 leaving = status == 0u ? normalize(v) : float3(0.0, 0.0, 0.0);
    if (status == 0u) color += through * sky(leaving);

    float alpha = (float)status + 4.0 * (float)crossed;
    if (pc.view == 2u) return float4(leaving, alpha);
    if (pc.view == 3u) return float4(first_g, first_radius, (float)steps, alpha);
    return float4(color, alpha);
}
