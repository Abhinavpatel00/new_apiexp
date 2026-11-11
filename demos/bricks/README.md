# bricks — a distance volume stored sparsely

```
once:         Classify ─► Allocate ─► Bricks (indirect) [─► Dense, for comparison]
every frame:  Draw (sphere tracing) ─► Present
```

A 16 m world sampled at 253³ would take 124 MB as one volume. Most of it is
air or deep ground, where a ray only needs to know "nothing within so many
metres". Here the world is cut into bricks of 7 voxels a side; only the
bricks near a surface are stored, in an atlas, and a small volume with one
texel per brick says where — or, for the others, how far away everything is.

| | technique | source |
|---|---|---|
| layout | bricks of 8³ samples (7 cells, the border samples are shared with the neighbours so filtering inside a brick needs no neighbour) in a 3D atlas; an indirection volume with one texel per brick | sparse brick maps as used for distance fields, e.g. Claybook (Aaltonen, GDC 2018), GigaVoxels (Crassin et al. 2009) for the brick-with-border idea |
| which bricks | stored if the distance at the centre is within half a brick diagonal + 2 voxels; otherwise the texel holds that distance minus half a diagonal, which is a safe step from anywhere in the brick | follows from the distance function being 1-Lipschitz |
| allocation | one serial compute invocation walks the bricks and hands out slots; the count becomes the arguments of an indirect dispatch that fills only the stored bricks | — |
| drawing | sphere tracing; in an empty brick one fetch, in a stored brick two (indirection, then the atlas with trilinear filtering) | Hart 1996 |

All cited from memory. The scene is an analytic function (ground, 25 shapes,
a ring), evaluated in the shaders; nothing is edited after the build.

## Acceptance (`--check --bricks 18`)

`--check` also builds the dense volume of the same samples and compares.
The table is for 127³ (the size `scripts/check_demos.sh` runs); the last
column repeats it at the default 253³.

| Line | Rule | 127³ | 253³ |
|---|---|---|---|
| bricks stored | 0 < share < 50 % | 1 166 of 5 832 (20.0 %) | 4 846 of 46 656 (10.4 %) |
| stored bricks in the indirection volume vs the allocator's count | equal | equal | equal |
| bricks that wanted a slot and got none | == 0 | 0 | 0 |
| slots out of range or given twice | == 0 | 0 | 0 |
| **every texel of every stored brick vs the dense volume (distance and normal), largest difference** | == 0 | 0 (596 992 texels) | 0 (2 481 152) |
| **every sample in every empty brick: the brick's distance exceeds the true one** | == 0 | 0 (2 388 992 samples) | 0 (21 406 720) |
| … has the wrong sign | == 0 | 0 | 0 |
| smallest distance an empty brick holds | ≥ 2 voxels | 2.04 | 2.00 |
| overview, 640×360: rays that hit in one volume and miss in the other | < 0.1 % of the picture (230) | 30 | 21 |
| rays whose hit distance differs by more than a voxel | < 0.5 % | 14 | 10 |
| hit distance, mean difference | < 0.2 voxels | 0.087 | 0.12 |
| colour, mean difference | < 0.002 | 0.0006 | 0.0004 |
| camera inside the field: hit in one, miss in the other | < 0.1 % | 15 | 30 |
| hit distance differs by more than a voxel | < 0.5 % | 21 | 96 |
| hit distance, mean difference | < 0.2 voxels | 0.014 | 0.11 |
| colour, mean difference | < 0.002 | 0.0003 | 0.0007 |

Reported, not judged:

| | 127³ sparse | 127³ dense | 253³ sparse | 253³ dense |
|---|---|---|---|---|
| overview: steps per ray | 4.37 | 3.21 | 4.54 | 3.39 |
| overview: texture fetches per ray | 6.52 | 3.47 | 6.36 | 3.65 |
| inside the field: steps per ray | 11.0 | 10.2 | 14.9 | 11.3 |
| inside the field: fetches per ray | 21.9 | 11.0 | 23.9 | 12.1 |
| largest colour difference on a pixel | 0.43 / 0.31 | | 0.35 / 0.48 | |
| largest hit-distance difference | 54 / 15 voxels | | 100 / 115 voxels | |

Memory (RGBA16F, 8 bytes a sample; "used" counts stored bricks and the
indirection volume, not the unused rest of the atlas):

| samples | bricks stored | sparse, used | dense | atlas allocated |
|---|---|---|---|---|
| 64³ (`--bricks 9`) | 255 of 729 (35 %) | 1.0 MB | 2.0 MB | 31.2 MB |
| 127³ (`--bricks 18`) | 1 166 of 5 832 (20 %) | 4.6 MB | 15.6 MB | 31.2 MB |
| 253³ (default) | 4 846 of 46 656 (10.4 %) | 19.3 MB | 123.6 MB | 31.2 MB |
| 505³ (`--bricks 72 --atlas 30`) | 24 007 of 373 248 (6.4 %) | 96.6 MB | 982.6 MB (never built) | 105.5 MB |

## What the numbers say

- The sparse volume is the dense one with parts left out, not an
  approximation of it: where a brick is stored every texel is identical, and
  where it is not, the one number it keeps never promises more room than
  there is. That is the whole correctness argument for the tracing, and both
  halves are tested on every sample.
- The memory saved grows with resolution because surfaces are
  two-dimensional: doubling the resolution multiplies the dense volume by 8
  and the stored bricks by about 4 (1 166 → 4 846 → 24 007). At 64³ the
  saving is a factor of 2, at 505³ a factor of 10.
- It is not free. A ray takes a third more steps (an empty brick offers one
  cautious distance for all of itself) and nearly twice the fetches (every
  step in a stored brick reads the indirection volume first), and a frame
  takes longer — see Time.
- The pictures agree on all but a few dozen rays. Those sit on silhouettes:
  the two tracers arrive at different sample points, and the test "closer
  than a quarter voxel" succeeds for one and not the other, after which that
  ray continues to whatever lies behind — hence differences of a hundred
  voxels on single pixels. This is my reading of where they are in the
  picture, not something the check establishes.
- Two things the check found while this was written, both fixed:
  the scene function at first looked only at the shapes near a point and so
  jumped where a far pillar came into range; 241 samples in empty bricks
  were then closer to a surface than their brick claimed. And where the box
  cuts through the ground, deep bricks are empty and have no normal, so the
  cut face was shaded from garbage at 253³ (not at 127³, where those bricks
  happen to be stored); the cut is now shaded as the face of the box in both
  modes.

## Time

lavapipe (CPU), 1280×720, difference of two runs of different length, two
measurements each:

| | sparse | dense |
|---|---|---|
| 127³ | 51–54 ms | 34–36 ms |
| 253³ | 53 ms | 42–48 ms |
| 505³ | 55–61 ms | — |

A whole run of two 64×36 frames including start-up and the build takes
0.29 s at 253³ sparse, 1.4 s with the dense volume as well, and 1.06 s at
505³ sparse. On a CPU rasteriser a texture fetch costs the same wherever it
lands; a GPU's caches may shift the comparison either way, and that is not
measured here.

## Not done, not tested

- **Static.** The volume is built once. Editing would need bricks to be
  allocated and freed as the surface moves; the allocator here only ever
  counts up. `demos/sdfworld` and `demos/clay` still use dense volumes.
- **The atlas does not grow.** If more bricks are needed than fit, the rest
  are left out, the program says so (`ATLAS FULL: … Raise --atlas`) and the
  picture has holes. 505³ needs `--atlas 29` or more; the default 20 holds
  8 000 bricks.
- **One level.** No hierarchy above the indirection volume, no coarser
  bricks for distant parts, no mip chain; at 505³ the indirection volume
  itself is 2.9 MB.
- **RGBA16F with the normal stored beside the distance** — four times what
  the distance alone needs. An R16F atlas with normals from differences at
  the hit would use a quarter of the brick memory; the framework's storage
  volumes are RGBA16F only, so that was not tried.
- **Normals across brick borders** are taken from the analytic function at
  build time, so they are right at the border. A volume built from other
  data would have to fetch neighbours for them.
- The allocator is one compute invocation looping over all bricks — fine for
  373 248, not a pattern for much more.
- Empty bricks keep the distance at their centre minus half a diagonal; near
  the stored band that is as little as 2 voxels, which is where the extra
  steps come from. A margin other than 2 voxels was not tried.
- lavapipe only; the window was opened under Xvfb, the panel not operated.

## Running

```sh
./build/cm/bricks                        # 253³, sparse
./build/cm/bricks --dense                # the same samples as one volume (124 MB), switch in the panel
./build/cm/bricks --view 3               # one colour per brick
./build/cm/bricks --bricks 72 --atlas 30 # 505³
./build/cm/bricks --headless out.ppm --size 640 360 --bricks 18 --check
```

`--bricks B` (4–72; samples a side = 7·B + 1), `--atlas A` (atlas side in
bricks, room for A³), `--dense`, `--view N` (0 lit, 1 steps per ray,
2 normal, 3 bricks).

## References

- J. C. Hart, *Sphere Tracing*, 1996.
- C. Crassin et al., *GigaVoxels*, I3D 2009 — bricks with a border behind a
  pointer structure.
- S. Aaltonen, *GPU-based clay simulation and ray-tracing tech in Claybook*,
  GDC 2018 — a sparse distance-field brick layout in a shipped game.
