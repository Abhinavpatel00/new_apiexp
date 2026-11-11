# gpu_driven — the GPU decides what to draw

4 096 buildings around a plaza and one ground slab, all boxes. The CPU uploads
them once; after that it sends a camera matrix and nothing else. Which boxes
are drawn is decided in compute and handed to the rasterizer through indirect
draw commands whose instance count the CPU never sees.

```
Reset (compute: both draw lists empty)
  -> CullA (compute)  -> DrawA (indirect)      what was visible last frame
  -> Pyramid0..N (compute)                     farthest depth, one pass per level
  -> CullB (compute)  -> DrawB (indirect)      what that depth does not hide, and A did not draw
  -> Present -> UI
```

| Part | Source | Where |
|---|---|---|
| Two phases: last frame's visible set, then the rest against this frame's depth | Haar & Aaltonen, *GPU-Driven Rendering Pipelines*, SIGGRAPH 2015 | `cull.slang` |
| Depth pyramid, test a screen rectangle against at most 2 × 2 texels of one level | Greene et al., *Hierarchical Z-Buffer Visibility*, SIGGRAPH 1993; the form used here follows Kapoulkine's *niagara* renderer | `occluded()`, `pyramid.slang` |
| Draw list as an indirect command plus an index list, filled with an atomic add | common practice; `demos/particles` does the same | `append()` |

Cited from memory; none was fetched while writing this.

## Why two phases

Testing against last frame's depth is cheap and wrong: when the camera turns,
a box that has just come into view is still marked hidden and is missing for
a frame. Here a box is skipped only if what has **already been drawn this
frame** hides it. Last frame's answer decides only whether a box is drawn in
phase A or phase B. With nothing remembered at all, phase A draws nothing and
phase B draws everything in the frustum: slow, and still the right picture.

## The occlusion test

A box's eight corners are projected; the result is a rectangle in pixels and
the nearest depth of the corners. The pyramid level is chosen so that the
rectangle touches at most 2 × 2 texels, and the box is hidden if its nearest
depth is beyond the farthest depth stored in those four texels (plus 2e-6).
A box with a corner behind the eye plane is never called hidden.

Pyramid levels are separate R32F targets, because `vk.h` cannot bind one mip
of a texture as a storage image. Each is half the one below, rounded down;
where the size below is odd a texel takes a third row or column, so the last
texel of a level reaches the edge and nothing is left out.

## Measured

`./build/cm/gpu_driven --headless out.ppm --size 640 360 --check`, lavapipe.
Twelve cameras in a row: cuts, turns, and one start with nothing
remembered.

| Check | Value | Rule |
|---|---|---|
| Pixels where the culled frame differs from drawing every box, all cameras | 0 | = 0 |
| The same for the lit picture, one camera | 0 | = 0 |
| Boxes that show a pixel but are in neither draw list | 0 | = 0 |
| Boxes in both draw lists, or twice in one | 0 | = 0 |
| List entries that are not a box | 0 | = 0 |
| Boxes where the GPU's frustum test and the CPU's disagree | 0 | = 0 |
| Third frame at one camera: boxes added in phase B | 0 | = 0 |
| Nothing remembered: boxes drawn in phase A | 0 | = 0 |

The comparisons are of the instance number per pixel (view 1), read back as
raw half floats. Also passes at 320 × 180 and 1280 × 720.

What was drawn, of 4 097 boxes:

| Camera | In the frustum | Drawn in A | Added in B | Show at least one pixel |
|---|---|---|---|---|
| cut to the plaza | 1 174 | 630 | 20 | 106 |
| plaza, the frame after | 1 174 | 201 | 0 | 106 |
| plaza, steady | 1 174 | 201 | 0 | 106 |
| plaza, turned 3° | 1 170 | 196 | 11 | 99 |
| plaza, turned 43° | 1 211 | 117 | 391 | 97 |
| cut to the overview | 2 972 | 447 | 2 453 | 1 646 |
| overview, the frame after | 2 972 | 2 888 | 0 | 1 646 |
| overview, steady | 2 972 | 2 635 | 0 | 1 646 |
| plaza, nothing remembered | 1 174 | 0 | 1 174 | 106 |

Time per frame at 1280 × 720 on lavapipe, camera still:

| Camera | frustum + occlusion | frustum only | no culling |
|---|---|---|---|
| plaza | 40 ms | 50 ms | 57 ms |
| overview | 61 ms | 61 ms | 58 ms |

## What the numbers say

**The picture is exactly the picture without culling**, at every camera
tried, including the frame of a cut.

**In the plaza, occlusion removes 83 % of what the frustum lets through**
(201 of 1 174), **and still draws twice what shows** (201 against 106). One
rectangle against one farthest depth cannot see that several buildings
together hide a box; it needs a single stretch of nearer depth across the
whole rectangle.

**From above it does almost nothing**: 2 635 drawn of 2 972, where 1 646
show. The ground is visible between the buildings almost everywhere, so
almost every rectangle contains a far depth.

**A large turn costs one frame of extra drawing, a cut costs two.** On the
frame of the turn phase B adds what is new (391). After a cut the first frame
tests against a nearly empty depth buffer, marks too much as visible, and the
next frame's phase A draws that (2 888 against 2 635).

**On this CPU rasterizer the plaza frame takes 40 ms instead of 57 without
culling (50 with the frustum test alone), and the overview takes 5 % longer
than without**: there the two cull passes and the pyramid cost more than the
337 boxes they remove. A box is 12 triangles; with real meshes the
balance would shift, and that is not measured here.

## Not done, not tested

- One mesh. With several, each needs its own command, written the same way;
  `pass_draw_indirect_count` exists in `vk.h` and is not used here.
- No clusters or meshlets: a box is culled whole.
- No triangle or back-face cluster culling, no small-primitive culling.
- No levels of detail.
- The frustum test accepts a box unless all eight corners are outside one
  plane, so a box beside a corner of the frustum can pass.
- The depth bias 2e-6 was not derived; it is 30 times the spacing of floats
  near 1 and the check passes with it. Not tried without.
- Order inside a draw list depends on thread timing. The picture does not
  (depth test, opaque), the lists' contents are checked as sets.
- Counts are not shown live: the panel shows the CPU's frustum count only,
  because reading a buffer back every frame is not something `vk.h` does
  cheaply.
- Lavapipe only. Run in a window once, under Xvfb, for one screenshot; the
  panel's controls were not operated.

## Running

```
./build/cm/gpu_driven                                             # window; drag to orbit
./build/cm/gpu_driven --headless out.ppm --size 640 360 --check
```

`--mode N` (0 frustum + occlusion, 1 frustum only, 2 no culling), `--view N`
(0 lit, 1 instance number, 2 red = added in phase B), `--overview`,
`--still`, `--frames N`.

## References

- Ulrich Haar, Sebastian Aaltonen, *GPU-Driven Rendering Pipelines*,
  SIGGRAPH 2015, Advances in Real-Time Rendering.
- Ned Greene, Michael Kass, Gavin Miller, *Hierarchical Z-Buffer Visibility*,
  SIGGRAPH 1993.
- Arseny Kapoulkine, *niagara*, https://github.com/zeux/niagara
