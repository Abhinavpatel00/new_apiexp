# blackhole — what a camera sees next to a black hole

```
Trace (fullscreen, one light ray per pixel) ─► Present
```

A Schwarzschild (non-rotating) black hole with a thin disc of gas round it
and a sky of stars behind it. Each pixel follows its light ray backwards
until it falls through the horizon, crosses the disc or leaves.

Units: G = c = 1, lengths in units of the hole's mass M. Horizon at r = 2,
light can circle at r = 3, the disc runs from r = 6 (the innermost stable
circular orbit) to r = 17.

## The method

In the plane of its orbit a light ray obeys, with u = 1/r,

    u'' + u = 3 M u²

Without the right side the solution is a straight line. With it, this is
the orbit of a particle under a pull of `3 M h² / r⁴` towards the centre,
where h = |x × v| stays constant. So the shader steps a particle under
that force in ordinary 3-D coordinates (fourth-order Runge–Kutta, step
0.06 · r): no orbital plane to find, and nothing special at the horizon.
This form is exact for the path, not an approximation; it does not give
travel time, which the picture does not need.

The disc: gas on circular orbits, Ω = sqrt(M/r³). Temperature follows the
thin-disc profile T⁴ ∝ (1 − sqrt(6M/r)) / r³. The light is shifted by

    g = sqrt(1 − 3M/r) / ( sqrt(1 − 2M/r₀) · (1 − Ω · (direction the gas moves) · (ray)) )

for a camera at rest at r₀, and the picture shows a black body at g·T with
brightness × g⁴. That is why the side coming towards the camera is bright
and blue-white and the other side dim and red.

`--flat` draws the same scene with the bending switched off, `--grid`
replaces the stars with latitude and longitude lines (18 cells from pole to
pole), `--no-doppler` shows the disc as if at rest, `--view 3` shows g.

## Acceptance (`--check`)

640 × 360. The check renders to RGBA32F; the picture uses RGBA16F.

| Line | Rule | Result |
|---|---|---|
| **shadow area against π·(image radius of sin α = 3√3 M sqrt(1 − 2M/r) / r)², camera at r = 15 / 30 / 60 M** | within 1 % | −0.06 % / −0.21 % / +0.51 % |
| shadow radius ÷ radius the horizon would have without bending | reported | 2.53 / 2.54 / 2.57 |
| rays that ran out of steps | ≤ 8 | 0 |
| **fell in or left: rays where the shader and the orbit equation disagree** (127 rays, impact parameter more than 0.05 M from 3√3 M) | 0 | 0 |
| **direction a ray leaves in, against `u'' + u = 3Mu²` integrated on the CPU in double precision, b > 1.1 b_c** | < 1e-4 rad | 4.1e-6 rad |
| … for b_c < b < 1.1 b_c, where rays circle the hole first | reported | 1.5e-5 rad |
| **deflection ÷ (4M/b + 15π/4 · (M/b)²) − 1, 129 rays with b ≥ 60 M** | < 0.5 % | 0.29 % |
| deflection ÷ (4M/b) − 1 at b = 100 M (the second term predicts 2.9 %) | 2.5–3.3 % | 3.04 % |
| **disc seen exactly edge-on: pixels that show it, flat space** | 0 | 0 |
| … curved space | > 2 % | 7.3 % |
| **disc pixels with a redshift outside what a circular orbit at that radius allows** (44 984 pixels) | 0 | 0 |
| bluest / reddest g | > 1.1 / < 0.75 | 1.45 / 0.49 |

The shadow rows use a step of 0.02 · r and up to 4 000 steps; the picture
uses 0.06 · r and 400.

## What the numbers say

- **The shadow is 2.5 times wider than the horizon would look in flat
  space**, and its size matches Synge's formula to 0.5 % or better. Most of
  the remaining error is pixel counting: at r = 60 M the shadow is 32
  pixels in radius.
- **The 3-D particle form and the polar orbit equation are the same
  path.** Two different integrators in different coordinates agree to
  4e-6 rad on the direction rays leave in, and never disagree on which
  rays fall in.
- **Far from the hole the bending is Einstein's 4M/b**, and the 3 % by
  which it is larger at b = 100 M is the second-order term, not error.
- **An infinitely thin disc seen edge-on is invisible in flat space and
  covers 7 % of the picture in curved space**: the arc over the top of the
  hole is the far side of the disc, seen over the hole; the thin arc under
  it is the far side's underneath.

## Limits

- No rotation (Kerr). A spinning hole drags the picture sideways and
  flattens one side of the shadow; this needs a different integrator.
- The camera is at rest. It does not fall or orbit, so there is no
  aberration of the sky.
- The disc is infinitely thin and opaque, with a noise pattern for looks
  only; the colour is a black body, the brightness scale is a free
  parameter.
- One sample per pixel: the photon ring and the grid lines alias.
- Stars are not magnified in brightness near the Einstein ring, as real
  point sources would be.
- Lavapipe only; about 2 s per 1280 × 720 frame there.

## Sources

- O. James, E. von Tunzelmann, P. Franklin, K. Thorne, *Gravitational
  lensing by spinning black holes in astrophysics, and in the movie
  Interstellar*, Class. Quantum Grav. 32 (2015) — the inspiration, and the
  SIGGRAPH 2015 talk by the same team. Their renderer handles a spinning
  hole and traces ray bundles; this demo does neither.
- J. L. Synge, *The escape of photons from gravitationally intense stars*,
  MNRAS 131 (1966) — the angular size of the shadow.
- J.-P. Luminet, *Image of a spherical black hole with thin accretion
  disk*, Astron. Astrophys. 75 (1979) — the first such picture and the g⁴
  brightness rule.
