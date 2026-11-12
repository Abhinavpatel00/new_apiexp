# flightsim

A flyable world: six-degrees-of-freedom flight model, procedural terrain on a
clipmap, an advecting weather system that bakes itself into a cloud map, and a
volumetric cloud march whose four algorithm variants are left in the shader so
their cost can be measured instead of argued about.

```
build/cm/flightsim                          # windowed, Nuklear panel with F1
build/cm/flightsim --headless out.ppm       # 320x180 PPM, two warm-up frames
build/cm/flightsim --check                  # 24 numeric assertions, exit 0 = pass
scripts/check_demos.sh flightsim            # reference image + the check suite
```

Controls: arrows or WASD pitch and roll, Q/E rudder, Shift/Ctrl throttle,
B speedbrake, F flight model (6-DOF / arcade), C cloud mode 0..4, V view
(cockpit / chase / orbit), H HUD, R reset.

## Frame graph

Ten passes, in order:

| pass        | kind    | writes                                            |
|-------------|---------|---------------------------------------------------|
| BakeVolumes | compute | the three cloud shape volumes, once at startup    |
| BakeWeather | compute | the 512² weather map from the advected cell list  |
| Sky         | graphics| scene colour (analytic sky, no depth)             |
| Terrain     | graphics| scene colour + linear view distance, clears both depth (reversed Z, 0) and the distance target |
| Water       | graphics| one 260 km quad at sea level, analytic waves      |
| Aircraft    | graphics| player and traffic, instanced from an 80 B record |
| Clouds      | compute | rgba16f accumulation + first-contact distance     |
| Resolve     | graphics| tone-maps the scene, composites the clouds, reprojects history |
| Hud         | graphics| segment/font symbology, alpha-blended             |
| Present     | graphics| swapchain / sRGB output                           |

Depth is reversed throughout: `main.c` overwrites `proj[10]`/`proj[14]` of
`mu_mat4_perspective_zo` exactly as `demos/clustered` does, so near maps to 1
and far to 0, every pass clears to 0 and compares `CMP_GREATER`. With the stock
zero-to-one matrix the same clear and compare make the *farthest* surface win,
which lets the 260 km water quad paint over the entire world.

The cloud march ping-pongs its accumulation, so the frame graph exists twice,
once per history parity (`App.programs[2]`); `app_shutdown` frees only the
parity `demo.c` will not destroy.

## Axes and the one quaternion that is not identity

World: +X east, +Y up, +Z south; heading 0 is north, clockwise from above.
Body: +X forward, +Y right, +Z down. Those two frames are not the same
orientation, so **the identity quaternion is not level flight** — it maps the
right wing into the sky. `flight.c` keeps `k_att_level = {0.5, 0.5, -0.5, 0.5}`,
which maps (forward, right, down) onto (north, east, down), and every attitude
(player, traffic, reset) is a yaw composed onto it. An aircraft "trimmed" at
the identity lifts sideways and tumbles, which is exactly what it did before
this constant existed.

## Terrain

No vertex buffer and no heightfield texture. The vertex shader builds each
clipmap ring from `SV_VertexID` and evaluates `fsTerrainHeight()` — the same
function the CPU answers "what is under this aircraft" from, mirrored in
`mu_lib/mu/mu_shader_noise.h`. Ring *i* has spacing `ring0 * 2^i` and its
centre snapped to that spacing, so the grid never swims; the middle half of
every ring but the first is collapsed in the shader because the next finer
ring already covers it. The base spacing jumps by octaves of the height above
ground, which hands the fine grid to a ring that can use it at cruise
altitude. `--check` marches the CPU heightfield down the centre pixel's ray
and compares the hit distance against the GPU's linear-depth target; the
tolerance is `6 * ring_spacing + 40 m`, the chord error of the coarsest ring
that can cover the ray, derived rather than tuned.

## Clouds: four variants, one push constant

`shaders/clouds.slang` carries every optimisation behind a uniform branch on
`quality.x`, so each one is a mode you can time:

1. **analytic** — five octaves of periodic noise per sample, uniform stepping,
   no jitter, no early exit, cone taps from the same analytic field.
2. **volume** — density baked into 3D textures, fetched with a manual
   trilinear of eight `Load`s (see Rejected approaches); the light cone reads
   the low-frequency volume.
3. **adaptive** — coarse-to-fine stepping and a transmittance early exit.
4. **temporal** — jittered entry point, accumulated with history in Resolve.

Measured on llvmpipe (LLVM 19.1.7), 320×180, default march budget and refine,
Clouds pass GPU time from `--perf`, median of three 30-frame runs:

| mode | pass cost | versus mode 1 |
|------|-----------|---------------|
| 0 off      |  0.77 ms | —                          |
| 1 analytic | 96.72 ms | the baseline               |
| 2 volume   | 43.47 ms | 2.2× cheaper               |
| 3 adaptive | 62.70 ms | 1.5× cheaper               |
| 4 temporal | 67.62 ms | 1.4× cheaper               |

**Mode 4 ships as the default**: the temporal jitter is what lets the history
hide the stepping, so it buys image quality at the same cost as mode 3, and
every mode above 1 beats evaluating the noise live by a wide margin.

Two honest caveats about the shape of that table. First, the volume fetch is
eight `Load`s and a manual trilinear, not one hardware `SampleLevel`: on
llvmpipe a filtered (or even unfiltered-but-repeating) 3D fetch from a compute
stage returns zero, silently, so the march drew an empty sky until the fetch
was done by hand. On a driver with working compute-stage 3D filtering, mode 2
onwards should be considerably cheaper than these numbers. Second, adaptive
stepping is *slower* than plain volume stepping at the default scattered
coverage: the early exit only pays once the march would have walked a long way
through opaque cloud, while the coarse-to-fine refine pays on every contact.
In an overcast sky the ordering flips; at the default sky it does not, and the
table says so rather than pretending otherwise.

(The same run spends ~87 ms in Terrain: on a software rasteriser the clipmap's
~200 k noise-evaluating vertices dominate everything, clouds included.)

### Rejected approaches

- **`SampleLevel` on a 3D image from a compute stage.** On llvmpipe it returns
  zero for every texel, with no error and no warning, which presented as an
  empty sky in every mode above 1 while readbacks of the volume showed perfect
  content. A repeating sampler on the 3D image reads zero too. The march now
  does eight `Load`s and a trilinear by hand, wrapping u and w with a mask
  because the baked field is periodic across the volume; slow, obvious, and
  correct on every driver.
- **Erosion as a second coverage gate.** Remapping the density against a
  threshold built from the erosion mask deleted every density below roughly a
  third, which is most of a cloud, and left the sky with wisps that looked
  like a working cloud system at a glance. The mask now scales the density
  instead of gating it.
- **Baking the shape volume at its noise period.** 64 lattice cells across 128
  texels is two texels per cell, above Nyquist: the bake came out as
  high-frequency noise whose trilinear mean has none of the variance a cloud
  needs, so the volume modes drew fog where the analytic mode drew clouds.
  Sixteen cells over 128 texels puts the last of four octaves at one texel per
  cell and keeps the variance.
- **Baking density from the CPU into a texture.** The CPU and GPU noise stacks
  do not agree bit-for-bit — different hash lowerings, and Slang's `lerp` may
  lower to `a+(b-a)t` — so a CPU bake would put a second, slightly different
  world into the pipeline. The bake that exists (`BakeVolumes`) runs on the GPU
  from the same Slang source the march samples, which keeps one source of
  truth and keeps `--check` honest.
- **Marching by accumulating a `float3` position.** Over ~100 steps the
  accumulated error drifts the ray off the analytic density; the march keeps a
  scalar `t` and rebuilds `pos = ro + rd*t`.
- **A shared `AttachDesc` array written once per pass.** `frame_program_build`
  deep-copies the pass table *at the end*, so three passes sharing one array
  all see its last contents — Terrain would have loaded the linear-depth
  target instead of clearing it. Each pass gets its own locals.
- **`MU_ASSERT` as an array bound.** It is `assert()` and compiles out under
  `NDEBUG`; the HUD text list uses an explicit `if (count == MAX) return;`.
- **Stack-buffer target names.** `TargetDesc.name` stores the pointer, so the
  ping-pong targets are named from `static const char *const` arrays.
- **Precomputing `model * view_proj` on the CPU** for the aircraft instances:
  the vertex shader needs `model` alone, and the push already carries the
  matrices.
- **A per-ring origin/spacing table in the push constant:** ring *i* is
  `ring0 * 2^i` from `instanceID`, one number instead of sixteen per ring.

## Checks

`--check` runs 24 assertions: airframe mesh sanity, terrain finiteness and
basin floor, trimmed 6-DOF holding altitude and staying flying, full back stick
passing the stall angle and then losing 100 m measured from the post-stall peak
(the zoom climb before the departure is physical and must not fail the test),
arcade reaching full-throttle speed, a pulsed forward-stick dive crashing at a
plausible speed (a *held* stick loops instead of diving), traffic holding
assigned altitude, the CPU/GPU centre-ray agreement above, frame content
(sky / green-dominant ground / not one colour), the base shape volume actually
holding baked density, the weather map responding to coverage by re-seeding
rather than re-baking, and clouds-on versus clouds-off
changing both the picture and its total light.
