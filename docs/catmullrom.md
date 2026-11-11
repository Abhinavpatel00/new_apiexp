# On the Parameterization of Catmull-Rom Curves — the paper and `demos/catmullrom`

Source. Cem Yuksel, Scott Schaefer, John Keyser, "On the Parameterization
of Catmull-Rom Curves", 2009 SIAM/ACM Joint Conference on Geometric and
Physical Modeling
(`http://www.cemyuksel.com/research/catmullrom_param/catmullrom.pdf`,
6 pages). Read: sections 1 to 5 and 6.1 to 6.4. Not read: the end of
section 6, the conclusion, and the extended journal version (Computer
Aided Design 43(7), 2011). Equation, theorem, section and figure numbers
are the paper's.

## 1. The curve (section 2)

A cubic Catmull-Rom curve passes through its control points `P_i`. Each
point has a parameter value `t_i`, and the piece between `P_1` and `P_2`
is a cubic polynomial of `t` in `t_1 … t_2` that depends on `P_0 … P_3`.
Barry and Goldman's construction (figure 3) is a pyramid of linear
interpolations:

    L01  = lerp(P0, P1) over t0 … t1      L12, L23 likewise
    L012 = lerp(L01, L12) over t0 … t2    L123 likewise over t1 … t3
    C12  = lerp(L012, L123) over t1 … t2

`cr_point` is this. The curve is C1: its tangent is continuous at the
control points, its curvature is not (section 6.4).

## 2. The family of parameterizations (eq. 1)

    t_i+1 = t_i + |P_i+1 − P_i|^α,     0 ≤ α ≤ 1

α = 0 is uniform (every edge takes one unit), α = 1 is chordal (an edge
takes its length), α = 1/2 is centripetal. If all edges have the same
length all α give the same curve; the differences show when lengths are
unequal (section 6).

## 3. Cusps and loops (section 3)

Write the piece as a cubic Bézier curve `B_0 … B_3` (eq. 2), with
`B_0 = P_1`, `B_3 = P_2`, and with `d_i = |P_i − P_i-1|`

    B_1 = (d1^2α P2 − d2^2α P0 + (2 d1^2α + 3 d1^α d2^α + d2^2α) P1) / (3 d1^α (d1^α + d2^α))

and `B_2` symmetric. `cr_bezier` is eq. 2.

A piece has no cusp and does not cross itself if, projected on the line
through its end points, it always moves forward.

**Theorem 1.** The piece starts in the direction `B_1 − B_0`. Its
projection on the edge is (eq. 3) a positive factor times

    d1^2α d2² − d2^(2α+1) d1 cos θ

with θ the angle at `P_1` between the edges. Taking cos θ = 1, this is
negative when `d1^2α d2 < d1 d2^2α`: for α < 1/2 when the edge before is
longer, for α > 1/2 when it is shorter. Only α = 1/2 never satisfies it.
So for every other α some control points make the piece start backward,
which leads to a cusp or loop.

**Theorem 2.** A cubic Bézier curve whose two inner points project
strictly between its end points moves forward all the way.

**Theorem 3.** For α = 1/2 the inner points do project there: the
position of `B_1` along the edge is (eq. 4, `r = d1/d2`)

    (r^α − r^(1−α) cos θ) / (3 (1 + r^α))

which is at least 0 by theorem 1 and cannot reach 1 for α ≥ 1/2.

Together: centripetal pieces have no cusps or loops, and no other α in
the family guarantees that.

## 4. Distance from the control polygon (section 4)

`B_1` is at distance (eq. 5)

    h1 = d2 r^(1−α) |sin θ| / (3 (1 + r^α))

from the edge's line, and a cubic Bézier curve is within 3/4 of its
control points' distance, so every point of the piece is within (eq. 6)

    h ≤ d2 r^(1−α) / (4 (1 + r^α))

For α < 1/2 this grows without limit as the neighbouring edge gets longer.
For α = 1/2 and α = 1 it is at most a quarter of the edge, for α = 2/3 an
eighth. For α = 1/2 the piece also stays between its two end points
(section 3), so a box around the edge holds it; for other α the piece can
pass an end point, by at most the distance of eq. 8 (up to a third of the
edge for α = 1), and circles round the control points are needed as well
(figure 5).

## 5. Curves that do not cross themselves (section 5)

With α = 1/2: pieces do not cross themselves; pieces that are not
neighbours cannot cross if their boxes do not overlap; and for
neighbours the paper bounds the angle γ by which a piece can lean inward
from its edge at the shared point: γ ≤ π/6.

## 6. The demo

A closed polygon, so every piece has four control points. The curves of
α = 0, 0.1, …, 1 are drawn as in the paper's figures 7 and 8 (green,
blue, red, the rest grey). `--bounds` draws for each edge the box of
eq. 6 with α = 1/2, using both neighbouring edges.

## 7. What the check measures

Numbers are in `demos/catmullrom/README.md`. The random control points
are four at a time: the middle edge of length 1, the outer edges with
lengths between e⁻⁴ and e⁴ and any direction, so that very unequal
neighbours are common.

1. Pyramid = Bézier form; C1 at the joints.
2. Eq. 3, 4, 5 against the Bézier points.
3. Theorem 1: pieces starting or ending backward, for six α.
4. Theorems 2, 3: for α = 1/2, where `B_1` projects and the smallest
   forward speed.
5. Loops (by intersecting the 64 lines of a sampled piece) and pieces
   leaving their strip.
6. Eq. 6 with the actual neighbours, and the α-only bounds 1/4, 1/8.
7. Eq. 8's 1/3 for α = 1.
8. Section 5's angle.

## 8. Agreement with the paper

All of the above holds as stated. Loops are rare for α above 1/2 (2 of
20000 at α = 2/3, 1 at 0.75, 10 at 1), more frequent below (223 at 0.25)
and most frequent for uniform (526), which matches section 6.2 ("As α increases, such features become
less likely to appear").

One difference from the text extracted from the PDF: in section 5,
tan γ is printed with `− √r cos θ` in the denominator. Dividing eq. 5 by
one minus eq. 4 (α = 1/2) gives `+ √r cos θ`, and the check confirms the
+ form against the Bézier points. The bound π/6 is the same for both
signs.

## 9. Not implemented

Three dimensions; the circles of eq. 8; section 5's rules for neighbours
and for distant pieces as a test on a whole curve; section 6.4's
observation that centripetal curves have their highest curvature at
control points; the journal version.
