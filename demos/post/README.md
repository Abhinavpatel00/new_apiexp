# post — bloom chain and display-transform lab

A procedural HDR scene (emissive shapes from 0.06 to several hundred nits-
equivalent, a brightness ramp, colour sweeps) pushed through a five-level
bloom chain and every tone mapper in `external/mu_lib/mu/shaders/post.slang`.

```
Scene (RGBA16F)
  -> Down0 (threshold + knee, optional Karis weight) -> Down1 -> ... -> Down4    13-tap
  -> Up3 -> Up2 -> Up1 -> Up0                                                     3x3 tent, additive
  -> Composite (sharpen, + bloom, exposure, tone map, vignette, grain, dither)
  -> UI
```

One render target per level. An upsample pass blends additively
(`ATT_WRITE_KEEP`) into the level it enlarges onto, so the chain needs five
targets and no copies.

```sh
./build/cm/post
./build/cm/post --headless out.ppm --time 1
./build/cm/post --headless cmp.ppm --time 1 --compare     # eight tone mappers side by side
./build/cm/post --headless bloom.ppm --time 1 --bloom-only
./build/cm/post --check
```

`--tonemap N`: 0 none, 1 Reinhard, 2 ACES fitted, 3 ACES, 4 neutral,
5 Uchimura, 6 Hable, 7 AgX. `--exposure EV`, `--strength X` (0 is no bloom), `--no-karis`.

## Acceptance check

| Check | Why it must hold | Limit | Measured |
|---|---|---|---|
| sum of the bloom output / (levels x sum of the scene), threshold 0, Karis off | every kernel's weights sum to 1, so each level carries the scene's energy once | 1 ± 0.01 | 0.998442 |
| sum of the bloom output with the threshold at 1e6 | nothing passes, so nothing may come out | exactly 0 | 0 |
| output bytes that differ between "threshold 1e6" and "bloom strength 0" | the composite must not change when the bloom is black | 0 | 0 |

The 0.16 % of missing energy leaves through the clamped borders of the
smallest levels.

## Different from the roadmap

The roadmap put this demo on top of `galaxy`. It is a standalone scene
instead: `galaxy` is hand-rolled around its own frame loop, and a known scene
(shapes of chosen luminance) is what makes the energy check possible.
