# clustered — Granite-style clustered forward shading

The scene and the 1 024 moving lights of [`deferred`](../deferred/README.md),
30 % of them spot lights, shaded forward: no G-buffer and no per-cluster
light lists.

```
PrePass (depth only)
  -> BinLights (compute: one bit per light per 32 x 32 pixel tile)
  -> Forward (RGBA16F: lights of the pixel's tile AND depth bin)
  -> [FogInject -> FogIntegrate (compute, only with --fog)] -> Fog
  -> Present -> UI
```

The data structure is the one Hans-Kristian Arntzen describes for Granite's
"bindless clusterer" ([Clustered shading evolution in Granite][granite],
2020), which follows Michal Drobot's z-binning for Call of Duty: screen
position and depth are culled separately.

| | `deferred` (lists per 3D cluster) | this demo |
|---|---|---|
| per frame, CPU | — | sort lights by view depth; fill the z bins |
| screen | 16 × 9 tiles … | 32-pixel tiles, `u32[lights / 32]` bit mask each |
| depth | … × 24 slices | 1 024 linear bins, `[first, last]` light index each |
| size at 1280 × 720, 1 024 lights | 7.1 MB (511 indices per cluster, fixed) | 126 KB |
| can overflow | yes, at 511 lights in a cluster | no: one bit per light |

A pixel loads its bin's range, then only the mask words that range touches,
masks them to the range (`maskRange`, Granite's `cluster_mask_range`) and
walks the set bits with a bit scan. Because the lights are sorted by depth, a
bin's range is short even though it has no idea about screen position.

```sh
./build/cm/clustered
./build/cm/clustered --headless out.ppm --time 3
./build/cm/clustered --headless out.ppm --time 3 --view 1   # lights tested per pixel
./build/cm/clustered --headless --check --time 3
```

`--view`: 0 lit, 1 lights tested per pixel (blue 0 … red 48), 2 bits set in
the tile's mask (… 96), 3 lights in the depth bin's range (… 512), 4 tested
but out of reach. `--brute` shades every light per pixel. `--sdf-shadows` adds a shadow ray
per light (`--sdf-bias X`: its start offset in voxels). `--spots X` sets
the share of spot lights.

## What is like Granite and what is not

Like it: lights sorted by depth; z bins holding a min/max light index; one
bit per light per tile; a compute shader that produces one 32-bit word per
thread from 32 lights; a type mask (one bit per light: spot or point) read
per word; the range-masked bit-scan loop; a depth pre-pass in front of the
forward pass.

Not like it:

- **No subgroup operations.** Granite makes the mask uniform over the
  subgroup (`subgroupOr`) so the light data loads become scalar. That is a
  per-GPU optimization; lavapipe would not show it and it is not written.
- **The tile test is four planes against a bounding sphere.** Granite
  rasterizes each light's shape conservatively in the compute shader. Here a
  spot light is binned by the smallest sphere around its cone, a point light
  by its own sphere. Both are conservative; the measured cost is below.
- **Z bins are filled on the CPU**, linear in depth, 1 024 over 0 … 100 m.
- **No shadow maps.** Granite's clusterer also carries shadow atlas
  transforms per light, and decals. Neither is here. Shadows come from a
  distance volume instead, when asked for (below).
- No transparent surfaces, though a forward renderer could draw them with
  the same structure.

## Acceptance (`--check`)

Two cameras, 640 × 360, 1 024 lights (overview / low camera).

| Line | Rule | Measured |
|---|---|---|
| clustered vs every light (HDR, every pixel, max difference) | == 0 | 0 / 0 |
| lights tested per pixel, mean | < 5 % of all lights | 21.1 / 18.0 |
| lights that reach the pixel, mean | reported | 4.3 / 5.6 |
| lights whose centre projects into a tile but are missing from its mask (CPU test, masks read back) | == 0 | 0 of 609 / 0 of 427 |
| bits set in the masks | 0 < fraction < 0.2 | 0.035 / 0.045 |
| widest z-bin range | reported | 242 / 234 lights |

The first line is exact because both loops visit lights in ascending index
order and a light out of range or outside its cone adds nothing at all.

**One light in five that is tested reaches the pixel** (0.21 / 0.31). The
rest is what the two cheap tests let through: the z bin knows nothing about
screen position, the tile mask nothing about depth, and both use bounding
spheres. With 4 096 lights at 320 × 180: 117 tested per pixel, 0.15 reach.
`deferred`'s 3D clusters were not instrumented the same way, so there is no
measured comparison of the two structures' tightness.

## Shadows for every light (`--sdf-shadows`)

No shadow maps. The scene is boxes, spheres and rings on a floor, so its
distance function can be written down. A compute pass samples it once into a
288 × 32 × 288 RGBA16F 3D texture (36 × 4 × 36 m, 12.5 cm voxels, 21 MB), and
the forward pass walks from each pixel towards each light that reaches it
through that texture, taking steps as long as the distance it reads:

    shadow = min over the ray of  k · distance(t) / t,   k = distance to the lamp / lamp radius

at most 40 steps of at least half a voxel, starting half a voxel off the
surface. The moon gets the same with a fixed k (a disc 2.4° in radius). The
formula is Quilez's (*Soft shadows in raymarched SDFs*); the idea of doing it
for rasterized geometry through a volume is that of Claybook and of Unreal's
distance-field shadows. Cited from memory.

**What it is compared with.** I had announced a shadow-map reference. The
reference here is the distance function itself, walked in 1 cm steps along
the same rays (view 7): it has no resolution or bias of its own, and the
check first confirms that the function is zero to 8 mm on every rasterized
surface in view, so it describes the meshes that are drawn.

`--check`, 640 × 360, overview / low camera; "3 lamps" are the three lamps
nearest the camera's target that hang at least 30 cm clear of everything:

| Line | Rule | Moon | 3 lamps |
|---|---|---|---|
| pixels facing the light and in reach | > 2 000 | 192 124 / 103 623 | 14 238 / 62 310 |
| of those, in shadow in the reference | > 2 % | 5.1 % / 6.9 % | 11.4 % / 38.9 % |
| in a penumbra in the reference | reported | 0.9 % / 1.2 % | 0.6 % / 1.7 % |
| blocked by more than a voxel of solid, but lit | == 0 | 0 / 0 | 0 / 0 |
| fully lit in the reference, but dark | < 0.1 % | 0.050 % / 0.076 % | 0.049 % / 0.013 % |
| blocked / not blocked differs | reported | 0.23 % / 0.76 % | 0.55 % / 1.0 % |
| smallest distance met on the ray, mean difference | < 0.1 voxel | 0.011 / 0.015 | 0.019 / 0.059 |
| the same, largest | reported | 0.65 / 0.74 voxel | 0.94 / 1.31 voxel |
| shadow term, mean difference | < 0.02 | 0.002 / 0.007 | 0.005 / 0.008 |

| Line | Rule | Measured |
|---|---|---|
| with shadows: clustered vs every light (HDR, every pixel) | == 0 | 0 / 0 |
| values brighter with shadows than without | == 0 | 0 / 0 |
| pixels darker with shadows than without | > 5 % | 21 % / 43 % |
| distance function at rasterized surfaces, largest | < 0.01 m | 0.0077 m / 0.0077 m |

**No light leaks; a few pixels in two thousand are dark that should be lit.**
Where they sit was not examined. Starting further off the surface removes
them and moves every contact shadow away from its object by as much:

| start offset, voxels | 0.5 (default) | 0.75 | 1.0 | 1.5 |
|---|---|---|---|---|
| falsely dark pixels, moon, overview / low | 97 / 79 | 45 / 31 | 19 / 12 | 0 / 0 |
| falsely dark pixels, 3 lamps, overview / low | 7 / 8 | 3 / 3 | 3 / 0 | 0 / 0 |

Not covered:

- The reference rays start at the same offset, so what the offset itself
  costs (a shadow that begins 6 cm from its object) is not measured.
- The reference uses the same penumbra formula on the exact distances. That
  the formula matches a real disc-shaped lamp is not tested.
- The check looks at the moon and three lamps, not at all 1 024, and ignores
  spot cones.
- The scene does not move. A moving object means baking again (about 0.6 s
  here for the whole volume) or editing a box of it as `sdfworld` does;
  neither is built.
- A mesh without a distance function would need one computed. Not built.
- Outside the volume's 36 × 36 m nothing is shadowed.

## Fog lit by the same lights (`--fog X`)

```
FogInject     every froxel of a 160 × 90 × 64 volume aligned with the view: extinction, and the
              light scattered towards the eye — the lamps of the froxel's tile and depth bin,
              the moon with a shadow ray through the distance volume, a little ambient
FogIntegrate  every column front to back: light gathered and background left, per slice
Fog           one filtered fetch at the pixel's depth: picture × left + gathered
```

The light lists are the ones the surfaces use: a point in mid-air has a
screen tile and a depth bin like a surface point does. Slices are spaced
quadratically in view depth up to 60 m (1.5 cm at the eye, 1.9 m at the far
end). Density falls off with height (`--fog-height`). The technique is
Wronski's (SIGGRAPH 2014) with Hillaire's slice integral (SIGGRAPH 2015);
both cited from memory.

`--fog-reference` draws the same medium by marching every pixel in 128
steps (`--fog-steps`) instead. That is the reference in `--check`:

| Line | Rule | overview | low camera |
|---|---|---|---|
| fog off: values the fog pass changes | == 0 | 0 | |
| uniform fog, even light, nothing else — froxels: transmittance vs exp(−density · path), worst pixel | < 0.002 | 0.0010 | 0.0011 |
| froxels: light gathered vs the closed form, worst pixel | < 0.002 | 0.0007 | 0.0009 |
| marched: the same two | < 0.002 | 0.0007 / 0.0002 | 0.0007 / 0.0002 |
| paths in that test | longest > 50 m | 11–83 m | 2–98 m |
| marched: lamps from tile and bin vs every lamp, largest difference | == 0 | 0 | 0 |
| moon alone: froxels vs marched, mean difference as a share of the fog's light | < 3 % | 1.6 % | 0.9 % |
| moon and 1 024 lamps: the same | < 25 % (guard) | **14.7 %** | **4.9 %** |
| largest difference on a pixel (fog light 0.12 / 0.22 on average) | reported | 0.79 | 0.61 |
| transmittance, mean difference | < 0.002 | 0.0004 | 0.0003 |

What the numbers say:

- The integration is right: in a fog whose answer can be written down, both
  the froxels and the march are within 0.001 of it over paths from 2 to
  98 m, which is the resolution of the 16-bit float targets.
- The light lists are right in mid-air: marching with the lamps of each
  point's tile and bin gives exactly the picture marching with all 1 024
  gives.
- Under the moon the froxels are within 1–2 % of the march, shafts included.
- Under a thousand small lamps they are not: 15 % of the fog's light is
  misplaced in the overview, and single pixels are off by more than the fog
  is bright. Marching with 512 steps instead of 128 leaves this at 14.7 %
  (3.9 % for the low camera), so it is the froxels and not the reference. A
  lamp here reaches a few metres and is brightest within 25 cm; far froxels
  are 1–2 m deep and lit at one point each. The usual remedy — jittering
  the sample point every frame and blending with the reprojected previous
  volume — is not built.

Not done: the lamps cast no shadows in the fog (only the moon does); no
temporal filtering; the fog ends at 60 m; the volume does not follow the
window's aspect; surfaces are not lit by the fog, nor dimmed on the way from
the lamp to the surface. Without `--fog` the Fog pass still runs and copies
the picture.

## Time

lavapipe, 1280 × 720, wall time per frame (5-frame run minus 1-frame run,
divided by 4). A CPU rasterizer; ratios on a GPU will differ.

| | 1 024 lights | 4 096 lights |
|---|---|---|
| clustered | 0.16 s | 0.55 s |
| clustered, `--sdf-shadows` (measured later, the same day's "clustered" was 0.18 s) | 0.47 s | not measured |
| moon only (`--lights 0`), without / with shadows | 0.05 s / 0.11 s | |
| `--brute` | 2.3 s | not measured |
| `deferred`, for scale (no spot lights, different shader) | 0.15 s | 0.41 s |

Fog, measured later (13-frame run minus 3-frame run): 1 024 lights 0.16 s
→ 0.23 s with `--fog 0.02`; 48 lights 0.05 s → 0.11 s; 48 lights with
`--fog-reference` 2.0 s. The froxel volume has as many cells as the picture
has pixels at this size, and does not grow with it.

## Not verified

lavapipe only. The windowed mode ran for a few seconds under Xvfb with the
validation layer (no messages); the panel's controls were not operated.
`--sdf-shadows` ran the same way, headless and for a few seconds in a
window; its checkbox in the panel was never clicked. `--fog` likewise: a
few seconds in a window, no validation messages, controls not touched.

[granite]: https://themaister.net/blog/2020/01/10/clustered-shading-evolution-in-granite/
