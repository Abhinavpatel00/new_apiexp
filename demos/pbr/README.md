# pbr — metallic/roughness with image-based lighting

A 7 x 5 grid of spheres under a procedural sky: roughness left to right;
rows from the top are metal, half metal, dielectric, clearcoat, sheen.

```
only when the sky changes (compute, storage images, all RGBA16F):
  Sky          -> level 0, 256^2            the sky, without the sun's disc
  Prefilter1-4 -> 128^2, 64^2, 32^2, 32^2   GGX, roughness 0.25 / 0.5 / 0.75 / 1
  Irradiance   -> 32^2                      cosine-weighted mean (E / pi)
  DFG          -> 64^2                      split-sum scale and bias over (NoV, roughness)

every frame:
  Background -> Forward (RGBA16F + D32F) -> Present -> UI
```

There are two frame programs, one with the seven bake passes in front and one
without; `app_frame` picks one per frame. The sun is an analytic directional
light on top of the baked sky.

| What | Where |
|---|---|
| GGX `D`, height-correlated Smith `V`, Fresnel, direct light, clearcoat and sheen lobes | `mu_lib` `lighting.slang` |
| GGX and cosine importance sampling, tangent frames | `mu_lib` `brdf_util.slang` |
| octahedral mapping, the sky, Hammersley points | `shaders/pbr_common.slang` |
| the four bake passes | `shaders/bake.slang` |
| split-sum assembly, multiple scattering, layers | `shaders/pbr.slang` |

First demo with storage images written by compute inside a frame program, and
with more than one frame program.

```sh
./build/cm/pbr
./build/cm/pbr --headless out.ppm --maps                  # baked maps along the bottom
./build/cm/pbr --headless furnace.ppm --furnace --energy 0
./build/cm/pbr --headless blur.ppm --background 2 --view 1
./build/cm/pbr --check
```

`--energy`: 0 single scattering, 1 multiple scattering (default), 2 `mu_lib`'s
`mu_pbr_indirect` unchanged. `--dfg`: 0 baked table, 1 `mu_env_brdf_approx`.
`--view`: 0 shaded, 1 specular IBL, 2 diffuse IBL, 3 sun only.

## Environment maps

`vk.h` has no cube targets and no per-mip writes, so the environment is
octahedral (the sphere of directions unfolded into a square) with one target
per roughness level; the shader blends the two nearest levels. The maps are
sampled with clamp addressing, which is wrong by up to half a texel along the
square's edge. The edge is the lower hemisphere's fold, where the ground is
dark and smooth; on a sky with detail there it would show as a seam.

## Acceptance check

| Check | Limit | Measured |
|---|---|---|
| DFG table against a brute-force integral on the CPU (midpoint rule over 360 000 light directions, double precision), 12 points with roughness >= 0.3 | error <= 0.01 | 0.0036 |
| irradiance map of a sky that is white above the horizon and black below, against the exact `(1 + n.y) / 2`, all 1024 texels | error <= 0.02 | 0.0074 |
| white sky: every prefiltered level and the irradiance map | within 0.002 of 1 | 0 |
| white furnace, mean of each sphere, 7 roughnesses x metal / half metal / dielectric | 1 ± 0.02 | 0.99991 … 1.00000 |
| white furnace, worst single pixel | within 0.03 of 1 | 0.0005 |
| control: the same with single scattering, roughest metal sphere | < 0.9 | 0.417 |

What the furnace test does and does not prove: with multiple scattering the
reflected and transmitted parts sum to 1 for *any* DFG value, by construction
of the formula. So the furnace rows verify the normalisation of the filters,
the level blending, the metalness mix and the layering — not the table. The
table is verified by the first row.

Two more numbers are printed and not asserted:

- `mu_env_brdf_approx` (the analytic fit) is off by up to 0.18 from the
  brute-force integral at the same 12 points. The demo uses the baked table
  by default; `--dfg 1` switches to the fit.
- With `--energy 2` the brightest white metal sphere reads **1.94** in a room
  of brightness 1.

### Found by the check

`mu_multi_scatter_fresnel` (`mu_lib`, `lighting.slang`) divides by
`1 - (1 - Ems) * Favg`; the published formula (Fdez-Agüera 2019) divides by
`1 - Ems * Favg`. For `F0 = 1` it returns `2 * Ess` instead of 1, so a smooth
white metal reflects almost twice the light that reaches it. With `F0 = 0.04`
the error is under 1 %, which is why it does not show on dielectrics.
`mu_pbr_indirect` also adds the diffuse term without removing what the
specular lobe reflected. The demo assembles the indirect light itself
(`multiScatter()` in `pbr.slang`); `mu_lib` is unchanged.

## Not done

- No bloom or other post pass beyond `Present`.
- Clearcoat and sheen are outside the furnace test. The clearcoat's own lobe
  uses no DFG term, and the sheen's environment response is a rim-weighted
  irradiance, not an integrated Charlie lobe.
- The spheres cast no shadows on each other.
