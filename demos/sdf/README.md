# sdf — sphere-traced scene

One full-screen fragment shader marches a signed-distance scene. No geometry,
no buffers: the smallest complete use of the demo framework
(`demos/common/demo.h`) and of `external/mu_lib/mu/shaders/sdf.slang`.

```
Trace (full-screen, RGBA16F, full or half resolution)
  -> Present (exposure, tone map, dither, bilinear upsample when --half)
  -> UI
```

| Shown | From `mu_lib` |
|---|---|
| scene: smooth unions, a subtraction, polar repetition, a twisted column | `mu_sd_*`, `mu_sd_smooth_union`, `mu_sd_polar` |
| the march, with its step count | `mu_sdf_raymarch_stats` |
| normals, soft shadow, ambient occlusion | `mu_sdf_normal`, `mu_sdf_soft_shadow`, `mu_sdf_ao` |

```sh
./build/cm/sdf                                   # window; F1 hides the panel
./build/cm/sdf --headless out.ppm --time 1.5
./build/cm/sdf --headless steps.ppm --view 2     # step-count heat map
./build/cm/sdf --check --size 640 360
```

`--view`: 0 shaded, 1 normals, 2 step count, 3 AO, 4 sun shadow. `--half`
traces a quarter of the pixels. `--steps N` sets the march budget.

## Acceptance check

`--check` replaces the scene with a single sphere and no ground, at three
radius/distance pairs, and counts the pixels the march reports as hit. A
sphere of radius r at distance d projects to an ellipse whose area is known in
closed form, so the count has an exact expected value.

| Check | Limit | Measured (lavapipe, 640x360) |
|---|---|---|
| hit pixels vs analytic silhouette area | within 1 % | -0.10 % … +0.003 % |
| mean steps per pixel | < 16 | 7.4 … 11.4 |
| rays that use the whole step budget | < 0.5 % | <= 0.063 % |

Rays that graze the silhouette always exhaust the budget (the distance to the
surface stops shrinking), so the third row asserts a fraction, not zero.
