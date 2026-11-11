# c2spline — curves through control points, C2 everywhere, each piece from four points

```
(method.c on the CPU: one curve through every three points, blended in pairs) ─► Draw ─► Present
```

Cem Yuksel: *A Class of C2 Interpolating Splines* (ACM Transactions on
Graphics 39(5), 2020).

The same closed set of control points with the paper's four
interpolation functions: top left Bézier, top right circular, bottom left
elliptical, bottom right hybrid. Red is where the curve comes from a
circle, blue from an ellipse, green from a parabola. The purple lines
show the curvature.

| option | |
|---|---|
| `--type 0..4` | 0 Bézier, 1 circular, 2 elliptical, 3 hybrid, 4 all four (default) |
| `--scene 0..2` | 0 a free shape whose points wander, 1 points on a circle (one slides along it, one leaves it), 2 a star whose tips are pairs of points that close up |
| `--comb L` | length of the curvature lines |
| `--still` | the control points do not move |

## The method

Through every control point and its two neighbours goes a simple curve,
the interpolation function: a parabola, a circle or an ellipse. Between
two control points there are two such curves (one centred on each end).
The spline is their blend with weights cos² θ and sin² θ (eq. 2). Those
weights are flat at both ends, so at a control point the blend has the
position, the first and the second derivative of one curve alone, and the
pieces join with continuous curvature whatever the interpolation function
is. No system of equations is solved, and a control point moves only four
pieces. `docs/c2spline.md` goes through it.

## What `--check` measures

75 assertions, under 1 s.

* Eq. 1: every function passes through its three points (to 1e-12 for
  5000 random triples each). The parabola's largest curvature is at the
  middle point (eq. 7). The ellipses have the points where section 4.3
  puts them.
* Continuity at the control points, by one-sided differences, 200 random
  closed curves for each function: jump of the first derivative at most
  1.5e-8, of the second derivative 7e-7, of the curvature 1e-6 (relative;
  this is the accuracy of the differences). With weights that are
  straight lines instead of cos² and sin², the first derivative is still
  continuous and the second jumps by 1.8 to 2.0 times its size.
* 8000 random segments (points anywhere in a square):

  | | Bézier | circular | elliptical | hybrid |
  |---|---|---|---|---|
  | largest distance from the chord ÷ chord | 0.124992 | 663 | 0.207008 | 0.206893 |
  | the paper's bound | 1/8 | none | 0.2071 | (0.2071) |
  | segments not between their two points | 0 | 2987 | 0 | 0 |
  | segments that cross themselves | 0 | 25 | 0 | 0 |
  | largest curvature in the middle half | 0 | 1207 | 203 | 0 |

* Points on a circle: the circular and the hybrid function give the
  circle to 3e-16; Bézier is off by 0.116 of the radius with 4 even
  points and 0.0085 with 8; elliptical gives the circle with 4 even
  points and is off by 0.036 with 8.
* Where the hybrid switches (an arc of a quarter turn) the ellipse and the
  circle are the same curve (1e-15).
* Moving one control point changes four segments, and those are the four
  around it.
* Four points in a line give a straight piece; turning, scaling and
  shifting the control points does the same to the curve.

## What this says

Every property that the paper states and the check can test holds: the
bounds 1/8 and (√2 − 1)/2 are met and nearly reached, no Bézier,
elliptical or hybrid segment crosses itself or leaves the strip between
its two points, and the circular function does both. The last row of the
table is the reason for the hybrid: with the elliptical function some
segments have their sharpest bend in the middle, with the hybrid none
did.

## Not done

Control points in three dimensions (the method is the same; this file is
written for the plane), open curves (the paper does not say how the first
and last segment are made; all curves here are closed), dragging points
with the mouse, the comparisons with other splines (the paper's table 1
and figures 12, 13).
