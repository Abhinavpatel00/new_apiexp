# shadows — shadow filtering lab

A directional light, four shadow maps, six filters side by side. The filters,
the normal offset and the cascade blend are
`external/mu_lib/mu/shaders/shadows.slang`.

```
Shadow0 (2048^2 D32F, whole scene)      depth only, pipe_intern_depth_only
Shadow1..3 (1024^2 D32F, cascades)      one pass and one target per map
  -> Forward (RGBA16F + D32F)           filter A | filter B, split screen
  -> Present -> UI
```

| `--technique` | Filter | Taps |
|---|---|---|
| 0 | one hard comparison | 1 |
| 1 | 3x3 hard comparisons | 9 |
| 2 | one hardware bilinear comparison | 1 (4 texels) |
| 3 | 3x3 hardware bilinear comparisons (`mu_pcf3x3`) | 9 (16 texels) |
| 4 | 16 Poisson taps, rotated per pixel (`mu_pcf_poisson`) | 16 |
| 5 | PCSS: blocker search, then a Poisson filter sized by blocker distance | 16 + 16 x rotations |

New library use: comparison samplers (`SamplerDesc.compare`), depth targets
sampled through the bindless image array, depth-only pipelines.

```sh
./build/cm/shadows                                         # PCF 3x3 left, PCSS right
./build/cm/shadows --headless out.ppm --technique 0 --right 5
./build/cm/shadows --headless c.ppm --cascades --view 2    # cascade tint, blend bands in yellow
./build/cm/shadows --headless acne.ppm --no-bias
./build/cm/shadows --calibration --check
```

## Bias

All three biases are in shadow-map texels, so they follow the resolution of
the map in use (a far cascade needs more world-space bias than a near one):
normal offset (`mu_normal_offset`), a constant, and a slope term
`tan(angle between normal and light)` multiplied by how many texels the
filter reaches sideways. A wide filter on a tilted receiver reads depths that
differ by exactly that much; a fixed bias is either too small for PCSS or too
large for a single tap.

The library has no rasterizer depth-bias call, so all of the bias is applied
on the receiver side.

## Cascades

Each cascade covers the bounding sphere of one slice of the view frustum
(constant size under camera rotation) and its centre is snapped to whole
texels (no crawling under camera translation). Within `blend width` of the
far end of a slice the result is a mix of this cascade and the next.

## Acceptance check

Run with `--calibration`: one thin plate, 2 x 2, floating 2.025 above the
ground; the light leans along z, so the plate's x edges cast straight.

| Check | Expected | Limit | Measured |
|---|---|---|---|
| x where the PCSS shadow crosses 50 % | 1 (the plate's edge) | ± 0.03 | 0.9933 |
| PCSS penumbra, 10 % to 90 % | 0.687 x W, W = light size x blocker distance = 0.08 x 2.173 | ± 15 % | 0.1212 = 1.015 x predicted |
| hard tap: pixels that are neither 0 nor 1 | 0 | 0 | 0 |
| open ground darkened at default bias, sun 34° above the horizon: hard, PCF 3x3, Poisson | 0 pixels | 0 | 0, 0, 0 |
| the same with all bias off (control: the check can fail) | acne | > 5 % of pixels | 50 %, 100 %, 100 % |
| mean difference between neighbouring cascades inside the blend bands | small | < 0.02 | 0 |

0.687 is the 10–90 % width of a straight edge swept across a uniform disc,
in units of the disc's diameter.

### Found by the check

`MU_POISSON16` fills the square [-1, 1]^2, not the unit disc: the farthest
point is 1.234 from the centre. Used as "a disc of the given radius" it makes
PCSS penumbrae 33 % too wide (measured 1.34 x, predicted from the table
1.33 x). The demo scales the PCSS radius by 0.765 so the table has the
variance of the disc it stands for; `mu_lib` itself is unchanged.

The cascade row is weaker than it looks: in the check's scene the blend bands
fall on open ground, where both cascades answer "lit". It proves the band
logic selects sane maps, not that two cascades agree along a shadow edge.
