# deferred — a thousand point lights, clustered

1 024 moving point lights by default (`--lights N`, up to 4 096) over a
floor with 64 objects. Geometry is drawn once into a G-buffer; a compute
pass sorts the lights into a 16 × 9 × 24 grid of view-frustum clusters; one
full-screen pass lights every pixel with the lights of its cluster only.

```
GBuffer (3 colour targets + depth)
  -> ClusterLights (compute, one thread per cluster)
  -> Lighting (full screen, RGBA16F)
  -> Present -> UI
```

A second frame program, `Forward -> Present -> UI`, shades the same geometry
straight from the vertex attributes with a loop over **every** light. It is
the reference `--check` compares against, and what `--forward` shows.

First demo with several colour attachments (`PipeKey.color_count`), an
integer render target (`FMT_R32_UINT`, read through the new shared module
`demos/common/shaders/mu_uint.slang`), position rebuilt from the depth
buffer, and light lists in the storage buffer that `--check` reads back with
`readback_request_buffer()`.

```sh
./build/cm/deferred
./build/cm/deferred --headless out.ppm --time 3
./build/cm/deferred --headless out.ppm --time 3 --view 5     # lights per cluster
./build/cm/deferred --headless out.ppm --time 3 --forward    # the reference; slow
./build/cm/deferred --headless --check --time 3              # about 12 s on lavapipe
```

`--view`: 0 lit, 1 albedo, 2 normal, 3 roughness (red) / metalness (green),
4 depth, 5 lights per cluster (blue 0 … red 96 and more; magenta = the list
overflowed), 6 clusters. `--brute` keeps the G-buffer but ignores the
clusters. `--standard-depth` switches reversed depth off (see below).

## The G-buffer

12 bytes per pixel, plus depth:

| Target | Format | Content |
|---|---|---|
| 0 | `RGBA8_SRGB` | albedo |
| 1 | `R32_UINT` | normal, octahedral, 16 + 16 bits (`mu_pack_normal_oct16`) |
| 2 | `RGBA8_UNORM` | roughness, metalness; two channels free |
| depth | `D32F`, reversed | position = the pixel's ray × this |

Each choice is there because a cheaper one was measured and lost. The
measure is the 8-bit image against the forward reference, 640 × 360, both
`--check` cameras (`pixels off by 1` / `off by > 1` of about 195 000 and
136 000 covered pixels):

| Variant | off by 1 code | off by more |
|---|---|---|
| as shipped | 53 % / 56 % | 0 / 0 |
| albedo `RGBA16F` instead of 8-bit sRGB | 3.4 % / 3.9 % | 0 / 0 |
| albedo `RGBA8_UNORM` (linear) | 24 % / 24 % | 0 / 0 |
| `--standard-depth` | 53 % / 56 % | 3 pixels (2 codes) / 0 |

- **Albedo.** Nearly all of the one-code differences are the albedo's 8
  bits: with a 16-bit albedo they drop to 3–4 %. In *this* scene a linear
  8-bit target does better than the sRGB one. That is expected here and not
  a reason to switch: every albedo in the scene is 0.25 or brighter, where
  linear steps are the finer ones. sRGB steps are the finer ones for dark
  colours; this scene has none, so that half of the argument is not measured
  here.
- **Normal.** A thinner layout — normal in 12 + 12 bits sharing a word with
  roughness, metalness in the albedo's alpha, 8 bytes — was built first. Its
  worst normal error is 0.00098 (length of the difference of the two unit
  vectors) against 0.000064 for 16 + 16 bits; `--check` still reports both.
  With it, specular highlights on the metals were up to 3 code values off
  the reference (29 pixels in an earlier, brighter version of this light
  set; the layout is no longer in the code, so that figure cannot be
  reproduced from this commit).
- **mu_lib's packed word is not used.** `mu_pack_nrm_rough_metal`
  (`external/mu_lib/mu/shaders/quantize.slang`) packs the normal into all 32
  bits and then overwrites the upper 16 with the material bytes, so the
  normal's second coordinate is gone. `--check` reports the round trip:
  error 2, the largest two unit vectors can have. mu_lib is unchanged.
- **Roughness and metalness** are authored in steps of 1/255
  (`q8()` in `main.c`), so the reference shades the same material the
  G-buffer can store. Without that, the comparison would measure the
  material's quantization instead of the renderer.

## Depth

The lighting pass has no position; it un-projects the pixel through
`inv_view_proj` (inverted in double precision on the CPU). So the image is
exactly as precise as the depth buffer.

| | max position error | mean |
|---|---|---|
| depth 0 near … 1 far (`--standard-depth`) | 3.9 mm | 0.17 mm |
| reversed, 1 near … 0 far (default) | 0.09 mm | 0.004 mm |

Measured by `--check` against the rasterizer's interpolated position, camera
20 m from the scene, near 0.1, far 100. (The numbers travel through a half
float modulo 1/16 m, which resolves 0.03 mm: the reversed-depth figures are
at that floor.) 4 mm is enough to move the highlight of a lamp 30 cm from a
metal surface by a visible amount.

Reversed depth needs nothing from `vk.h` beyond what it has: the projection
is built in `app_frame`, the clear value is 0 and the test `CMP_GREATER`.

## Clusters

`cluster.slang`. Cluster = screen tile × depth slice; slices are exponential
(`0.5 · (far / 0.5)^(k / 24)`), slice 0 reaches back to the camera and the
last one has no far end. One compute thread per cluster builds the cluster's
bounding box in view space and tests every light's sphere against it. The
record is a count and up to 511 indices, in ascending light order.

The box is padded by 0.1 %: the lighting pass decides a pixel's cluster from
a position with float error in it, and a pixel on a boundary has to be
covered from both sides.

Lights beyond their radius contribute exactly 0 (`mu_point_attenuation`
windows the falloff, and the loop skips them outright), and both loops —
the cluster's list and "every light" — walk the lights in the same order.
So culling does not approximate anything: clustered and `--brute` images are
required to be bit-identical, and are.

The falloff stops growing closer than 25 cm (`LAMP_RADIUS`). Lights pass
through objects in this scene, and an unbounded 1/d² next to a surface
produced HDR values of 1 300 whose exact size depended on the last bits of
the position.

Cost: the test is brute force, clusters × lights (3 456 × 1 024 sphere–box
tests per frame). Nothing is done about clusters with no geometry in them.

## Acceptance (`--check`)

Two cameras (an overview, and a low one 5 m from the objects), 1 024 lights.

| Line | Rule | Measured, 640 × 360 |
|---|---|---|
| clustered vs all lights (HDR target, every pixel, max difference) | == 0 | 0 / 0 |
| clusters over capacity (lists read back from the storage buffer) | == 0 | 0 / 0 |
| longest list | ≤ 511 and < all lights | 62 / 56 |
| mean list / all lights | < 0.05 | 0.0034 / 0.0067 |
| bad or unordered indices in the lists | == 0 | 0 / 0 |
| deferred vs forward, 8-bit output, covered pixels | ≤ 1 code value | 1 / 1 |
| normal error of the G-buffer's 16 + 16 bits | < 0.0001 | 0.000064 |
| position from depth, max error | < 0.5 mm | 0.09 mm |

The roadmap's criterion was "equals a brute-force forward loop within
1/255"; that is the sixth line. The dither of `Present` is switched off for
the comparison. Sky pixels are not compared (the two programs draw the sky
differently).

With `--lights 4096` the check also passes at 320 × 180; the longest list is
then 252.

## Time

lavapipe (a CPU rasterizer), 1280 × 720, wall time per frame from the
difference between a 3-frame and a 1-frame run. Not a GPU measurement; the
ratios will be different on one.

| | 1 024 lights | 4 096 lights |
|---|---|---|
| clustered | 0.16 s | 0.42 s |
| `--brute` (G-buffer, every light per pixel) | 1.8 s | 7.2 s |
| `--forward` | 2.4 s | not measured |

The pass timer (`--perf`) reports 0.05 ms for `ClusterLights`. That is not
believable for 3.5 million sphere tests and is not quoted as a result;
lavapipe's timestamps around a compute dispatch were not investigated.

## Not done

- No shadows from the point lights, no light volumes or markers showing
  where the lamps are.
- No transparency: a deferred renderer needs a forward pass for it, and the
  cluster lists are what that pass would use. `fragForward` exists but takes
  the brute-force path only.
- No depth-bounds or "active cluster" pass; the grid is fixed at
  16 × 9 × 24.
- A list longer than 511 is truncated and the image is then wrong where it
  happens. View 5 shows such clusters in magenta and `--check` fails on
  them; nothing prevents it.
- Run on lavapipe only. The windowed mode was started under Xvfb for a few
  seconds (no validation messages); the panel's controls were not operated.
