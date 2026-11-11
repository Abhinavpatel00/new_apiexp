# oit — transparency without sorting, measured against depth peeling

Twelve tinted slabs and bubbles over an opaque scene, drawn three ways:

```
weighted blended   Opaque ─► Accum (add) ─► Reveal (multiply) ─► Composite ─► Present
depth peeled       Opaque ─► [Peel k ─► Over k] × 12, farthest layer first ─► Present
draw order         Opaque ─► Blend ─► Present
```

| | technique | source |
|---|---|---|
| weighted blended | every transparent fragment adds `w · (colour · α, α)` to one target and multiplies another by `1 − α`; the composite is the weighted average colour over the background, with coverage `1 − Π(1 − α)` | McGuire & Bavoil 2013 |
| depth peeled | pass k keeps, per pixel, the farthest fragment nearer than the one pass k − 1 kept, and lays it over the picture | Everitt 2001, run back to front |
| draw order | ordinary blending, objects in the order they were added | — |

Weighted blending needs no sorting and two passes whatever the number of
layers. It gets the **coverage** exactly and **guesses the colour**: an
average cannot know which layer is in front, so the weight `w` favours
nearer fragments. `--weight` picks it:

- `0` the paper's general-purpose weight, a function of view depth in metres;
- `1` none (`w = 1`);
- `2` the shape of the paper's depth-buffer weight, `3000 · (1 − d)³`, with
  `d` rescaled to run from the nearest to the farthest transparent object in
  view. The rescaling is mine: with this camera's near and far planes the
  depth-buffer value is above 0.99 for the whole scene.

The weights are written from memory of the paper, not copied from it.

## Acceptance (`--check`)

640 × 360, two cameras: looking along the stack of five slabs, and across it.
Errors are mean absolute differences from the peeled reference in linear
colour (white = 1), over the pixels with that many transparent layers.

What is exact:

| Line | Rule | Along / across |
|---|---|---|
| pixels with something transparent | > 10 % | 28 % / 23 % |
| most layers on a pixel | ≤ 12, what the reference separates | 7 / 4 |
| reference drawn in reverse order, largest difference | == 0 | 0 / 0 |
| weighted drawn in reverse order, largest difference | < 0.005 | 0.0015 / 0.0010 |
| draw order drawn in reverse order, mean difference | reported | 0.049 / 0.016 |
| no transparent layer: weighted vs reference, largest | == 0 | 0 / 0 |
| one layer: weighted vs reference, largest | < 0.003 | 0.0005 / 0.0010 |
| black glass, any number of layers (only coverage is left), largest | < 0.003 | 0.0005 / 0.0002 |

What is guessed — mean error by number of layers:

| Along the stack | 2 layers | 3–4 | 5+ |
|---|---|---|---|
| pixels | 14 052 | 10 023 | 4 459 |
| weighted, general weight (rule: < 0.06) | 0.032 | 0.032 | 0.032 |
| weighted, fitted weight | 0.034 | 0.024 | 0.025 |
| weighted, no weight | 0.042 | 0.073 | 0.071 |
| draw order (here mostly back to front) | 0.009 | 0.032 | 0.031 |
| reverse draw order | 0.085 | 0.117 | 0.124 |
| largest error on a pixel, general / fitted | 0.11 / 0.11 | 0.19 / 0.12 | 0.14 / 0.11 |

| Across the stack | 2 layers | 3–4 | 5+ |
|---|---|---|---|
| pixels | 11 948 | 4 107 | 0 |
| weighted, general weight (rule: < 0.06) | 0.008 | 0.019 | — |
| weighted, fitted weight | 0.016 | 0.026 | — |
| weighted, no weight | 0.015 | 0.022 | — |
| draw order | 0.020 | 0.048 | — |
| reverse draw order | 0.016 | 0.061 | — |
| largest error on a pixel, general / fitted | 0.13 / 0.16 | 0.16 / 0.19 | — |

The 0.06 rule is a guard against regressions set above what was measured. It
is not a quality bar from anywhere.

## What the numbers say

- Everything that does not depend on order is right to the precision of the
  16-bit float targets: no layer, one layer, and the coverage of any number
  of layers.
- With two or more layers the colour is off by 0.01–0.03 on average and by up
  to 0.19 on single pixels. The picture shows it: the dark slab in front
  comes out lighter than in the reference.
- A depth weight is worth having looking along the stack (0.032 against 0.07
  without). Looking across it, it makes little difference.
- Neither weight wins everywhere. The fitted one is better with many layers
  along the stack and worse across it. I did not find out why, and tried no
  others.
- Unsorted ordinary blending is as good or better when the draw order happens
  to be right and three to four times worse when it is reversed. Weighted
  blending gives the same picture either way.

## Time

lavapipe, 1280 × 720, wall time per frame (45-frame run minus 5-frame run,
divided by 40), two runs:

| weighted blended | depth peeled, 12 layers | draw order |
|---|---|---|
| 41–47 ms | 214–249 ms | 25–26 ms |

## Not done, not tested

- lavapipe only. The windowed mode ran for a few seconds under Xvfb with the
  validation layer (no messages); the panel's controls were not operated.
- One scene, two cameras, alpha between 0.25 and 0.7. The paper's known weak
  cases — nearly opaque layers, and large depth ranges — are not in it.
- Colours stay below 1. Bright emissive transparent surfaces were not tried.
- Back faces are culled, so a bubble is one layer, not two.
- The reference peels 12 layers and the check asserts no pixel has more. A
  scene with more would need more passes; that limit is not handled, only
  asserted.
- Accum and Reveal are two passes drawing the geometry twice, because
  `vk.h` has one blend state per pass. With per-attachment blending they are
  one pass with two targets. So the time above overstates the method's cost.
- No MSAA, no refraction, no transparent shadows.

## Running

```
./build/cm/oit                                   # window; left drag orbits
./build/cm/oit --headless out.ppm --mode 1       # the reference
./build/cm/oit --headless out.ppm --check --size 640 360
```

`--mode N` (0 weighted blended, 1 depth peeled, 2 draw order), `--weight N`
(0 general, 1 none, 2 fitted), `--reverse` (draw the transparent objects in
the opposite order).

## References

- Morgan McGuire, Louis Bavoil, *Weighted Blended Order-Independent
  Transparency*, JCGT 2(2), 2013.
- Cass Everitt, *Interactive Order-Independent Transparency*, NVIDIA, 2001.
