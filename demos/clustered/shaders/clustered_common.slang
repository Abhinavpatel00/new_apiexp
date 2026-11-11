// clustered_common.slang — layouts shared by bin.slang and clustered.slang.
//
// The structure is the one Granite's "bindless clusterer" uses (Arntzen,
// "Clustered shading evolution in Granite", 2020), which follows Drobot's
// z-binning for Call of Duty: screen tiles and depth are culled separately,
// O(X * Y + Z) storage instead of O(X * Y * Z).
//
//   lights      48 B each, sorted by view depth on the CPU every frame
//   cull        16 B each: bounding sphere in view space (xyz, radius)
//   types       1 bit per light: 1 = spot
//   tile masks  per screen tile, 1 bit per light: "may touch this tile"
//   z bins      per depth bin: lowest and highest light index reaching it
//
// A pixel shades the lights whose bit is set in its tile AND whose index is
// inside its depth bin's range.
static const uint F_VIEW_PROJ = 0u, F_VIEW = 64u, F_EYE = 128u, F_FORWARD = 144u, F_PROJ = 160u, F_SIZE = 176u,
                  F_GRID = 192u, F_DATA = 208u, F_DATA2 = 224u, F_MOON = 240u;
// Shadows from a distance volume (sdf_shadow.slang): box origin + voxel size; volume, sampler, shapes,
// shape count; on, light looked at by the check views; lamp radius, moon k, bias in voxels.
static const uint F_SDF_BOX = 256u, F_SDF_DATA = 272u, F_SDF_CTL = 288u, F_SDF_PARAMS = 304u;
static const uint LIGHT_SIZE = 48u;
// Fog (fog.slang): camera right and up; density per metre, height scale (0 = uniform), far end of the
// froxels, ambient; phase g, albedo, near, far plane; froxel volumes (injected, integrated), mode
// (0 off, 1 froxels, 2 marched per pixel), what the fog pass writes; the forward pass's colour and
// depth, -, -; reference: steps, every n-th pixel, every light.
static const uint F_RIGHT = 320u, F_UP = 336u, F_FOG = 352u, F_FOG_PARAMS = 368u, F_FOG_DATA = 384u,
                  F_FOG_SCENE = 400u, F_FOG_REF = 416u;
static const uint3 FROXELS = uint3(160u, 90u, 64u);

static const float LAMP_RADIUS = 0.25;   // the falloff stops growing this close to a lamp

// Granite's cluster_mask_range: the bits of word `start / 32` whose light
// index lies in [range.x, range.y].
uint maskRange(uint mask, uint2 range, uint start)
{
    range.x = clamp(range.x, start, start + 32u);
    range.y = clamp(range.y + 1u, range.x, start + 32u);
    uint bits = range.y - range.x;
    uint keep = bits == 32u ? 0xffffffffu : ((1u << bits) - 1u) << (range.x - start);
    return mask & keep;
}

