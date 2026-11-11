# Seiler's interpolation — the talk and `demos/seiler`

Cem Yuksel: *Seiler's Interpolation for Evaluating Polynomial Curves*,
SIGGRAPH Talks 2024, 2 pages.
<http://www.cemyuksel.com/research/seilers_interpolation/>

Sources: the project page and the two-page talk abstract were read in
full. The method first appeared in Daqi Lin, Larry Seiler, Cem Yuksel,
*Hardware Adaptive High-Order Interpolation for Real-Time Graphics* (HPG
2021), which was **not** read; what is said about hardware below is what
the talk says. Equation numbers are the talk's.

## 1. The question

A cubic Bézier curve has four control points b0 … b3 and is

```
C(t) = (1−t)³ b0 + 3(1−t)² t b1 + 3(1−t) t² b2 + t³ b3            (eq. 1)
```

There are three usual ways to work this out:

* **Bernstein form**: the formula as written.
* **Polynomial form**: precompute p0 … p3 and evaluate
  ((p3 t + p2) t + p1) t + p0 (eq. 2). Fewest operations.
* **de Casteljau**: lerp neighbouring control points with weight t, three
  times over: 3 + 2 + 1 = 6 lerps, where lerp(a, b, t) = (1−t) a + t b.

Lerps matter because graphics hardware has them for free: a linear
texture filter *is* a lerp of neighbouring texels. If the control points
are in a texture, the first row of de Casteljau's lerps can be done by
the sampler. But three more remain, and each fetch gives only one lerp.

## 2. The construction

Look at the difference between the curve and the straight line from b0
to b3. It is zero at both ends, so it contains the factor (1 − t) t.
Dividing a cubic by that quadratic leaves something linear in t, and a
linear function is a lerp of two fixed vectors. So

```
C(t) = lerp(b0, b3, t) + (1 − t) t · lerp(d1, d2, t)
d1 = 3 (b1 − b0) − (b3 − b0)                                       (eq. 3)
d2 = 3 (b2 − b3) − (b0 − b3)                                       (eq. 4)
```

d1 and d2 are the **difference terms**. To see that these are the right
ones, compare slopes at t = 0: the curve leaves b0 with slope 3(b1 − b0),
the straight line with slope (b3 − b0), and (1 − t) t · d has slope d at
t = 0. So d1 must be the difference of the two slopes, and the same at
the other end gives d2.

Adding the end points gives the **Seiler points**

```
s1 = b0 + d1 = 3 b1 − b0 − b3            s2 = b3 + d2 = 3 b2 − b3 − b0
```

and the whole evaluation becomes three lerps, the *pure lerp* form:

```
b03 = lerp(b0, b3, t)
s12 = lerp(s1, s2, t)
C   = lerp(b03, s12, (1 − t) t)
```

In the demo's picture b03 is the blue dot sliding along the blue chord,
s12 the red dot sliding along the red line between the Seiler points,
and the curve point is the green dot on the segment between them, a
fraction (1 − t) t of the way, which is at most a quarter. The Seiler
points sit far outside the control polygon, about three times as far
from the chord as b1 and b2.

A third ordering, with **offsets**, saves a few multiplications:

```
e1 = b0 + (1 − t) t d1        e2 = b3 + (1 − t) t d2        C = lerp(e1, e2, t)
```

## 3. Why it helps

* The first two lerps have the same weight t and work on separate pairs
  (b0, b3) and (s1, s2). Stored as neighbouring texels of two textures,
  or two channels, a linear filter does both. One lerp is left for the
  shader, against three after de Casteljau's first row.
* The two can also run in parallel on a CPU; de Casteljau's levels
  depend on each other.

The talk's table 1 counts multiplications M and additions A for n
dimensions:

| | polynomial | Bernstein | de Casteljau | Seiler, differences | Seiler, lerps | Seiler, offsets |
|---|---|---|---|---|---|---|
| all | n(3M+3A) | n(4M+3A)+6M | n(12M+6A) | n(5M+3A)+M | n(6M+3A)+M | n(4M+3A)+M |
| after the first lerps | – | – | n(6M+3A) | n(M+A)+M | n(2M+A)+M | n(2M+A) |

So on a CPU the polynomial form is still the cheapest, and Seiler's wins
once the first lerps come from the hardware. The demo's timing agrees
with the first row: about 2.7 ns for the polynomial, 3.5 for Seiler's
lerps, 4.9 for de Casteljau, 10 for Bernstein as written (one machine,
one run; treat as rough).

## 4. Storage and joins

A piecewise cubic of m pieces needs 3m + 1 Bézier points, since pieces
share end points. Seiler points are **not** shared across a join, even a
smooth one: split one cubic in two and the Seiler points either side of
the split differ (0.45 units in the demo's check, for a curve 3 units
wide).

The exception is the **uniform Catmull-Rom spline**. Its piece from knot
P_j to P_j+1 has b1 = P_j + (P_j+1 − P_j−1)/6, which gives

```
s1 = 2 P_j − (P_j−1 + P_j+1) / 2
```

and the piece before it gets the same point as its s2. So there is one
Seiler point per knot, 2m + 2 points in all, and the whole spline is

```
C(x) = lerp( lerp(P_j, P_j+1, t),  lerp(S_j, S_j+1, t),  (1 − t) t )
```

Put the knots in one texture and the Seiler points in another: two
linearly filtered fetches at the same coordinate and one lerp give
cubic interpolation of the texture. This is `seiler_spline`, and scene 1
of the demo. (With non-uniform parameters the directions of the
difference terms still match at a join, not their sizes.)

## 5. Other degrees

A quadratic has one difference term d1 = 2(b1 − b0) − (b2 − b0) and two
Seiler points b0 + d1, b2 + d1.

For degree d the form is recursive (eq. 5 and 6):

```
C(t)   = lerp(b0, b_d, t) + (1 − t) t D_1(t)
D_i(t) = 0                                              if 2i = d + 1
       = d_i                                            if 2i = d
       = lerp(d_i, d_(d−i), t) + (1 − t) t D_(i+1)(t)   otherwise
```

Each level peels off the two ends and a factor (1 − t) t. The difference
terms up to degree 5 are (eq. 7 to 10)

```
d_1     = d (b_1 − b_0) − (b_d − b_0)
d_(d−1) = d (b_(d−1) − b_d) − (b_0 − b_d)
d_2     = C(d,2) (b_2 − b_1) − C(d−2,2) (b_1 − b_0) − (d−3)(b_(d−1) − b_d) − 3 (b_(d−1) − b_1)
d_(d−2) = the same with every index i replaced by d − i
```

Seiler points chain: s_i = s_(i−1) + d_i from the left, s_(d−i) =
s_(d−i+1) + d_(d−i) from the right, starting at the end points. A curve
of degree d takes d lerps, ⌈d/2⌉ of them independent. The talk leaves
degree 6 and above open.

`seiler_differences`, `seiler_points`, `seiler_from_differences`,
`seiler_from_points` and `seiler_from_offsets` in
`demos/seiler/method.c` are these formulas for degrees 2 to 5. For an
even degree the code keeps two copies of the middle Seiler point, one
reached from each side, and so uses one lerp more than the talk's count
there.

## 6. What the check measures

For each degree 2 to 5, 500 random curves at 21 parameters: all three
Seiler forms and de Casteljau agree with the Bernstein form to 6e-16.
This is also the test that the formulas for degree 4 and 5 were copied
correctly. The Catmull-Rom statement holds to 2e-16 and the 2m + 2
storage reproduces the Bézier pieces to 1.3e-15.

## 7. What was not implemented

* The hardware side: no texture filter is used, and the modified filter
  of the HPG 2021 paper, with bicubic and tricubic filtering of textures,
  is not implemented.
* Curves of degree 6 and above.
