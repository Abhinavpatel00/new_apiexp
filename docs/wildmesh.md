# Simplifying Textured Triangle Meshes in the Wild — the paper and `demos/wildmesh`

Source. Hsueh-Ti Derek Liu, Xiaoting Zhang, Cem Yuksel, "Simplifying
Textured Triangle Meshes in the Wild", ACM Trans. Graph. 44(6), December
2025, DOI 10.1145/3763277 (`https://arxiv.org/abs/2409.15458`, version 2).
Read: introduction, sections 3, 4, the start of 5 with tables 1 and 2, 6,
appendices A – D. Not read: related work (section 2), the middle of
section 5. Section, equation and figure numbers are the paper's.

## 1. The problem (sections 1, 3)

Edge-collapse simplification takes the edge that is cheapest to remove,
merges its two ends into one vertex, and repeats. With the quadric error
of Garland and Heckbert the cost is how far the new vertex is from the
planes of the faces that met at the two ends.

That works for a closed surface from a scanner or from marching cubes. The
paper looks at what people upload instead. Its figure 3 gives the share of
non-manifold meshes: 22.5 % in Thingi10k, 54.3 % in ModelNet, 98.9 % in
ShapeNet. On such meshes (section 3.1):

* The quadric error is a distance to planes. A part that can be flattened
  without stretching reports no error when edges in it collapse, so large
  parts are removed (figure 9).
* Adding the area term of Lindstrom and Turk keeps some large parts, but
  parts are still deleted "due to no edges to collapse between
  disconnected parts": each part shrinks by itself until it is gone.
* Garland and Heckbert proposed edges between vertices that are nearer
  than a threshold. The paper finds that this does not help much
  (figure 8 D), because the distance between vertices says little about
  the distance between parts.

For textures (section 3.2): keeping UV coordinates through the collapses
bleeds the background of the texture image onto the surface when there
are many UV islands, and keeping island borders exactly stops the
simplification early.

## 2. Virtual edges (section 4.1)

The mesh is M = (V, E, F). A physical edge belongs to at least one face. A
virtual edge belongs to none; it only says that its two ends may be
merged.

Section 4.1.1 builds them after the Čech complex, with the r-offset
surface of a triangle in the place of a ball round a point:

> If the distance between two triangles is smaller than 2r and the two
> triangles are not from the same connected component, we form a virtual
> edge between the two triangles by connecting their closest vertex pairs

The distance is between triangles, so a ball that rests in the middle of
a large triangle is found even though it is far from that triangle's
corners. Figure 15 shows a lamp: by vertex distance 4 parts remain, by
triangle distance 1.

## 3. The cost (section 4.2, appendix A)

A quadric is Q = (A, b, c) and its value at x is xᵀA x + 2 bᵀx + c
(eq. 1 – 6). For a plane through p with unit normal n: A = n nᵀ,
b = −A p, c = pᵀA p, which makes the value the squared distance of x from
the plane.

*Plane term.* A vertex takes from each of its faces the face's plane
quadric times a third of the face's area (eq. 9 – 11). The quadric of an
edge is the sum of its two ends' (eq. 12, 13). After a collapse the new
vertex keeps that sum. The paper calls this "memory".

*Area term* (A.3, eq. 15 – 17), after Lindstrom and Turk. For a border
edge from v_a to v_b near the collapsing edge, with s = v_b − v_a and
t = v_a × v_b, the quadric is ([s]×ᵀ[s]×, −[s]× t, tᵀt) / 2. Its value at
x is |s × x + t|² / 2, and |s × x + t| is twice the area of the triangle
v_a v_b x: the term charges for the area that the border sweeps when the
vertex moves to x.

*The change.* The paper adds the plane term up through the collapses but
works the area term out from the mesh as it is at the moment, "memoryless".
Its reason: collapsing a virtual edge can glue two border edges into one
inside edge. A summed area term would still be attached to that edge, and
for an inside edge the area gained on one side and lost on the other
should cancel, while the quadric squares and adds both. The cost of long
inside edges becomes too high and the result too even (figure 16, a
triangle soup).

Appendix B: "we only augment the area quadric if the local edge one-ring
contains boundary edges". A closed surface has none, so there the cost is
the plane term alone, the same as Garland and Heckbert's.

The new vertex is where the quadric is least, A x = −b (eq. 14).

## 4. Colour without UV coordinates (section 4.3)

The paper does not carry UV coordinates along. It builds a map T from the
simplified mesh M_c back to the input M_0 and bakes a new texture through
it (figure 18).

T is made of one step for each collapse, taken backwards (4.3.1). The
history {M_0, …, M_c} is kept. For a point on M_c that lies on a face
round the vertex of the last collapse, the collapse is taken back (a
vertex split) and the point is projected onto the nearest of the faces
round the edge as they were before. Then the same for the collapse before
that, until M_0. Two details:

* Only the faces of that one collapse are searched. So a point tends to
  stay on the part of the surface it came from; the paper says this
  "encourages (but does not guarantee)" it. Figure 17 is a thin cloth with
  two colours: the nearest point of the whole input often lies on the
  wrong side.
* "we reuse point locations on M_c for closest point queries": the point
  itself is not moved from step to step. The steps only decide which
  faces are searched next.

Appendix C.2: for each collapse the faces round the edge before and round
the vertex after are stored, and points on other faces are skipped.

## 5. What the paper reports (section 5)

Table 1, Thingi10K reduced to 0.1 % / 1 % / 10 % of the faces, boxes of
diagonal 1. Hausdorff distance × 10⁻¹: Garland and Heckbert 1.05 / 0.57 /
0.13, Trettner and Kobbelt 1.05 / 0.58 / 0.14, the paper's 0.84 / 0.44 /
0.10. Mean squared Chamfer distance × 10⁻¹: 2.36 / 1.37 / 0.46, 2.36 /
1.36 / 0.39, 2.06 / 1.11 / 0.29.

Table 2, colour difference at 1 % on two sets of textured meshes: 0.18 and
0.11 for Garland and Heckbert, 0.10 and 0.09 for the paper's.

Section 6 names what is left open: the map can take colour from layers
inside a model; many small parts (a conifer's needles) are still deleted;
the result can be as non-manifold as the input.

## 6. The demo

`demos/wildmesh/method.c` holds the mesh as lists (C.1): for each vertex
its faces and its edges. `wm_open` finds the connected parts, adds virtual
edges and the vertex quadrics, and puts every edge in a queue by cost.
`wm_collapse` takes the cheapest, moves one end's lists to the other,
drops faces that became degenerate or double, and records the faces round
the edge as they were. `wm_transfer` is the map of section 4.3.1.

`main.c` builds four meshes of its own and shows the input, a baseline and
the paper's method side by side on the 3-D canvas. The colours of a result
come through `wm_transfer`: each result face is cut into up to 64 small
triangles and the middle of each is mapped back.

## 7. What the check measures

See `demos/wildmesh/README.md` for the numbers.

1. The stored quadrics against the definitions (eq. 9, eq. 15).
2. A closed surface: the paper's method and the plane quadric alone give
   the same mesh, coordinate for coordinate (the "converges to the
   original quadric error metric for watertight manifold meshes" of the
   introduction).
3. Virtual edges by vertex distance against triangle distance: parts left
   (figure 15's comparison, on the demo's town: 9 against 1).
4. The town simplified to a tenth and a fiftieth: how much of the ground
   sheet is still covered, the distances between input and result.
5. The soup: area term summed against anew (figure 16's comparison).
6. The cloth: wrong-side colours by nearest point against the map
   (figure 17's comparison), and that taking all collapses back gives the
   input again.

## 8. Agreement with the paper, and where the demo's numbers say less

* On the closed surface the two methods are identical, as stated.
* On the town the paper's method keeps the ground and gets every input
  point within 0.018 of the size at a tenth of the faces; the plane
  quadric alone leaves input points 0.33 of the size away. The other
  direction is worse for the paper's method (0.046 against 0.0016): its
  result has points that far from any input face. Where they are was not
  looked into.
* On the soup the memoryless area term is much better than the summed one
  at both levels.
* On the cloth the map is no better than the nearest point at a tenth of
  the faces (2.03 % against 2.05 %) and 2.4 times better at a fiftieth
  (6.5 % against 15.7 %).
* Tables 1 and 2 are not reproduced: they need the data sets.

## 9. The demo's own choices

* The new vertex solves (A + l I) x = −b + l m, m the middle of the edge,
  l = 1e-7 of A's trace + 1e-30. The paper names the SVD or probabilistic
  quadrics for a singular A.
* Eq. 15 is used with the factor 1 / 2 as printed.
* r = 0.005 of the diagonal of the mesh's box. No value was found in the
  text read.
* A virtual edge is made for every pair of corners of the two triangles
  that is as near as the nearest pair.
* The middle copy's "plane quadric alone" has no border term at all. The
  paper's baseline is MeshLab's quadric simplification, which was not run.
* No test for faces that turn over, none of the optional validity checks
  of appendix B.

## 10. Not implemented

Reading or baking textures, UV maps, mesh colours; probabilistic quadrics
(Trettner and Kobbelt); visibility weights (figure 20); the user studies;
the data sets.
