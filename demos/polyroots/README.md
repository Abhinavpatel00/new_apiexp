# polyroots — a picture made of polynomial roots

```
(method.c on the CPU: for every cell and curve the roots of a degree 5 polynomial) ─► Draw ─► Present
```

Cem Yuksel: *High-Performance Polynomial Root Finding for Graphics*
(Proc. ACM Comput. Graph. Interact. Tech. 5(3), HPG 2022).

Three thick cubic curves seen along z, turning. For every cell of the
picture and every curve, the point of the curve nearest to the cell's ray
is a root of a polynomial of degree 5 in the curve's parameter; the cell
is covered when that point is within the radius. This is the "closest
point approach" that the paper's section 6.4 also renders with its
solver.

The box at the lower right is that polynomial for the white probe and
the orange curve: yellow where a monotonic piece changes sign and is
searched, grey where it is skipped, vertical ticks at the zeros of the
derivative, dots at the roots. Lines go from the probe to the curve at
each root; the nearest point is the yellow one.

| option | |
|---|---|
| `--epsilon E` | error threshold of the root finder for the picture (0: full precision). At 0.1 the curves break up |
| `--radius R` | radius of the curves |
| `--cells N` | rows of the picture (36 … 108) |
| `--no-graph` | without the probe and its polynomial |
| `--still` | the curves do not turn |

## The method

Between two neighbouring zeros of its derivative a polynomial only rises
or only falls. So it has at most one root there, and it has one exactly
when its values at the two ends have different signs. The solver finds
the zeros of the derivative (the same problem one degree lower, down to
a quadratic), cuts the interval at them, skips the pieces without a sign
change, and in the others runs Newton iterations that cannot escape:
every evaluated point shrinks the interval that holds the root, and a
step that leaves it is replaced by the midpoint. `docs/polyroots.md` goes
through it.

## What `--check` measures

126 assertions, about 13 s (most of it the reference in long double).

**Random Bernstein polynomials with coefficients in −1 … 1, roots in
0 … 1, full precision** (10000 of each degree, 3000 of degree 20). The
shares with no, one, two or more roots are compared with the paper's
table 1; every root is compared with a bisection on the Bernstein form in
long double.

| degree | no root % | one % | two or more % | the paper's table 1 | largest error of a root | iterations a search | µs a polynomial |
|---|---|---|---|---|---|---|---|
| 3 | 27.0 | 48.1 | 24.9 | 27, 48, 24 | 1.5e-14 | 6.5 | 0.17 |
| 4 | 21.6 | 46.2 | 32.1 | 22, 45, 33 | 1.7e-14 | 6.7 | 0.44 |
| 5 | 16.9 | 41.1 | 41.9 | 18, 42, 41 | 5.7e-14 | 7.0 | 0.79 |
| 6 | 14.2 | 37.7 | 48.1 | 15, 38, 47 | 2.1e-13 | 7.4 | 1.2 |
| 7 | 11.9 | 34.9 | 53.2 | 12, 35, 53 | 2.5e-13 | 7.8 | 1.9 |
| 8 | 10.2 | 32.6 | 57.2 | 10, 32, 58 | 8.5e-13 | 8.3 | 2.7 |
| 9 | 8.9 | 29.5 | 61.6 | 9, 29, 62 | 1.9e-12 | 8.9 | 3.8 |
| 10 | 7.4 | 26.4 | 66.2 | 8, 27, 66 | 6.6e-12 | 9.5 | 5.1 |
| 20 | 2.3 | 11.6 | 86.1 | 2, 12, 86 | 9.8e-8 | 16.4 | 45 |

No sign change between 513 samples of the Bernstein form is without a
reported root, at any degree. Up to degree 10 every reported root is a
sign change of the Bernstein form within 1e-7. At degree 20, 4 reported
roots are not: the power form that the solver is given has lost that much
by then (the paper: "simply evaluating high-order polynomials is prone to
numerical truncation errors").

**Cubics with an error threshold.**

| ε | largest error | mean error | the paper's mean (single precision) | iterations a search |
|---|---|---|---|---|
| 1e-2 | 3.8e-3 | 4.6e-5 | 5e-5 | 2.7 |
| 3.5e-4 | 6.1e-6 | 2.9e-8 | 5e-8 | 3.5 |

**Plain Newton iterations** on the same cubics (from 0, each step kept in
0 … 1, at most 40): the root is not found for 31.5 % of the cubics with
one root (the paper: 33 %), 10.9 % of those with two (11 %), 17.7 % of
all (18 %).

**Known roots.** Chebyshev T₁₀: all ten roots to 1e-15. (x − 0.1) … (x −
0.9): nine roots to 3e-12. (x − 0.5)²(x + 1): the root at 0.5, where the
curve only touches zero, is not reported; the paper names this as a
failure case of every method it tested.

**The picture's use.** The distance from a point to a cubic by the roots
agrees with a search to 8e-16 for 2000 random cases.

## What this says

The statistics of the paper's test are reproduced to within 1.3 points, and
so are its mean errors and its failure rates of plain Newton iterations.
The times in the last column are close to the paper's "Ours (full)"
column (0.17, 5.45 and 43 µs at degrees 3, 10 and 20), but that is two
different machines and this file has none of the paper's optimisations,
so it means little.

## Not done

The paper's faster path for cubics (unsafeguarded steps after a split at
the inflection point), deflation, the step of eq. 10 that bounds the
error for higher degrees, infinite intervals, the comparison with other
solvers, single precision, and the degree 10 polynomial for the true
surface of a thick curve (the picture uses the closest point approach,
degree 5, orthographic rays).
