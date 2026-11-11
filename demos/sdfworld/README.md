# sdfworld — a deformable world made of distances

No triangles. The world is a volume that stores, per voxel, the distance to
the nearest surface; it is drawn by stepping rays through it, and changed by
writing new distances into a brush-sized box. A digger cuts a trench, a
builder winds a tower, stones fall and leave craters, lumps of clay fall and
stick, and the left mouse button digs or adds clay under the pointer.

```
Edit (compute: brushes, in place, only inside each brush's box)
  -> Occupied -> ReachX -> ReachY -> ReachZ (compute: the empty-space level)
  -> Pick, Balls (compute: the pointer's ray; falling stones and clay)
  -> Prepass (1/8 resolution cone trace: a safe start depth per 8 x 8 block)
  -> Trace (sphere tracing, soft shadow, ambient occlusion; RGBA16F)
  -> Present -> UI
```

This is one demo, not several. What it takes from the work on distance
fields, and from whom:

| Technique | From | Where |
|---|---|---|
| Sphere tracing: step by the distance | Hart 1996 | `volumeTrace` |
| Volume with distances stored only within ±4 voxels of the surface, so that an edit is local | Aaltonen, *GPU-based clay simulation and ray-tracing tech in Claybook*, GDC 2018 | `edit.slang` |
| A coarser level for empty space | Claybook's dilated mip levels, reduced to one level | `coarse.slang` |
| Over-relaxed steps, with a check that falls back to plain ones | Keinert et al., *Enhanced sphere tracing*, 2014 | `volumeTrace` |
| Hit when the surface is closer than the ray is wide | Claybook (`D < pixelConeWidth * t`) | `volumeTrace` |
| Low-resolution cone-trace pre-pass | Claybook (8 × 8 pixel bounding cones) | `fragPrepass`, `volumeConeStart` |
| Soft shadow from one ray, as the narrowest distance / travelled ratio | Quilez; Claybook | `volumeShadow` |
| …with the closest approach estimated from two successive samples | Aaltonen, as published in Quilez' *Soft shadows in raymarched SDFs* | `volumeShadow` |
| Ambient occlusion sampled from the field along the normal | Quilez; in spirit Wright's distance-field AO (UE4) and Evans, *Learning from failure* (Dreams) | `volumeAO` |
| Smooth union and subtraction | Quilez (polynomial smooth min, via `mu_lib`'s `sdf.slang`) | `compEdit` |
| The world is changed by simulated bodies, on the GPU | Claybook (particles cut and stamp the world) | `compBalls` → `compEdit` |

## The volume

128 × 64 × 128 voxels of 25 cm (32 × 16 × 32 m) in one RGBA16F 3D texture:
one trilinear fetch per sample. Red is the distance in voxels, clamped to
±4; green, blue and alpha are the clay's colour. 8.4 MB.

(The first version kept the 64 slices as tiles of a 1024 × 1024 2D texture
and blended two fetches by hand, because `vk.h` could not make a 3D view.
The port changed no number in the tables below except the times.)

Claybook's is 1024 × 1024 × 512 at 8 bits with five mip levels. This one is
1/512 of that, 16-bit, with one coarse level.

## Editing

A brush is one compute dispatch over `radius + 4 + smoothing` voxels around
its centre. That box is enough because the stored distance is clamped:
further away the result is the old value, bit for bit. This is the reason
Claybook gives for the limited range: with an unlimited one, a local change
has to be propagated through the whole volume.

The CPU places the digger and the builder. It does not place the other two
kinds of brush and never reads the world:

- **Pointer.** `compPick` traces the pointer's ray and stores the hit in the
  storage buffer. Next frame's `Edit` pass reads the centre from there.
- **Balls.** `compBalls` moves 24 balls, tests each against the distance
  field (at the new position and halfway), and stores an impact. Next
  frame's `Edit` pass digs a crater there (stones, grey) or adds a lump
  (clay, coloured), and the ball starts again from the sky.

Both passes also write the arguments of their brush's dispatch: the brush's
box in groups, or zeros. `Edit` uses `pass_dispatch_indirect`, so a ball in
the air and a pointer over the sky cost no brush work.

## Finding the surface

**Coarse level.** One number per cell of 4³ voxels: the Chebyshev distance,
in cells, to the nearest cell holding anything closer than 4 voxels to a
surface, up to 9. A ray in a cell with D ≥ 2 jumps (D − 1) · 4 voxels.
Chebyshev distance separates into three 17-tap passes; all 16 384 cells are
rebuilt every frame.

**Pre-pass.** At 1/8 resolution, a cone containing all 64 pixel rays of a
block advances from the eye while the whole cone is empty:
`step = d − r(t + d)`, with `r` the cone's radius. Each pixel ray then starts
where its block's cone stopped.

## Measured

`sdfworld --headless --check --size 640 360` first lets the world be
deformed for 240 frames, then stops everything and measures (lavapipe):

| | value | rule |
|---|---|---|
| one dig: voxels changed outside the brush's box | 0 | = 0 |
| one dig: voxels changed inside it | 2 206 | > 500 |
| one dig: largest difference from the same formula on the CPU | 0.0019 voxels | < 0.004 |
| one dig: share of the volume the pass visited | 1.2 % | reported |
| coarse level: cells that differ from a CPU rebuild | 0 of 16 384 | = 0 |
| coarse level: cells a ray can jump from | 55 % | reported |
| field: largest change between neighbouring voxels | 1.000 | ≤ 1.005 |
| field: mean gradient length within 2 voxels of the surface | 0.75 | reported |
| pointer: picked point to the surface drawn at that pixel | 0.23 voxels | < ¼ + 2 ray widths |
| pointer: voxels changed outside the box around the picked point | 0 | = 0 |
| stone: values changed while both balls' dispatches had zero groups | 0 | = 0 |
| stone: groups the GPU wrote for its brush / for the ball still in the air | 5 / 0 | = the box / = 0 |
| stone: voxels its crater changed, outside the box around the impact | 834, 0 | > 300, = 0 |
| stone: largest difference from the formula at the impact | 0.0020 voxels | < 0.004 |

| steps per ray | overview | low camera |
|---|---|---|
| plain sphere tracing | 18.2 | 14.4 |
| + coarse level | 10.0 | 13.6 |
| + pre-pass | 3.7 | 6.6 |
| + over-relaxation 1.2 (the default) | 3.3 | 6.3 |

| against plain sphere tracing (230 400 rays) | overview | low camera | rule |
|---|---|---|---|
| coarse level: hit/miss changed | 0 | 0 | = 0 |
| coarse level: hit moved more than 2 ray widths | 0 | 0 | = 0 |
| pre-pass: rays started beyond their plain hit | 0 | 0 | = 0 |
| pre-pass: hit/miss changed | 21 | 4 | < 0.05 % |
| pre-pass: hit moved more than 2 ray widths | 15 | 3 | < 0.05 % |
| pre-pass: of those, rays not on a silhouette | 0 | 0 | = 0 |
| all three: hit/miss changed | 29 | 5 | < 0.05 % |
| all three: hit moved more than 2 ray widths | 15 | 0 | < 0.05 % |
| all three: of those, rays not on a silhouette | 0 | 0 | = 0 |

Time per frame at 1280 × 720 on lavapipe (a CPU; only the ratios mean
anything): 0.14 s for the still world with both accelerations, 0.22 s with
neither; 0.14 s with the digger and the builder, 0.21 s with 24 balls as
well. With the 2D atlas these were 0.16, 0.27 and 0.24 s.

## What the numbers say

**The coarse level changes nothing and saves little.** In empty space the
clamped distance is exactly 4 voxels, so a jump of (D − 1) · 4 lands where
D − 1 plain steps would have: the images are identical, and only the count
of samples drops. In a world 128 voxels across, a plain ray is never more
than a few dozen 4-voxel steps from anything, and from the low camera the
level saves 5 %. Raising its reach from 3 cells to 8 moved the overview from
11.3 to 10.0 steps. The steps that remain are near the surface, where the
level does not help. Claybook's five levels pay for themselves in a volume
512 times larger; this demo cannot show that.

**The pre-pass is what helps here**: 2.2 to 4.9 times fewer steps.

**Over-relaxation helps a little, and only when it is small.** Steps per ray
with coarse level and pre-pass, by factor:

| factor | 1.0 | 1.1 | 1.2 | 1.3 | 1.4 | 1.6 |
|---|---|---|---|---|---|---|
| overview | 3.67 | 3.41 | 3.25 | 3.24 | 3.53 | 4.21 |
| low camera | 6.56 | 6.20 | 6.34 | 6.42 | 6.60 | 7.47 |

11 % and 3 % at 1.2; above 1.4 it costs steps. A ray that meets the ground
head-on fails the overlap check on arrival, goes back, and has spent a step
for nothing; only grazing rays gain. Two variants were tried and dropped:
resuming over-relaxation after a failed step was worse at every factor
(3.74 and 7.39 at 1.4), and testing the overlap with |d|, as the check is
usually written, let one ray in 230 400 stop inside the solid after stepping
through the surface. The test here uses the signed distance.

**The pre-pass changes a few rays, all on silhouettes.** No ray starts
beyond the point where plain tracing stops, which is all the pre-pass
promises. But a ray stops at the first sample that is closer to the surface
than the ray is wide, and which sample that is depends on where the ray
started; a ray grazing an edge can stop on it or pass it. That is a property
of sphere tracing with a cone-width threshold, and the reason the depth rule
is "2 ray widths", not zero. A first version of this rule (¼ voxel) passed
at 640 × 360 and failed at 320 × 180, where rays are twice as wide.

**A first version of the pre-pass was wrong**, and the picture showed it:
its cone started where the block's centre ray enters the volume's box, and
at the box's edges other rays of the block enter earlier. The cone now
starts at the eye, with a lower bound for the distance outside the box.

**The field is not a true distance.** The terrain is a height field divided
by a bound on its slope, and smooth unions round distances down; the mean
gradient near the surface is 0.75, so near the ground a step covers about
three quarters of what a true distance would allow. It is the terrain that
is flat, not the edits: the starting world measures 0.74, 0.75 after 240
frames and 0.76 after 900, because sphere brushes are true distances. What
tracing needs is that the field never overestimates, and along the voxel
axes it does not (largest neighbour difference 1.000). The test cannot see
diagonals, where trilinear interpolation of such data can in principle be
steeper.

**Aaltonen's soft-shadow estimate makes little difference in this scene.**
View 5 uses the plain ratio: 0.4 % of pixels differ by more than 2 of 255,
and neither shows bands. Steps are at most 4 voxels here, which is probably
why.

## Not done, not tested

- Tested only by eye: adding clay (the dig formula is replayed on the CPU,
  the add formula is not), the balls, shadows, ambient occlusion.
- The balls cost 0.07 s a frame, and that is drawing them (every pixel tests
  every ball), not their brushes: dispatching the brushes indirectly instead
  of always changed the frame time by less than the measurement's noise. An
  earlier version of this file said `vk.h` had no indirect dispatch. It had;
  it was untested and unused.
- Balls do not bounce, roll or collide with each other, and cast no shadow.
  Claybook's clay is a particle simulation; this is not.
- No sparse storage, no streaming, no mip chain, no redistancing after
  edits.
- The validation layer does not check that a bindless slot read as a 3D
  texture holds one; nothing here would catch a mixed-up index except the
  results being wrong.
- Normals come from the trilinear field and show faint contour lines where
  the surface crosses voxel layers.
- Windowed mode was run only under Xvfb; the mouse path was exercised by the
  test through the same two passes, not by a mouse. The balls use a fixed
  1/60 s step, so on a slow device they fall in slow motion.

## Running

```
sdfworld                       # left button: orbit, or dig / add clay (panel)
sdfworld --headless out.ppm    # 240 frames, then the picture
sdfworld --still --view 1      # the starting world, steps per ray
sdfworld --check --headless --size 640 360
```

`--view N`: 0 lit, 1 steps per ray, 2 normal, 3 light only, 4 start depth
from the pre-pass, 5 light only with the plain shadow estimate.
`--no-coarse`, `--no-prepass`, `--relax W`, `--balls N`, `--still`, `--frames N`.

## References

- J. C. Hart, *Sphere tracing: a geometric method for the antialiased ray
  tracing of implicit surfaces*, The Visual Computer, 1996.
- S. Aaltonen, *GPU-based clay simulation and ray-tracing tech in Claybook*,
  GDC 2018.
- I. Quilez, articles on distance functions, smooth minimum and soft shadows
  (iquilezles.org).
- B. Keinert et al., *Enhanced sphere tracing*, 2014.
- A. Evans, *Learning from failure: a survey of promising, unconventional
  and mostly abandoned renderers for Dreams PS4*, SIGGRAPH 2015.
- D. Wright, *Dynamic occlusion with signed distance fields*, SIGGRAPH 2015.

Only the Claybook slides were read while writing this; the others are cited
from memory.
