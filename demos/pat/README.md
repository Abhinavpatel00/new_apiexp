# pat — signed distance to a point cloud, every point a torus

```
Bake ─► Draw ─► Dots ─► Present
```

A point cloud with normals, drawn as a solid surface without ever making
a mesh. The method is *Points as Tori* (Feng, Gkioulekas, Crane, ACM TOG
2026): a small transformer looks at each point and its 64 neighbours and
answers with a torus that touches the surface there to second order; the
distance to the surface is a weighted average of the distances to the
nearest 32 tori. The network is the authors' own, with their trained
weights, run in plain C.

`--cloud` picks torus, rounded box, bumpy ball (2 048 points each,
sampled from a known surface), tooth (2 048) or snail (4 096, the
default; both scans come with the authors' code). `--blend 1` replaces
the tori by tangent planes, the method the paper improves on; `--blend 2`
uses the nearest torus alone. `--view 1` is a slice of the distance,
`--view 2` the normals. `--offset 0.1` draws the surface 0.1 further
out. `--combine 1|2|3 --other N` is union, intersection or difference
with a second cloud. `--dots` shows the input points.

The paper is explained at length in
[`docs/points-as-tori.md`](../../docs/points-as-tori.md).

## The method

1. **Fit** (`pat --fit`, done once; results are in `data/*.pat`). For
   each point: its 64 nearest, in a frame built on its normal and scaled
   by the median distance, go through 8 transformer blocks of width 128.
   Out come six coefficients of a height field
   `a00 + a10 s + a01 t + a11 st + a20 s² + a02 t²` over the tangent
   plane. Its two principal curvatures give a torus: the tube wraps the
   stronger one, the equator follows the weaker. 20 ms a point on one
   core.
2. **Distance.** At x, take the 32 nearest points; each gives the signed
   distance to its torus; average them with weights
   `exp(−64 · distance ÷ (half the farthest))`.
3. **Bake** writes that into a 64³ volume (compute, once per cloud).
4. **Draw** marches a ray through the volume until the surface is within
   a voxel, then by the points themselves until the distance is under
   10⁻⁴. The normal is the gradient of the blend (the paper's appendix D).

A torus is stored by its two curvatures and the point where it touches,
not by centre and radii: where the surface is nearly flat the radii are
in the millions and floats cannot subtract them. Points are sorted into
a 16³ grid so a query looks only at nearby cells.

## Acceptance (`--check`)

| Line | Rule | Result |
|---|---|---|
| **network in C against the authors' code, 4 points: largest difference in a coefficient** | < 2e-5 | 2.2e-6 |
| the same points against `data/torus.pat` | < 1e-4 | 0 |
| **torus on a polynomial bending one way: order of contact** | ≥ 2.9 | 3.02 / 3.05 |
| gradient at the point · the polynomial's normal | > 0.999999 | 1 |
| on a saddle (the construction as published) | reported | order 2.0, gradient · normal = −1 |
| curvature form of the torus distance − equation 23 | < 1e-5 | 1.4e-6 |
| radii of 5 million: equation 23 in float / the curvature form, against the plane | reported / < 1e-6 | 0.50 / 1.1e-7 |
| the walk that gives the bumpy ball's true distance, tried on the torus | < 1e-5 | 5e-8 |
| **mean \|distance − true\| on 32³, tori: torus / box / ball** | < 0.005 | 0.00055 / 0.00086 / 0.00034 |
| **tangent planes** | > tori | 0.0038 / 0.0064 / 0.0032 |
| nearest torus alone | reported | 0.00053 / 0.00093 / 0.00042 |
| grid points of 32 768 with the wrong sign, tori (planes) | ≤ 16 | 8 / 0 / 12 (19 / 5 / 10) |
| mean \| \|gradient\| − 1 \| | < 0.05 | 0.0011 / 0.0010 / 0.0009 |
| gradient of appendix D − central differences, near the surface | < 0.05 | 0.0004 / 0.0004 / 0.0003 |
| **GPU − CPU on a slice: distance, tori / planes / union / difference** | < 1e-4 | 9e-7 / 8e-7 / 9e-7 / 9e-7 |
| gradient component | < 2e-3 | 1.1e-4 / 8e-5 |
| **change of the distance over a step of 1e-5 ÷ step: blended** | < 2 | 1.31 |
| nearest torus alone | > 10 × blended | 389 |
| rays out of steps, level 0 / 0.1 | ≤ 230 | 11 / 32 |
| hits with more than 1e-4 of distance left | ≤ 1 % of hits | 25 of 36 274 / 0 of 51 210 |
| the most that is left | < 0.005 | 0.0020 / 1e-4 |
| **mean \|true distance − level\| where the rays stop** | < 0.003 | 0.00058 / 0.00050 |
| **pixels where volume-started and all-pointwise rays disagree about hitting** | ≤ 115 | 0 / 0 |
| largest difference of their depths | reported | 5e-4 / 9e-5 |

## Read this before trusting it

* The weights are the authors'. Training is not reproduced, and nothing
  here tests it.
* The reference coefficients in `--check` come from running the authors'
  JAX code once; they are constants in `main.c`.
* The network never chose a saddle on the test clouds, and the published
  construction does not touch a saddle to second order (line 5 of the
  table). Kept as published.
* The blended distance can change faster than 1 (1.31 measured), so it is
  not a safe bound; rays step 0.9 of it and come back when they overshoot.
* The scans show facets where neighbouring tori disagree, and the snail
  has a small pocket of wrong sign inside.
* The true distance for the bumpy ball is found by walking over the
  surface from the nearest input point; on a shape with two equally near
  parts it could settle on the farther one.
* Only the 32 nearest points are summed, as in the released code; the
  paper's fixed radius is not used. Morphology, normal re-orientation
  and Gaussian splats from the paper are not here.
* lavapipe: 0.4 s a frame at 640 × 360, 1 s to bake.

## Files

`method.h/.c` the network, the torus and the blend on the CPU ·
`main.c` clouds, fitting, passes, checks · `shaders/pat_common.slang`
the same torus and blend for the GPU · `bake.slang`, `draw.slang` ·
`data/weights.bin` the network (6 MB) · `data/*.ply` scans ·
`data/*.pat` fitted tori.
