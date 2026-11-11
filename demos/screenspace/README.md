# screenspace — ambient occlusion and reflections from the depth buffer

```
GBuffer (albedo, world normal, view depth)
  ─► Mirror (the same scene reflected in the floor: the reference for reflections)
  ─► AO ─► Blur ─► SSR ─► Planar ─► Composite ─► Present
```

Two effects that know nothing but what the camera sees, each next to
something it can be measured against: the occlusion against the same
estimator with 256 samples, the reflections against the scene actually
mirrored in the floor.

| | technique | source |
|---|---|---|
| occlusion | 16 points in the hemisphere over each pixel (cosine-weighted, Hammersley), blocked when the depth buffer is nearer; kernel rotated by one of 16 angles per pixel | Mittring 2007 (Crytek), hemisphere form after Filion & McNaughton 2008 |
| blocked test | the surface seen at the sample must also rise above the pixel's tangent plane | the idea of horizon/Alchemy-style estimators (McGuire et al. 2011) |
| blur | 4 × 4 average over pixels of similar depth — one full tile of the rotation pattern | common practice |
| reflections | the reflected ray marched in view space in equal steps, compared with the depth buffer; 6 halvings on a crossing; "behind by more than the thickness" = hidden, keep going | common practice; McGuire & Mara 2014 describe the better pixel-stepping version, not built |
| reference | the scene drawn again mirrored in the floor plane, shaded the same way | planar reflections |

All cited from memory.

## Acceptance (`--check`, 640 × 360)

Occlusion, over the pixels of geometry:

| Line | Rule | overview | low camera | from above |
|---|---|---|---|---|
| 256 samples: pixels below 0.8 | > 1 % | 3.2 % | 3.9 % | 2.1 % |
| mean / darkest | reported | 0.980 / 0.08 | 0.975 / 0.05 | 0.986 / 0.04 |
| 16 samples as sampled vs 256, mean difference | < 0.01 | 0.0052 | 0.0062 | 0.0042 |
| largest | reported | 0.25 | 0.27 | 0.20 |
| 16 samples blurred vs 256, mean difference | < as sampled | 0.0036 | 0.0042 | 0.0025 |
| largest | reported | 0.46 | 0.33 | 0.30 |
| **the floor alone: lowest value** | == 1 | 1 | 1 | — |

Samples per pixel (`--samples N`), mean difference to 256, overview / low camera:

| samples | as sampled | blurred |
|---|---|---|
| 4 | 0.0120 / 0.0144 | 0.0078 / 0.0092 |
| 8 | 0.0075 / 0.0090 | 0.0048 / 0.0057 |
| 16 (default) | 0.0052 / 0.0062 | 0.0036 / 0.0042 |
| 32 | 0.0035 / 0.0041 | 0.0031 / 0.0036 |
| 64 | 0.0022 / 0.0027 | 0.0030 / 0.0032 |

Reflections, over the floor pixels whose true reflection shows an object
(15 580 / 33 138 / 12 678 pixels); "from above" is reported, not judged:

| Line | Rule | overview | low camera | from above |
|---|---|---|---|---|
| found, colour within 0.05 | > 90 % (guard) | 94.8 % | 97.2 % | 83.2 % |
| not found: sky shown instead | reported | 2.7 % | 1.7 % | 4.8 % |
| something else shown | < 5 % (guard) | 2.5 % | 1.1 % | 12.0 % |
| where found, mean colour difference | < 0.01 | 0.003 | 0.001 | 0.001 |
| an object shown where the true reflection is sky, share of the floor | < 0.5 % | 0.07 % | 0.14 % | 0.13 % |

Steps and assumed thickness (`--steps N --thickness X`); found / sky instead /
something else, in %:

| steps, thickness | overview | low camera | from above |
|---|---|---|---|
| 24, 0.6 m | 88.6 / 4.9 / 6.5 | 91.4 / 5.8 / 2.8 | 79.5 / 11.8 / 8.7 |
| 48, 0.6 m | 92.6 / 3.7 / 3.7 | 96.0 / 2.7 / 1.3 | 80.9 / 8.5 / 10.6 |
| 96, 0.6 m (default) | 94.8 / 2.7 / 2.5 | 97.2 / 1.7 / 1.1 | 83.2 / 4.8 / 12.0 |
| 192, 0.6 m | 95.3 / 2.5 / 2.2 | 97.6 / 1.3 / 1.1 | 84.1 / 3.4 / 12.5 |
| 96, 0.15 m | 88.7 / 5.5 / 5.8 | 93.9 / 3.0 / 3.1 | 81.2 / 9.3 / 9.4 |
| 96, 2 m | 94.8 / 2.6 / 2.6 | 96.8 / 1.8 / 1.4 | 85.3 / 2.3 / 12.4 |
| 96, everything solid behind its surface | 94.3 / 2.4 / 3.3 | 96.8 / 1.8 / 1.4 | 81.4 / 1.9 / 16.8 |

## What the numbers say

- **The bare floor was the bug.** With the textbook test ("the depth buffer
  is nearer than the sample") a floor with nothing on it blocked up to 2 of
  16 samples at shallow angles: a neighbouring pixel's depth is that of its
  centre, and a sample lying almost in the floor compares against it and
  loses. Requiring the blocker to rise above the pixel's own tangent plane
  brings the bare floor to exactly 1 everywhere, and leaves the rest of the
  picture as it was (mean 0.981 → 0.980).
- The blur buys about one doubling of the sample count up to 16 samples and
  nothing beyond: it has an error of its own of about 0.003, from averaging
  across edges that the depth test alone does not see (the largest error on
  a pixel goes *up* with the blur, 0.25 → 0.46). At 64 samples the blurred
  picture is worse than the unblurred one. A normal-aware weight was not
  tried.
- What is measured is noise against the same estimator, not the estimator
  against the truth. Whether "hemisphere of 0.6 m, depth buffer as the
  world" resembles real ambient occlusion is not tested here.
- Reflections: at eye level screen space finds 95–97 % of what the mirror
  really shows, and where it finds something the colour is right to 0.003.
  From above it drops to 83 % with 12 % showing something else: the mirror
  should show undersides and far sides, the screen holds only tops and near
  sides, and the ray ends on whatever is in front. More steps do not help
  there (12.0 → 12.5 %); no screen-space method can.
- The thickness is a real trade: thin (0.15 m) loses rays that pass just
  behind a front edge, "everything solid" turns misses into wrong hits
  (from above 12 → 17 %). The scene is forgiving — every face is one flat
  colour, so a hit on the wrong part of the right face still counts as
  found.

## Time

lavapipe, 1280 × 720, difference of two runs of different length, measured
twice: the default picture 244–248 ms; with 1 sample and 1 step
104–112 ms (two G-buffers and five full-screen passes); 64 samples
463 ms; 24 steps 190–205 ms; 256 samples 1.3 s. That makes roughly 70 ms for 16
samples and 65 ms for 96 steps — both derived from those differences, not
timed alone.

## Not done, not tested

- Reflections only on the floor, mirror-sharp, no roughness, no fade at the
  screen's edge, no fallback other than a sky gradient. Rays march in view
  space, so distant stretches skip pixels and near ones revisit them.
- No temporal filtering of either effect; no half-resolution passes.
- The occlusion dims the ambient term only, as it should, which also means
  it is faint in a sunlit picture: `--view 1` shows it alone.
- The planar pass always runs, also when its result is not shown.
- Samples that fall off the screen count as open; the darkening at the
  picture's border that follows was not measured.
- lavapipe only; the window was opened under Xvfb, the panel not operated.

## Running

```sh
./build/cm/screenspace                 # occlusion + screen-space reflections
./build/cm/screenspace --mode 2        # the mirrored scene instead
./build/cm/screenspace --view 1        # occlusion alone; --no-blur: as sampled
./build/cm/screenspace --view 2        # what the floor reflects
./build/cm/screenspace --headless out.ppm --size 640 360 --check
```

`--mode 0|1|2`, `--view 0|1|2`, `--no-ao`, `--no-blur`, `--samples N`,
`--radius R`, `--steps N`, `--thickness X`.

## References

- M. Mittring, *Finding Next Gen — CryEngine 2*, SIGGRAPH 2007 course.
- D. Filion, R. McNaughton, *StarCraft II: Effects & Techniques*, SIGGRAPH 2008 course.
- M. McGuire et al., *The Alchemy Screen-Space Ambient Obscurance Algorithm*, HPG 2011.
- M. McGuire, M. Mara, *Efficient GPU Screen-Space Ray Tracing*, JCGT 2014.
