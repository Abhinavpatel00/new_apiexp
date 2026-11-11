# catmullrom — why the centripetal Catmull-Rom curve is the one without loops

```
(method.c on the CPU: the pyramid of linear interpolations for each piece) ─► Draw ─► Present
```

Yuksel, Schaefer, Keyser: *On the Parameterization of Catmull-Rom Curves*
(2009 SIAM/ACM Joint Conference on Geometric and Physical Modeling).

One closed control polygon and its cubic Catmull-Rom curve for different
parameterizations, in the colours of the paper's figures: green uniform
(α = 0), blue centripetal (α = 1/2), red chordal (α = 1), grey the values
between in steps of 0.1.

| option | |
|---|---|
| `--scene 0..1` | 0 a shape with long and short edges whose points wander, 1 a star whose tips are two points that close up |
| `--alpha A` | draw this one parameterization only |
| `--no-fan` | without the grey curves |
| `--bounds` | the boxes of eq. 6 that hold each piece of the centripetal curve |
| `--still` | the control points do not move |

## The method

A Catmull-Rom curve passes through its control points, and each piece
depends on four of them. It also depends on the parameter value given to
each point. The paper looks at the family in which the parameter advances
by (distance to the next point)^α (eq. 1). With α = 0 every edge takes
the same parameter time whatever its length, and the curve loops at short
edges. With α = 1 the time is the length, and the curve bulges beside
long edges and overshoots at short ones. The paper proves that α = 1/2 is
the only value for which a piece can never form a cusp or cross itself,
and bounds the distance from the control polygon. `docs/catmullrom.md`
goes through it.

## What `--check` measures

65 assertions, about 1 s. "Edge" is the middle edge P1 P2 of four control
points; the two outer edges have lengths between e⁻⁴ and e⁴ times it, in
any direction; 20000 such sets for each α.

* The pyramid of figure 3 and the Bézier form of eq. 2 are the same curve
  (9e-16); the derivative is continuous where pieces meet (1e-11).
* Eq. 3, 4 and 5 agree with the Bézier points computed from eq. 2 (1e-15).

| α | 0 | 0.25 | 0.5 | 2/3 | 0.75 | 1 |
|---|---|---|---|---|---|---|
| pieces that start or end going backward (theorem 1) | 7531 | 6346 | 0 | 5448 | 6203 | 7305 |
| pieces that cross themselves | 526 | 223 | 0 | 2 | 1 | 10 |
| pieces that leave the strip between their points | 9837 | 6610 | 0 | 4645 | 5621 | 7006 |
| largest distance from the edge's line ÷ edge | 5.28 | 1.10 | 0.213 | 0.123 | 0.131 | 0.241 |
| the paper's bound for any neighbours (eq. 6) | none | none | 1/4 | 1/8 | (1/4) | 1/4 |
| largest distance ÷ the bound of eq. 6 for these neighbours | 0.991 | 0.993 | 0.995 | 0.999 | 0.994 | 0.991 |

* Centripetal: the first inner Bézier point is between 2e-10 and 0.587 of
  the way along the edge (theorem 3: in 0 … 1); the derivative along the
  edge is never negative (theorem 2).
* Chordal: the farthest point past an end of the edge is 0.113 edge
  lengths from that end (eq. 8: at most 1/3).
* Section 5: the angle γ is at most 0.428 in the random sets and 0.5236
  = π/6 at its largest over a grid of length ratios and angles.

## What this says

Everything the paper proves is seen: among the six values only α = 1/2
has no piece going backward, crossing itself or leaving its strip, and
eq. 6 holds for every α and is nearly reached.

One formula differs from the text as extracted from the PDF: section 5
prints tan γ = √r |sin θ| / (3 + 2√r − √r cos θ). From eq. 4 and 5, with
θ as defined for eq. 3, the last sign is +; the check confirms the + form
to 3e-16 and finds the − form off by up to 0.19. The largest value over θ
is the same for both, so the paper's π/6 is not affected.

## Not done

Curves in three dimensions, the circles around control points that eq. 8
adds to the boxes when α ≠ 1/2, the test for intersections between
different pieces (section 5's rules 2 and 3), the curvature observations
of section 6.4, the journal version's applications.
