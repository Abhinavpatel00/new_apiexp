// volume.slang — a signed distance volume, and tracing it.
//
// Included after the bindless tables and mu_volume.slang.
//
// After Sebastian Aaltonen's Claybook (GDC 2018, "GPU-based clay simulation
// and ray-tracing tech in Claybook"):
//   - the world is a volume of distances, not triangles;
//   - a voxel stores the distance only within +-BAND voxels of the surface
//     ("limited range"), so an edit touches a brush-sized box and nothing
//     else;
//   - a coarser level says how far empty space reaches where the fine level
//     can only say "at least BAND";
//   - a low-resolution cone-trace pre-pass gives every 8 x 8 pixel block a
//     safe depth to start from.
//
// The volume is a 3D texture (mu_volume.slang): r = distance in voxels,
// gba = albedo; one trilinear fetch per sample.
//
// The coarse level has one texel per CELL^3 voxels: the Chebyshev distance,
// in cells, to the nearest cell that may contain surface or solid
// (coarse.slang). 0 = this cell may; D >= 2 means every point of this cell
// is at least (D - 1) * CELL voxels from anything.

static const float CELL = 4.0;

struct Volume
{
    uint   fine, coarse;      // bindless indices of the two volumes
    uint   linear, pad;       // bindless sampler index (clamp to edge)
    float3 dims;              // voxels
    float  band;              // voxels
};

float4 volumeSample(Volume v, float3 p)
{
    // Voxel centres are at integer + 0.5, which is where texel centres are.
    return muSampledVolumes[NonUniformResourceIndex(v.fine)].SampleLevel(
        muSamplers[NonUniformResourceIndex(v.linear)], p / v.dims, 0.0);
}

float volumeDistance(Volume v, float3 p) { return volumeSample(v, p).r; }

// Cells of guaranteed emptiness around p's cell (see the header).
float volumeCoarse(Volume v, float3 p)
{
    float3 c = clamp(floor(p / CELL), float3(0.0), v.dims / CELL - 1.0);
    return muSampledVolumes[NonUniformResourceIndex(v.coarse)].Load(int4(int3(c), 0)).r;
}

float3 volumeNormal(Volume v, float3 p)
{
    const float h = 0.75;
    return normalize(float3(
        volumeDistance(v, p + float3(h, 0, 0)) - volumeDistance(v, p - float3(h, 0, 0)),
        volumeDistance(v, p + float3(0, h, 0)) - volumeDistance(v, p - float3(0, h, 0)),
        volumeDistance(v, p + float3(0, 0, h)) - volumeDistance(v, p - float3(0, 0, h))) + 1e-6);
}

// Ray against the volume's box. Returns (enter, exit); enter > exit = miss.
// `face` is the outward normal of the face the ray enters through.
float2 volumeBox(Volume v, float3 ro, float3 rd, out float3 face)
{
    float3 inv = 1.0 / select(abs(rd) < 1e-6, float3(1e-6), rd);
    float3 a = (float3(0.0) - ro) * inv, b = (v.dims - ro) * inv;
    float3 lo = min(a, b), hi = max(a, b);
    float  t0 = max(max(lo.x, lo.y), lo.z), t1 = min(min(hi.x, hi.y), hi.z);
    face = t0 == lo.x ? float3(-sign(rd.x), 0, 0) : t0 == lo.y ? float3(0, -sign(rd.y), 0) : float3(0, 0, -sign(rd.z));
    return float2(max(t0, 0.0), t1);
}

struct VolumeHit { float t; float steps; bool hit; };

// Sphere tracing (Hart 1996), ro and t in voxels. `cone` is the tangent of
// the ray's half-angle: a hit is "closer than the ray is wide" (Claybook:
// D < pixelConeWidth * t). With `skip`, the coarse level jumps empty space.
//
// `relax` > 1 is over-relaxation (Keinert et al. 2014): step relax * d
// instead of d. That can step over a surface, so each step is checked: the
// empty sphere at the new point must reach back to the empty sphere at the
// old one (d + previous d >= step). If it does not, the piece in between was
// never shown to be empty: go back and continue with plain steps. A field
// that underestimates distances, as this one does, wastes less this way.
VolumeHit volumeTrace(Volume v, float3 ro, float3 rd, float t0, float t1, float cone, bool skip, uint budget,
                      float relax)
{
    VolumeHit h;
    h.t = t0; h.steps = 0.0; h.hit = false;
    float previous = 0.0, step = 0.0;
    for (uint i = 0u; i < budget; ++i)
    {
        if (h.t >= t1) break;
        float3 p = ro + rd * h.t;
        h.steps += 1.0;
        if (skip)
        {
            float cells = volumeCoarse(v, p);
            if (cells >= 2.0) { h.t += (cells - 1.0) * CELL; previous = 0.0; step = 0.0; continue; }
        }
        float d = volumeDistance(v, p);
        if (relax > 1.0 && d + previous < step)
        {
            // Also taken when d < 0: an over-long step went through the surface.
            step -= relax * step;       // back into the last sphere known to be empty
            relax = 1.0;                // and plain steps from here on (as in the paper)
        }
        else
        {
            if (d < max(cone * h.t, 0.02)) { h.hit = true; break; }
            step = d * relax;
        }
        previous = max(d, 0.0);
        h.t += step;
    }
    return h;
}

// The pre-pass: a cone instead of a ray. Advances while the whole cone is
// known to be empty and returns the depth where that stops. Every ray inside
// the cone can start there.
//   The sphere of radius d around the axis point at t is empty. A point of
//   the cone at t + s is at most sqrt(s^2 + r(t+s)^2) <= s + r(t + d) from
//   it, so s = d - r(t + d) keeps the whole cross-section inside.
// The cone starts at the eye, not where its axis enters the box: at the
// box's silhouette other rays of the block enter long before the axis does.
// Outside the box, with c the nearest point of the box, anything solid is at
// least sqrt(|p - c|^2 + d(c)^2) away (the box is convex).
float volumeConeStart(Volume v, float3 ro, float3 rd, float t1, float coneTan, bool skip, uint budget,
                      out float steps)
{
    float t = 0.0;
    steps = 0.0;
    for (uint i = 0u; i < budget; ++i)
    {
        if (t >= t1) return t1;
        float3 p   = ro + rd * t;
        float3 c   = clamp(p, float3(0.0), v.dims);
        float  outside = length(p - c);
        steps += 1.0;
        if (skip && outside == 0.0)
        {
            float reach = (volumeCoarse(v, p) - 1.0) * CELL;
            float s     = reach - coneTan * (t + reach);
            if (s > 0.25) { t += s; continue; }
        }
        float d = volumeDistance(v, c);
        if (outside > 0.0) d = sqrt(outside * outside + max(d, 0.0) * max(d, 0.0));
        float s = d - coneTan * (t + d);
        if (s <= 0.25) break;
        t += s;
    }
    return t;
}

// Soft shadow from one ray towards the light: the narrowest "distance /
// travelled" ratio seen on the way is how much of the light's disc is
// visible (Quilez; Claybook widens the cone the same way). Taking the ratio
// at the sample points only makes bands, because the closest approach falls
// between samples. Aaltonen's fix (GDC 2018): two successive distances are
// two spheres; where they intersect is an estimate of the closest point.
//   y = d^2 / (2 * previous d)      how far back along the ray it was
//   sqrt(d^2 - y^2)                 how close the ray came
// A sample clamped at +BAND only says "at least BAND" and must not narrow
// the cone.
float volumeShadow(Volume v, float3 p, float3 toLight, float sharpness, bool improved)
{
    float3 face;
    float2 box = volumeBox(v, p, toLight, face);
    float  res = 1.0, t = 1.0, previous = 1e9;
    for (uint i = 0u; i < 48u && t < box.y; ++i)
    {
        float3 q     = p + toLight * t;
        float  cells = volumeCoarse(v, q);
        if (cells >= 2.0) { t += (cells - 1.0) * CELL; previous = 1e9; continue; }
        float d = volumeDistance(v, q);
        if (d < 0.02) return 0.0;
        if (d < v.band * 0.98)
        {
            if (improved)
            {
                float y = d * d / (2.0 * previous);
                res = min(res, sharpness * sqrt(max(d * d - y * y, 0.0)) / max(t - y, 1e-3));
            }
            else res = min(res, sharpness * d / t);
            previous = d;
        }
        else previous = 1e9;
        t += max(d, 0.35);
    }
    // Smooth the kink where the penumbra meets full light.
    res = saturate(res);
    return res * res * (3.0 - 2.0 * res);
}

// Ambient occlusion from the field itself: how much less room there is
// along the normal than there would be over a flat surface.
float volumeAO(Volume v, float3 p, float3 n)
{
    float occ = 0.0, w = 1.0;
    for (uint i = 1u; i <= 5u; ++i)
    {
        float h = 0.7 * (float)i;
        occ += w * (h - min(volumeDistance(v, p + n * h), h));
        w *= 0.7;
    }
    return saturate(1.0 - 0.55 * occ);
}
