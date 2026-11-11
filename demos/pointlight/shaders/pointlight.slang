// pointlight.slang — point lights whose brightness stays finite next to a surface.
//
// Cem Yuksel: "Point Light Attenuation Without Singularity" (SIGGRAPH 2020 Talks). A point
// light of intensity I lights a surface at distance d by I fa(d) with fa = 1 / d^2, which has
// no upper limit as d goes to 0. The paper gives the light a radius r that is used for
// nothing but the attenuation (eq. 7):
//
//     fa(d) = 2 / r^2 * (1 - d / sqrt(d^2 + r^2))
//
// the solid angle of a disc of radius r facing the surface, divided by pi r^2. It is 2 / r^2
// at d = 0 and goes to 1 / d^2 for d much larger than r. Written as above it subtracts two
// nearly equal numbers when d is large; the same function without a subtraction is
//
//     fa(d) = 2 / (d^2 + r^2 + d sqrt(d^2 + r^2))
//
// which is what this shader evaluates.
//
// A room open at the front with two balls, one ray per pixel.
//   scene 0   a spot light under the ceiling. Its light is carried on by virtual point lights:
//             `lights` rays from the spot, and a small light where each ends. They sit on the
//             surfaces, so nearby points are at d near 0 (the situation of the paper's
//             figure 1). The picture shows what they carry on; power.z adds the spot's own light.
//   scene 1   three lights placed by hand that fly 0.04 from the floor, the back wall and a
//             ball (closer than r), drawn as small balls.
// view: 0 left half 1 / d^2, right half eq. 7;  1  1 / d^2;  2  eq. 7;  3  the usual
// 1 / (d^2 + r^2 / 2) (eq. 8);  4 no picture: r, g, b = the three functions at d = 8 uv.x.
import mu_gfx;

struct Push
{
    float4 eye_radius;     // xyz camera position, w the lights' radius r
    float4 right_tan;      // xyz camera right, w tan(fov_y / 2)
    float4 up_aspect;      // xyz camera up, w aspect
    float4 forward_time;   // xyz camera forward, w seconds
    float4 spot;           // xyz the spot light's direction, w cosine of its half angle
    float4 power;          // x the spot light's intensity, y of the placed lights, z 1: without the spot's own light
    uint   view, lights, scene, hidden;   // hidden 1: the placed lights are not drawn
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

static const float  PI = 3.14159265;
static const float3 ROOM = float3(2.0, 1.2, 2.0);      // half size; the room is centred at (0, 1.2, 0)
static const float3 SPOT = float3(0.0, 2.25, 0.4);
static const float  NEAR = 0.04;                       // how close the placed lights come to a surface
static const float4 BALL[2] = {float4(-0.9, 0.5, -0.7, 0.5), float4(0.85, 0.35, 0.2, 0.35)};

float attenuation(float d, uint which)
{
    float r = pc.eye_radius.w, d2 = d * d;
    if (which == 1u) return 1.0 / max(d2, 1e-12);
    if (which == 3u) return 1.0 / (d2 + 0.5 * r * r);
    return 2.0 / (d2 + r * r + d * sqrt(d2 + r * r));
}

// The first surface along a ray: distance (negative: none), its normal and its colour.
float trace(float3 o, float3 d, out float3 n, out float3 albedo)
{
    float3 c = o - float3(0.0, ROOM.y, 0.0), inv = 1.0 / d;
    float3 t0 = (-ROOM - c) * inv, t1 = (ROOM - c) * inv, far = max(t0, t1), near = min(t0, t1);
    float  leave = min(far.x, min(far.y, far.z)), enter = max(near.x, max(near.y, near.z)), t = leave;
    n = float3(0.0);
    albedo = float3(0.73);
    if (enter > leave || leave <= 0.0) return -1.0;
    // the wall the ray leaves the room through, seen from inside
    if (leave == far.x) { n = float3(-sign(d.x), 0.0, 0.0); albedo = d.x < 0.0 ? float3(0.63, 0.065, 0.05) : float3(0.14, 0.45, 0.09); }
    else if (leave == far.y) n = float3(0.0, -sign(d.y), 0.0);
    else n = float3(0.0, 0.0, -sign(d.z));
    if (n.z < -0.5) t = -1.0;   // the open front
    for (uint k = 0u; k < 2u; ++k)
    {
        float3 q = o - BALL[k].xyz;
        float  b = dot(q, d), disc = b * b - dot(q, q) + BALL[k].w * BALL[k].w, s = -b - sqrt(max(disc, 0.0));
        if (disc > 0.0 && s > 1e-4 && (t < 0.0 || s < t)) { t = s; n = (q + s * d) / BALL[k].w; albedo = float3(0.8); }
    }
    return t;
}

// 1 if nothing stands between two points of the room (only the balls can).
float visible(float3 a, float3 b)
{
    float3 d = b - a;
    float  len = length(d), open = 1.0;
    d /= len;
    for (uint k = 0u; k < 2u; ++k)
    {
        float3 q = a - BALL[k].xyz;
        float  h = dot(q, d), disc = h * h - dot(q, q) + BALL[k].w * BALL[k].w, s = -h - sqrt(max(disc, 0.0));
        if (disc > 0.0 && s > 1e-3 && s < len - 1e-3) open = 0.0;
    }
    return open;
}

// Light k: where it is, the surface normal there (0: it shines evenly in all directions) and its intensity.
void light(uint k, out float3 at, out float3 normal, out float3 intensity)
{
    if (pc.scene == 1u)
    {
        static const float3 COLOUR[3] = {float3(1.0, 0.55, 0.25), float3(0.3, 0.6, 1.0), float3(0.5, 1.0, 0.45)};
        float time = pc.forward_time.w, a = 0.7 * time;
        at = float3(1.3 * sin(0.5 * time), NEAR, 0.9 + 0.6 * cos(0.37 * time));                 // over the floor
        if (k == 1u) at = float3(1.2 * cos(0.43 * time), 1.3 + 0.6 * sin(0.61 * time), -2.0 + NEAR);   // before the back wall
        if (k == 2u) at = BALL[0].xyz + (BALL[0].w + NEAR) * float3(cos(a) * 0.8, 0.6, sin(a) * 0.8);  // round a ball
        normal = float3(0.0);
        intensity = pc.power.y * COLOUR[k];
        return;
    }
    // A ray from the spot light: evenly over its cone by (k + 1/2) / lights and the bits of k reversed.
    float  u = ((float)k + 0.5) / (float)pc.lights, v = (float)reversebits(k) * 2.3283064e-10;
    float  cosine = 1.0 - u * (1.0 - pc.spot.w), sine = sqrt(1.0 - cosine * cosine);
    float3 axis = pc.spot.xyz, side = normalize(cross(axis, float3(0.0, 0.0, 1.0))), other = cross(axis, side);
    float3 d = axis * cosine + (side * cos(2.0 * PI * v) + other * sin(2.0 * PI * v)) * sine, albedo;
    float  t = trace(SPOT, d, normal, albedo);
    at = SPOT + d * max(t, 0.0);
    // the spot sends I * 2 pi (1 - cos) in all; this ray carries 1 / lights of it, and the
    // surface sends albedo / pi of that back, per steradian, times the cosine
    intensity = t > 0.0 ? albedo * (pc.power.x * 2.0 * (1.0 - pc.spot.w) / (float)pc.lights) : float3(0.0);
}

[shader("fragment")]
float4 fragLight(FullscreenVaryings i) : SV_Target0
{
    float2 ndc = i.uv * 2.0 - 1.0;
    float3 eye = pc.eye_radius.xyz, n, albedo, colour = float3(0.0);
    float3 look = normalize(pc.forward_time.xyz + pc.right_tan.xyz * (ndc.x * pc.up_aspect.w * pc.right_tan.w) + pc.up_aspect.xyz * (ndc.y * pc.right_tan.w));
    uint   which = pc.view == 0u ? (i.uv.x < 0.5 ? 1u : 2u) : pc.view;
    uint   count = pc.scene == 1u ? 3u : pc.lights;

    if (pc.view == 4u)
    {
        float d = 8.0 * i.uv.x;
        return float4(attenuation(d, 1u), attenuation(d, 2u), attenuation(d, 3u), d);
    }

    float t = trace(eye, look, n, albedo);
    if (t > 0.0)
    {
        float3 x = eye + look * t + n * 1e-4;
        for (uint k = 0u; k < count; ++k)
        {
            float3 at, normal, intensity;
            light(k, at, normal, intensity);
            float3 to = at - x;
            float  d = length(to), facing = dot(normal, normal) > 0.5 ? max(dot(normal, -to / d), 0.0) : 1.0;
            float  falls = max(dot(n, to / d), 0.0) * facing;
            if (falls > 0.0) colour += intensity * (falls * attenuation(d, which) * visible(x, at + normal * 1e-3));
        }
        if (pc.scene == 0u && pc.power.z < 0.5)
        {
            // the spot light itself: far from every surface, so 1 / d^2
            float3 to = SPOT - x;
            float  d = length(to), inside = smoothstep(pc.spot.w, pc.spot.w + 0.04, dot(-to / d, pc.spot.xyz));
            colour += pc.power.x * inside * max(dot(n, to / d), 0.0) / (d * d) * visible(x, SPOT);
        }
        colour *= albedo / PI;
    }
    if (pc.scene == 1u && pc.hidden == 0u)
        for (uint k = 0u; k < 3u; ++k)
        {
            // where the light is: a ball of radius 0.025
            float3 at, normal, intensity;
            light(k, at, normal, intensity);
            float3 q = eye - at;
            float  b = dot(q, look), disc = b * b - dot(q, q) + 0.000625, s = -b - sqrt(max(disc, 0.0));
            if (disc > 0.0 && s > 0.0 && (t < 0.0 || s < t)) { colour = 6.0 * intensity / max(pc.power.y, 1e-6); t = s; }
        }
    if (pc.view == 0u && abs(i.uv.x - 0.5) < 0.0015) colour = float3(0.8);
    return float4(colour, 1.0);
}
