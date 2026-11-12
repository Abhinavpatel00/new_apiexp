// fs_common.slang — the world model the flight simulator's shaders and its
// flight code share.
//
// One set of constants and one height field, included by every stage that has
// to agree with another about where the ground is. The C side of the same
// contract is demos/flightsim/world.c on mu_shader_noise.h; --check compares
// the two over the same lattice, because a simulator whose CPU terrain is not
// its GPU terrain crashes into hills the player can see through.
//
// Coordinates: metres, right-handed, +X east, +Y up, +Z south. North is -Z.
// The planet is a sphere of FS_PLANET_R whose centre sits at (0, -R, 0), so
// "height above sea level" is a distance from that centre and the surface near
// the origin is the y = 0 plane. That one choice is what lets the cloud layer
// curve away to the horizon while the terrain stays a cheap analytic function
// of x and z.
#ifndef FS_COMMON_SLANG
#define FS_COMMON_SLANG

#include "shaders/noise.slang"      // mu_lib: mu_fbm4, mu_ridged4, mu_value_noise2/3, mu_cell_hash3
#include "shaders/common.slang"     // mu_lib: mu_pcg3d, mu_ign, mu_bayer4, MU_PI

// ---- world scale ----------------------------------------------------------

static const float FS_PLANET_R   = 6371000.0;   // m; Earth
static const float FS_SEA_LEVEL  = 0.0;         // m
static const float FS_FAR        = 260000.0;    // m; the far plane, past the last terrain ring
static const float FS_FOG_DIST   = 90000.0;     // m; aerial perspective takes over here

// The cloud shell. A flight simulator spends its time inside these numbers, so
// they are not decoration: base below cruise, top above it, and both are
// heights above sea level on the curved planet.
static const float FS_CLOUD_BASE = 1350.0;      // m
static const float FS_CLOUD_TOP  = 5600.0;      // m

// The weather map tiles the world on this period and is scrolled by the wind,
// so weather moves without anything being re-baked.
static const float FS_WEATHER_TILE = 49152.0;   // m; 512 texels -> 96 m per texel

// The density volumes tile horizontally on their own periods. Vertical is the
// cloud layer itself, so a volume texel spans the whole deck in 64 steps.
static const float FS_BASE_TILE    = 9216.0;    // m; 128 texels -> 72 m per texel
static const float FS_DETAIL_TILE  = 1152.0;    // m; 32 texels -> 36 m per texel
// Volume edges in texels, mirroring BASE_HI_DIM / BASE_LO_DIM / DETAIL_DIM in
// main.c. Powers of two, which is what lets the march wrap u and w with a mask
// instead of a sampler address mode.
static const uint  FS_VOL_HI_DIM     = 128u;
static const uint  FS_VOL_LO_DIM     = 32u;
static const uint  FS_VOL_DETAIL_DIM = 32u;

// ---- integer helpers ------------------------------------------------------

// A positive modulo. Slang's % follows the sign of the left operand, and the
// periodic lattice needs a wrap, not a remainder.
int fsModI(int a, int p) { int m = a % p; return m < 0 ? m + p : m; }

// ---- terrain --------------------------------------------------------------

// The height field, in metres above sea level. Three octaves of work:
//   continents  mu_fbm4 at 1/60 km      sets where land is and where ocean is
//   ranges      mu_ridged4, gated by the continent mask, so mountains only
//               appear where the continent says so instead of everywhere
//   relief      two more mu_fbm4 at 1/3 km and 1/400 m for hills and ripples
//
// This is evaluated three times per terrain vertex (once for the position, two
// finite differences for the normal), so it is deliberately four octaves deep
// and no deeper: the cost is 12 value-noise evaluations, 48 hashes, per call.
float fsTerrainHeight(float2 xz)
{
    float2 p     = xz * (1.0 / 60000.0);
    float  cont  = mu_fbm4(p, 2.0, 0.5);                        // -1 .. 1
    float  mask  = saturate(cont * 2.1 + 0.28);                 // land above ~0
    float  ridge = mu_ridged4(p * 2.7 + 13.1, 2.0, 0.5);        //  0 .. 1
    float  h     = cont * 240.0 + mask * ridge * 2250.0;
    h += mu_fbm4(xz * (1.0 / 3000.0) + 41.3, 2.0, 0.52) * 105.0;
    h += mu_fbm4(xz * (1.0 /  400.0) +  7.7, 2.0, 0.55) *  10.0;
    // Ocean floor: clamp the basin so deep water does not run away to -inf and
    // so the shoreline stays a single crossing of y = 0.
    return max(h, -420.0);
}

// Terrain normal by central difference. The step scales with the clipmap ring
// so the far rings, whose vertices are kilometres apart, do not produce a
// normal from two points that are the same texel of the coarse octaves.
float3 fsTerrainNormal(float2 xz, float step)
{
    float hL = fsTerrainHeight(xz - float2(step, 0.0));
    float hR = fsTerrainHeight(xz + float2(step, 0.0));
    float hD = fsTerrainHeight(xz - float2(0.0, step));
    float hU = fsTerrainHeight(xz + float2(0.0, step));
    return normalize(float3(hL - hR, 2.0 * step, hD - hU));
}

// ---- atmosphere -----------------------------------------------------------

// Single-scatter sky: a Rayleigh-shaped gradient tinted by the sun's elevation,
// a Henyey-Greenstein Mie lobe around the sun, and a darkened ground plane.
// Cheap enough to be a full-screen pass every frame, and it gives the cloud
// lighting a sun colour that matches the sky the clouds sit in — which matters
// more than getting either one exactly right.
float3 fsSkyRadiance(float3 dir, float3 sun_dir, float sun_intensity, float turbidity)
{
    float  up      = dir.y;
    float  cos_sun = dot(dir, sun_dir);
    float  sun_elev = sun_dir.y;

    // Day-to-dusk palette driven by the sun's elevation alone.
    float  dusk    = saturate(1.0 - sun_elev * 3.2);
    float3 zenith  = lerp(float3(0.10, 0.28, 0.62), float3(0.045, 0.075, 0.20), dusk);
    float3 horizon = lerp(float3(0.62, 0.74, 0.88), float3(0.95, 0.48, 0.22), dusk * dusk);
    zenith  *= lerp(1.0, 1.35, turbidity * 0.5);
    horizon *= lerp(1.0, 1.20, turbidity * 0.5);

    // Rayleigh-ish gradient: the horizon is bright because the path is long.
    float  g       = pow(saturate(1.0 - abs(up)), 5.0);
    float3 sky     = lerp(zenith, horizon, g);

    // Below the horizon the sky becomes the ground haze, not a mirror.
    sky = lerp(sky, float3(0.20, 0.22, 0.24) * horizon, saturate(-up * 6.0));

    // Mie: the sun's disc and its forward glow.
    float  henyey  = 0.0795774 * (1.0 - 0.36 * 0.36) /
                     pow(max(1.0 + 0.36 * 0.36 - 2.0 * 0.36 * cos_sun, 1e-4), 1.5);
    float  disc    = smoothstep(0.99955, 0.99975, cos_sun);
    sky += sun_intensity * (henyey * float3(1.0, 0.92, 0.80) * 0.35 + disc * float3(28.0, 25.0, 21.0));

    // A second, wider lobe sells the bloom around a low sun.
    sky += sun_intensity * pow(max(cos_sun, 0.0), 8.0) * float3(0.16, 0.10, 0.05) * dusk;
    return max(sky, 0.0);
}

// Aerial perspective: blend a surface colour toward the sky along its view
// direction. This is what makes 80 km of terrain readable as distance instead
// of as a flat texture, and it has to be applied to terrain and water alike or
// the shoreline shows a seam.
float3 fsAerialPerspective(float3 colour, float3 dir, float distance, float3 sun_dir,
                           float sun_intensity, float turbidity)
{
    float  t    = 1.0 - exp(-distance / FS_FOG_DIST);
    // Looking at the sun thickens the haze; looking away thins it.
    t *= lerp(0.75, 1.45, saturate(dot(dir, sun_dir) * 0.5 + 0.5));
    float3 fog  = fsSkyRadiance(dir, sun_dir, sun_intensity, turbidity);
    return lerp(colour, fog, saturate(t));
}

// ---- cloud layer geometry -------------------------------------------------

// Intersect a ray with the spherical shell [FS_CLOUD_BASE, FS_CLOUD_TOP] around
// the planet centre. Returns false when the ray misses the shell entirely,
// which is the single cheapest exit in the whole cloud pass: on a typical
// frame most of the pixels above the horizon take it after four flops.
//
//   t0, t1 : entry and exit distances along dir, in metres
bool fsShellIntersect(float3 origin, float3 dir, out float t0, out float t1)
{
    // Every early return below leaves the caller with a false and nothing else
    // to read, so these values are never consumed. They are written anyway:
    // an out parameter that a path does not initialise is the kind of thing a
    // later edit turns into a read, and the compiler is right to flag it.
    t0 = 0.0;
    t1 = 0.0;
    float3 centre = float3(0.0, -FS_PLANET_R, 0.0);
    float3 o      = origin - centre;
    float  b      = dot(o, dir);
    float  c      = dot(o, o);

    // Outer sphere.
    float  r_out  = FS_PLANET_R + FS_CLOUD_TOP;
    float  disc   = b * b - (c - r_out * r_out);
    if (disc < 0.0) return false;
    float  sq     = sqrt(disc);
    t0 = -b - sq;
    t1 = -b + sq;
    if (t1 < 0.0) return false;

    // Inner sphere: a ray from below the deck enters at the inner surface, and
    // a ray from inside the deck must stop there on the way out.
    float  r_in   = FS_PLANET_R + FS_CLOUD_BASE;
    float  disc_in = b * b - (c - r_in * r_in);
    if (disc_in >= 0.0) {
        float sq_in = sqrt(disc_in);
        float in0   = -b - sq_in;
        float in1   = -b + sq_in;
        float h     = length(o) - FS_PLANET_R;
        if (h < FS_CLOUD_BASE) {
            // Below the deck: the inner sphere is a wall, not a way in.
            if (in1 < 0.0) return false;
            t0 = max(t0, in1);
        } else {
            // Inside or above the deck: stop at the near side of the inner sphere.
            t1 = min(t1, in0 > 0.0 ? in0 : t1);
        }
    }
    t0 = max(t0, 0.0);
    return t1 > t0;
}

// Height above sea level on the curved planet.
float fsPlanetHeight(float3 p) { return length(p - float3(0.0, -FS_PLANET_R, 0.0)) - FS_PLANET_R; }

// Remap with saturation, the primitive every cloud density term is built from.
//   v in [lo, hi] -> [0, 1]; outside -> clamped.
float fsRemap(float v, float lo, float hi) { return saturate((v - lo) / max(hi - lo, 1e-6)); }

// ---- periodic 3D noise ----------------------------------------------------
//
// The density volumes tile horizontally, so the noise that fills them has to
// tile too: a volume whose lattice does not wrap shows a vertical seam every
// nine kilometres, and from inside the deck that seam is a wall. mu_lib's
// mu_value_noise3 is built on the same PCG3D cell hash but is not periodic, so
// the wrap is added here by reducing the cell coordinates modulo the lattice
// period before hashing. Everything below the hash is mu_lib's own value noise.
//
// Y is clamped rather than wrapped: the deck has a floor and a ceiling and
// wrapping them together would put cumulonimbus tops under stratus bases.

float fsPeriodicValue3(float3 p, int3 period)
{
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * (3.0 - 2.0 * f);
    int3   c = int3(i);

    int x0 = fsModI(c.x, period.x),     x1 = fsModI(c.x + 1, period.x);
    int z0 = fsModI(c.z, period.z),     z1 = fsModI(c.z + 1, period.z);
    int y0 = clamp(c.y, 0, period.y - 1), y1 = clamp(c.y + 1, 0, period.y - 1);

    float n000 = mu_cell_hash3(int3(x0, y0, z0));
    float n100 = mu_cell_hash3(int3(x1, y0, z0));
    float n010 = mu_cell_hash3(int3(x0, y1, z0));
    float n110 = mu_cell_hash3(int3(x1, y1, z0));
    float n001 = mu_cell_hash3(int3(x0, y0, z1));
    float n101 = mu_cell_hash3(int3(x1, y0, z1));
    float n011 = mu_cell_hash3(int3(x0, y1, z1));
    float n111 = mu_cell_hash3(int3(x1, y1, z1));

    float nx00 = lerp(n000, n100, u.x);
    float nx10 = lerp(n010, n110, u.x);
    float nx01 = lerp(n001, n101, u.x);
    float nx11 = lerp(n011, n111, u.x);
    return lerp(lerp(nx00, nx10, u.y), lerp(nx01, nx11, u.y), u.z) * 2.0 - 1.0;
}

// The same wrap in 2D, for the weather map: it tiles the world on
// FS_WEATHER_TILE and is scrolled by the wind, so a non-periodic lattice would
// draw a straight seam in the cloud coverage every 49 km. Four hashes, not the
// eight a 3D lattice needs.
float fsPeriodicValue2(float2 p, int2 period)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    int2   c = int2(i);

    int x0 = fsModI(c.x, period.x),     x1 = fsModI(c.x + 1, period.x);
    int y0 = fsModI(c.y, period.y),     y1 = fsModI(c.y + 1, period.y);

    float a = mu_cell_hash(int2(x0, y0));
    float b = mu_cell_hash(int2(x1, y0));
    float d = mu_cell_hash(int2(x0, y1));
    float e = mu_cell_hash(int2(x1, y1));
    return lerp(lerp(a, b, u.x), lerp(d, e, u.x), u.y) * 2.0 - 1.0;
}

float fsPeriodicFbm2(float2 p, int2 period, int octaves, float gain)
{
    float a = 0.5, sum = 0.0, norm = 0.0;
    int2  per = period;
    for (int i = 0; i < octaves; ++i) {
        sum  += a * fsPeriodicValue2(p, per);
        norm += a;
        p     = p * 2.03 + float2(11.7, 7.3);
        per   = per * 2;
        a    *= gain;
    }
    return sum / norm;
}

// Fractal sum over the periodic lattice. Each octave doubles the period along
// with the frequency, which is what keeps the sum periodic: an octave whose
// period is not an integer multiple of the tile would break the wrap.
float fsPeriodicFbm3(float3 p, int3 period, int octaves, float gain)
{
    float a = 0.5, sum = 0.0, norm = 0.0;
    int3  per = period;
    for (int i = 0; i < octaves; ++i) {
        sum  += a * fsPeriodicValue3(p, per);
        norm += a;
        p     = p * 2.03 + float3(11.7, 3.1, 7.3);
        per   = per * 2;
        a    *= gain;
    }
    return sum / norm;
}

// The cloud's shape, evaluated in lattice units. A domain warp first: three
// extra evaluations turn a grid-aligned fbm into the cotton-wool structure
// cumulus actually has, and it is far cheaper than the worley lattice that
// would otherwise be needed to get the same roundness.
float fsCloudShape(float3 p, int3 period, int octaves)
{
    float3 w = float3(fsPeriodicValue3(p + float3(11.3, 5.1,  7.7), period),
                      fsPeriodicValue3(p + float3(23.7, 3.9, 17.1), period),
                      fsPeriodicValue3(p + float3(31.1, 19.3, 5.9), period));
    float3 q = p + w * float3(1.7, 0.9, 1.7);
    return saturate(fsPeriodicFbm3(q, period, octaves, 0.55) * 0.5 + 0.5);
}

// ---- cloud shape terms ----------------------------------------------------

// The vertical profile. Coverage and cloud type come from the weather map and
// decide how the deck is weighted between its floor and its ceiling: stratus
// hugs the base, cumulus fills the middle, cumulonimbus runs the whole height.
// The result is a 0..1 multiplier on density, so a cloud's *shape* comes from
// the noise and its *altitude profile* comes from here.
float fsHeightGradient(float h, float type)
{
    float t        = fsRemap(h, FS_CLOUD_BASE, FS_CLOUD_TOP);
    float bottom   = lerp(0.09, 0.42, type);      // stratus is thin at the base
    float top      = lerp(0.30, 0.12, type);      // anvil tops thin out slowly
    float g        = saturate(t / bottom) * saturate((1.0 - t) / top);
    // Buoyancy: density falls off with height so the deck has a soft top
    // instead of a hard ceiling at FS_CLOUD_TOP.
    return g * (1.0 - 0.35 * t);
}

// ---- cloud variants -------------------------------------------------------
//
// One push-constant mode selects how the march gets its density and how hard it
// works for it. The modes are cumulative: each one adds exactly one
// optimisation to the previous, so timing them in order prices each
// optimisation on its own instead of in a bundle. See README.md for the
// measurements.
enum
{
    FS_CLOUDS_OFF      = 0u,   // no cloud pass at all
    FS_CLOUDS_ANALYTIC = 1u,   // noise evaluated live, uniform steps, no jitter
    FS_CLOUDS_VOLUME   = 2u,   // + density baked into 3D textures, trilinear fetch
    FS_CLOUDS_ADAPTIVE = 3u,   // + coarse-to-fine stepping and a transmittance exit
    FS_CLOUDS_TEMPORAL = 4u,   // + jittered entry, resolved against the last frame
};

#endif // FS_COMMON_SLANG
