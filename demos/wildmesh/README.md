# wildmesh — simplifying a mesh that is not one clean surface

```
Draw (3-D canvas; the method runs in C: method.c) ─► Present
```

Hsueh-Ti Derek Liu, Xiaoting Zhang, Cem Yuksel: *Simplifying Textured
Triangle Meshes in the Wild* (ACM Trans. Graph. 44(6), 2025).

Models made by hand are seldom one closed surface. Parts stand on each
other without sharing a vertex, sheets have borders, three faces meet in
one edge. Collapsing edges by the quadric error of Garland and Heckbert
goes wrong on them in two ways: a flat sheet costs nothing to remove, and
parts that only touch are never merged, so each one shrinks away by
itself. The paper changes three things: edges between parts that are near
each other ("virtual edges"), a cost for losing area at borders that is
worked out anew each time, and colours looked up through a map from the
result back to the input.

Three copies of one mesh, each turning in place:

| left | middle | right |
|---|---|---|
| the input; blue lines are the virtual edges | the plane quadric alone (`--baseline 0`) | the paper's method |

The number of faces steps down: 50, 25, 10, 5, 2 per cent.

* `--scene 0` (default) a small town, 5120 faces in 57 parts: a ground
  sheet, houses of six separate sheets, a dome and a ball that rest on
  roofs, lamps on thin poles, trees of three sheets through one line.
* `--scene 1` two blocks as a triangle soup: 1968 triangles, no two share
  a vertex.
* `--scene 2` a cloth 0.012 thick (4 wide), yellow above and blue below.
  Use `--baseline 2`: the middle copy is then the paper's mesh with every
  colour from the nearest point of the input, the right copy the same mesh
  coloured through the collapses (the paper's figure 17).
* `--scene 3` a closed surface, where the two methods give the same mesh.

| option | |
|---|---|
| `--keep K` | 0 steps down; 1 … 5: 50, 25, 10, 5, 2 per cent |
| `--baseline B` | middle copy: 0 plane quadric alone; 1 virtual edges between near vertices and the area term summed; 2 the right copy's mesh, colours from the nearest point |
| `--virtual V` | right copy's virtual edges: 0 none, 1 near vertices, 2 near triangles (the paper's) |
| `--area A` | right copy's area term: 0 none, 1 summed, 2 anew for every cost (the paper's) |
| `--reach R` | r of the virtual edges as a part of the mesh's size (default 0.005) |
| `--wire` | draw the edges of the simplified triangles |
| `--no-edges` | do not draw the virtual edges |
| `--still` | no turning, no stepping down |

## What `--check` measures

64 assertions, about 6 s. "Size" is the diagonal of the mesh's box.
Distances are measured from 4000 points spread over one mesh to the
nearest point of the other.

**The quadrics.** At 400 vertices of the town with random points x: the
stored plane quadric against the sum of area / 3 × distance² over the
vertex's faces (eq. 9), 3.1e-13; the area quadric against the sum over the
237 border edges met of 2 × (area of the triangle edge–x)² (eq. 15),
4.0e-14.

**A closed surface** (3584 faces to a tenth): no virtual edges, one part,
and the paper's method gives the same 358 faces at the same coordinates as
the plane quadric alone (difference 0).

**Virtual edges on the town** (r = 0.005 of the size): 57 parts. Joining
vertices nearer than 2 r leaves 9 parts (1786 edges); joining triangles
nearer than 2 r leaves 1 (2391 edges).

**The town simplified:**

| | to 512 faces | | to 102 faces | |
|---|---|---|---|---|
| | plane quadric | paper's | plane quadric | paper's |
| ground sheet still covered | 0 % | 100 % | 0 % | 100 % |
| largest distance input → result / size | 0.328 | 0.0179 | 0.326 | 0.0860 |
| largest distance result → input / size | 0.0016 | 0.0462 | 0.0067 | 0.0400 |
| mean squared distance, both ways / size² | 0.0173 | 9.9e-6 | 0.0183 | 2.1e-4 |
| faces left that were lamps | 202 | 121 | 23 | 0 |

The plane quadric alone removes the whole ground first (it is flat, so
every collapse in it costs 0) and spends the faces on the lamps. The
paper's method takes 0.4 – 0.5 s for this mesh, the search for near
triangles included.

**The soup** (1968 triangles, 15809 virtual edges, one part once joined):

| | to 196 | | to 39 | |
|---|---|---|---|---|
| | largest distance / size | mean squared / size² | largest | mean squared |
| no virtual edges, plane quadric | 0.328 | 0.0294 | 0.455 | 0.0583 |
| virtual edges, area term summed | 0.0504 | 1.7e-4 | 0.104 | 2.8e-3 |
| virtual edges, area term anew (the paper's) | 2.8e-8 | 1.3e-17 | 0.0106 | 2.3e-6 |

With the area term anew, 196 faces still give the two blocks exactly.
Summed, the term stays on edges that are no longer borders, which is the
paper's reason for not summing it (section 4.2, figure 16).

**The cloth** (6400 faces): 4000 points of the result are taken back to
the input. After that the mesh is the input again exactly (6400 faces,
coordinates differ by 0), and every point lies on the input face it names
(1.6e-16). Points that get their colour from the other side of the cloth:

| | to 640 faces | to 128 faces |
|---|---|---|
| nearest point of the whole input | 2.05 % | 15.7 % |
| back through the collapses (the paper's) | 2.03 % | 6.5 % |

The paper says its map "encourages (but does not guarantee)" the right
side. At a tenth of the faces the two ways are equal here; at a fiftieth
the paper's is 2.4 times better and still wrong for 6.5 % of the points.

## Differences from the paper

* The point a collapse moves to solves (A + l I) x = −b + l m with m the
  middle of the edge and l = 1e-7 of A's trace. The paper names the SVD or
  probabilistic quadrics where A is singular.
* r: no value was found in the text read; 0.005 of the size is the demo's.
* The "plane quadric alone" copy has no border term at all. It is not the
  paper's baseline (MeshLab's quadric simplification), which was not run.
  `--baseline 1` is nearer to the combination the paper calls G97+L98+T20,
  without the probabilistic quadric.
* Nothing stops a collapse that turns a face over.
* Near triangles are found by a sweep along x over the triangles' boxes.

## Not implemented

Textures: no UV map is read or baked, no mesh colours; the colour of a
piece of a result face (each face is cut into up to 64) is the input
face's colour at the mapped point. Probabilistic quadrics. Weights by
visibility (figure 20). The data sets and tables 1 and 2: the numbers
above are for the demo's own four meshes.

Explained in [`docs/wildmesh.md`](../../docs/wildmesh.md).
