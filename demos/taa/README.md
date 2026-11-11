# taa — temporal anti-aliasing, measured against 64 samples per pixel

```
Scene (projection shifted within the pixel; colour + velocity + depth)
  ─► Resolve (history A ─► B; next frame B ─► A) ─► Present
```

| | technique | source |
|---|---|---|
| jitter | the projection moves by a fraction of a pixel each frame, 8-entry Halton(2, 3) | common practice |
| history | `history = lerp(history where this surface was last frame, current, 0.1)`, the old position from a velocity target (object and camera motion), fetched bilinearly | — |
| clamp | the fetched history is clamped to the range of the current frame's 3 × 3 neighbourhood, so stale history cannot stay | Karis 2014 |

A floor of 25 cm squares to the horizon, a fence of 2.4 cm bars and a ball
that moves at up to 2 m/s: things that alias.

## Acceptance (`--check`)

640 × 360. **Reference:** 64 frames, each shifted to one cell of an 8 × 8 grid
inside the pixel, read back unresolved and averaged on the CPU — 64 samples
per pixel, with no part of the resolve shader in it. "Error" is the mean
absolute difference from it in linear colour (white = 1).

Nothing moves:

| Line | Rule | Measured |
|---|---|---|
| no anti-aliasing, error | > 0.005 (the scene does alias) | 0.048 |
| TAA after 64 frames | < half of that | 0.013 |
| TAA without clamp after 64 frames | < half of that | 0.010 |
| TAA, change from one frame to the next | < 0.01 (a guard, not derived) | 0.0056 |

Is the velocity right? One frame is drawn 4/60 s before the moment tested;
the next only fetches that picture from where the velocity points (new-frame
weight 0, no clamp) and is compared with a frame drawn at the moment itself:

| | old picture left in place | fetched along the velocity (rule: < ¼ of the left) |
|---|---|---|
| ball moves; pixels on the ball (3 948) | 0.080 | 0.0029 |
| camera turns at 0.3 rad/s; lowest quarter of the picture | 0.075 | 0.0098 |

(The camera test uses the lowest quarter because further up the floor
aliases, and aliasing does not move with the surface. Over the whole picture
the same two numbers were 0.116 and 0.058.)

Sixty frames of motion, then the last one against the reference of that
instant:

| | ball moves; the 5 872 pixels the last 0.25 s changed | camera turns; whole picture |
|---|---|---|
| no anti-aliasing | 0.019 | 0.047 (rule: > TAA) |
| TAA | **0.031** | 0.035 |
| TAA without clamp (rule: > TAA) | 0.059 | 0.043 |
| TAA without velocity (rule: > TAA) | 0.036 | 0.056 |

Ball moves, whole picture: 0.048 without, 0.014 with TAA.

## What the numbers say

- Standing still, TAA removes most of the aliasing (0.048 → 0.013) but the
  picture keeps changing by 0.0056 per frame: the clamp lets each frame's
  jittered samples through. Without the clamp the still picture is better
  (0.010).
- **Where the ball has just passed, this TAA is worse than no anti-aliasing**
  (0.031 against 0.019). The ball drags a visible trail. My guess, not
  tested: on a black-and-white floor nearly every colour lies inside the
  neighbourhood's range, so the clamp removes little. The clamp and the
  velocity each still help (0.059 and 0.036 without them).
- While the camera turns the gain is small: 0.047 → 0.035.
- The velocity target is right for both kinds of motion, to the extent the
  fetch test shows.

## Time

lavapipe, 1280 × 720, wall time per frame (45-frame run minus 5-frame run,
divided by 40), two runs: no anti-aliasing 32–33 ms, TAA 53–54 ms, TAA
without clamp 42–43 ms. ("No anti-aliasing" still runs the resolve pass as a
copy and writes the velocity target.)

## Not done, not tested

- lavapipe only. Windowed: see the last line of this list.
- The parts that production TAA adds against exactly the failures measured
  above are **not built**: clamping in YCoCg or to a variance box, rejecting
  history by depth or velocity difference, taking the velocity of the nearest
  neighbour, a sharper (bicubic) history fetch, weighting in tonemapped
  space, sharpening afterwards. So the numbers describe the plainest version,
  not what TAA can do.
- One scene, chosen to alias. One speed of ball and of camera.
- The reference is a box filter over the pixel. TAA with a different
  reconstruction filter would be marked down by it.
- Nothing transparent, no particles (they have no velocity).
- No upscaling.
- The window was run for a few seconds under Xvfb with the validation layer;
  the panel's controls were not operated.

## Running

```
./build/cm/taa                                     # window; left drag orbits
./build/cm/taa --headless out.ppm --still --frames 40
./build/cm/taa --headless out.ppm --check --size 640 360
```

`--off`, `--no-clamp`, `--no-velocity`, `--alpha X` (weight of the new frame,
default 0.1), `--spin X` (camera rotation in rad/s), `--still`.

## References

- Brian Karis, *High Quality Temporal Supersampling*, SIGGRAPH 2014 course
  "Advances in Real-Time Rendering". Cited from memory.
