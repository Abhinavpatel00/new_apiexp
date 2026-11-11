# A Class of C2 Interpolating Splines — the paper and `demos/c2spline`

Source. Cem Yuksel, "A Class of C2 Interpolating Splines", ACM
Transactions on Graphics 39(5), article 160, 2020
(`http://www.cemyuksel.com/research/interpolating_splines/a_class_of_c2_interpolating_splines.pdf`).
Read: the abstract, sections 3 and 4 in full, 5.1, the start of 5.2 and
5.3, 5.4, 5.5. Not read: sections 1 and 2 (introduction, earlier work),
table 1 beyond its first row, the conclusion, the appendix with the
proofs. Equation, section, figure and theorem numbers are the paper's.

## 1. The problem

A designer places points and wants a smooth curve through them. As
background (this paragraph is not from the paper, whose sections on
earlier work were not read): the cubic Catmull-Rom spline that most
software offers passes through the points and is local, but only its
tangent is continuous; cubic splines with continuous curvature through
the points need a linear system over all points, and then every point
affects the whole curve; B-splines are smooth and local but do not pass
through their control points.

The paper gives curves that pass through the points, have a continuous
second derivative (C2), and are local: a piece between two points depends
on four points (the abstract: "only four control points define each curve
segment"). No system is solved.

## 2. The formulation (section 3)

For every control point `p_i` choose an interpolation function `F_i`: any
curve through `p_i-1`, `p_i` and `p_i+1`, parametrized so that (eq. 1)

    F_i(0) = p_i-1,   F_i(π/2) = p_i,   F_i(π) = p_i+1

Between `p_i` and `p_i+1` two of these curves are available: the second
half of `F_i` and the first half of `F_i+1`. The spline segment is their
blend (eq. 2):

    C_i(θ) = cos²θ · F_i(θ + π/2) + sin²θ · F_i+1(θ),      θ in 0 … π/2

Why this is C2. Differentiate (eq. 3):

    C_i' = 2 cosθ sinθ (F_i+1 − F_i) + cos²θ F_i' + sin²θ F_i+1'

At θ = 0 the first term is zero (sin θ = 0) and the rest is `F_i'`. So
the tangent at `p_i` is that of `F_i`, from both sides. The second
derivative (eq. 4) has a term `2(cos²θ − sin²θ)(F_i+1 − F_i)`, which is
zero at the ends because both functions pass through the control point,
a term with `cosθ sinθ`, zero again, and what remains is `F_i''`. So the
second derivative at `p_i` is that of `F_i`, from both sides.

The third derivative (eq. 5) keeps a term with `F_i+1' − F_i'`, which
does not vanish, so the curves are not C3.

The trigonometric weights matter. With weights `1 − w` and `w` linear in
θ, the first derivative is still continuous but the second derivative has
the term `2 w' (F_i+1' − F_i')`, which is not zero. The check measures
this.

`c2_segment` is eq. 2.

## 3. Four interpolation functions (section 4)

**Bézier (4.1).** A quadratic Bézier curve with end points `p_i-1`,
`p_i+1` and the middle control point set so that the curve passes through
`p_i` at a parameter `t_i` (eq. 6). Any `t_i` works. The paper takes the
one that puts the parabola's largest curvature at `p_i`: the root in
0 … 1 of a cubic (eq. 7, from Yan et al.'s κ-curves). With it, the paper
proves that a segment stays within 1/8 of its chord length from the chord
(theorem A.1), stays between the two lines through its end points
perpendicular to the chord (A.2), and does not cross itself (A.3). It
cannot make a circle.

**Circular (4.2).** The circle through the three points. Four points on a
circle give an exact arc. But the curve can go far from the control
polygon and can cross itself (figure 5).

**Elliptical (4.3).** An ellipse with `p_i` at the end of one axis (the
"primary" one), the farther of the two neighbours at the end of the other
axis, and the nearer neighbour on the other side of `p_i`, between two
ends of axes. That fixes the ellipse. Its centre is on the circle that
has `p_i` and the farther neighbour across a diameter (the axes meet at a
right angle at the centre), and the paper finds it by bisection; so does
`c2_function`, on the angle between the primary axis and the line to the
farther neighbour. The paper proves a distance bound of (√2 − 1)/2 ≈ 0.207
of the chord (theorem B.1), the same strip property (B.3) and no
self-crossing (B.4). When the primary axis is the shorter one, the
ellipse's largest curvature is not at `p_i`, and segments can bend most
in their middle (figure 8f).

**Hybrid (4.4).** Different control points may use different functions.
The hybrid takes the circle when both of its arcs (to the previous and to
the next point) are at most a quarter turn, and the ellipse otherwise. At
exactly a quarter turn the ellipse is the circle, so the curve changes
continuously when points are moved. This keeps the elliptical function's
guarantees, makes circles easy, and avoids the bends in the middle.

## 4. Parameters

Each function has its own parameter `t` in 0 … 1, with `p_i` at `t_i`.
θ maps to `t` linearly on each side of `p_i`: 0 … π/2 to 0 … `t_i`, and
π/2 … π to `t_i` … 1. The two sides have different speeds, so in θ the
derivative has a jump in size at `p_i`. The paper's global parameter `s`
(eq. 8, 9) removes it: `s` advances by `α_i t_i` before `p_i` and
`α_i (1 − t_i)` after, so that `F_i` is linear in `s` over its whole
length. The check scales its θ-derivatives by `1/t_i` and `1/(1 − t_i)`
accordingly, and also compares the curvature, which does not depend on
the parameter.

For arcs the paper writes `F(t) = cos(α t + δ) u + sin(α t + δ) v + q`
(eq. 10) so that the curve can be transformed by any affine map through
`q`, `u`, `v`. The demo stores the same `q`, `u`, `v` with `u = p_i − q`
and measures the angle from `p_i`. (As printed, the text takes
`v_i = p_i − q_i` and `t_i = −δ_i/α_i`, which would put `p_i` at `q + u`;
measuring from `p_i` avoids choosing between the two.)

## 5. The demo

`method.c` is the four functions and eq. 2 for points in the plane.
Three points in a line get the parabola that is the straight line through
them. The picture draws the control polygon, the curve coloured by which
kind of function it comes from (the paper colours circular red and
elliptical blue in its hybrid figures), and the curvature as lines across
the curve, as the paper's figures do.

## 6. What the check measures

Numbers are in `demos/c2spline/README.md`.

1. Eq. 1 for all four; eq. 7's root is a curvature maximum; the ellipse
   has the points where section 4.3 says.
2. Continuity of first and second derivative and curvature at control
   points, by differences that do not use the derivation above; and the
   failure of linear weights. The curves for this test have their points
   around a centre: for points anywhere in a square, nearly straight
   triples produce very large circles (a segment went 663 chord lengths
   from its chord), and differences lose the second derivative in
   rounding.
3. Theorems A.1, B.1 (distance), A.2, B.3 (strip), A.3, B.4 (no
   crossing) on 8000 random segments; that the circular function breaks
   all three.
4. Figure 8: which functions reproduce a circle.
5. The hybrid's switch.
6. Local support; straight lines; similarity transforms.

## 7. Agreement with the paper

* The two distance bounds hold and are nearly reached (0.124992 against
  1/8; 0.207008 against 0.2071).
* No Bézier, elliptical or hybrid segment left its strip or crossed
  itself; 2987 and 25 of 8000 circular ones did.
* The paper reports from experiments that Bézier and hybrid curves have
  their curvature maxima near control points, and that elliptical ones
  can have peaks away from them. Measured here as "the largest curvature
  of the segment is in its middle half": 0 Bézier, 0 hybrid, 203
  elliptical, 1207 circular, of 8000 each. This is a coarser statement
  than the paper's (the largest value, not every local maximum).
* The hybrid's bound of 0.2071 is this demo's reading of "inherits all
  benefits of the elliptical interpolation function"; the paper states
  the theorem for the elliptical function.

## 8. Not implemented

Three and more dimensions; open curves; the global parametrization as
code (only its scale factors, in the check); affine transformation of
stored arcs; sharp corners by coincident points (two points at the same
place; scene 2 brings them close, not together).
