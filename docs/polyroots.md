# High-Performance Polynomial Root Finding for Graphics — the paper and `demos/polyroots`

Source. Cem Yuksel, "High-Performance Polynomial Root Finding for
Graphics", Proc. ACM Comput. Graph. Interact. Tech. 5(3), article 7 (HPG
2022), DOI 10.1145/3543865
(`http://www.cemyuksel.com/research/polynomials/polynomial_roots_hpg2022.pdf`).
Read: the abstract, sections 3, 4, 5, 6.1, the start of 6.2 with table 1,
and parts of 6.4. Not read: sections 1.1, 1.2 and 2 (earlier work), 6.3,
tables 2 to 4 in detail, the conclusion, the supplemental document. The
paper's code (the paper says it is online and cites
`http://www.cemyuksel.com/?x=polynomials`) was not looked at.
Equation, section, figure and table numbers are the paper's.

## 1. The problem

Many graphics problems end in "find the real roots of this polynomial in
this interval": where a ray meets a curved surface, when two moving
triangles first touch, which point of a curve is nearest. Degree 2 has a
formula. Degree 3 and 4 have formulas that are slow and lose accuracy.
Above that there is none. Newton iterations are fast but can jump
anywhere or cycle, and they find one root at most, without saying whether
there are others.

## 2. Monotonic pieces (section 3.1)

Between two neighbouring zeros of the derivative `f'` the polynomial `f`
only rises or only falls. So on such a piece

* there is at most one root, and
* there is one exactly when `f` has different signs at the two ends.

For a cubic, `f'` is a quadratic, solved by its formula; its zeros cut
the target interval into at most three pieces, and two evaluations of
`f` per piece say which pieces to search. A piece without a sign change
is skipped at once; the paper names this as a main reason for its speed
(27 % of its random cubics have no root in the interval).

## 3. Newton iterations that cannot escape (sections 3.2, 3.3)

A Newton step (eq. 3)

    x ← x − f(x) / f'(x)

is unreliable where `f'` is small, which here is at the ends of the
pieces. The paper's guaranteed search (3.3) keeps an interval that holds
the root and

* after evaluating `f(x)`, moves the end with the same sign to `x`
  (eq. 6): the interval shrinks every iteration, at no extra cost;
* if the Newton step does not land inside the interval, takes the
  midpoint instead (eq. 7);
* starts at the middle of the piece, away from both ends where `f' = 0`.

It stops when the Newton step is at most the error threshold ε. With
ε = 0 it runs until the step no longer changes the number.

For cubics the paper first tries a cheaper loop (3.2): Newton steps only
clamped to the piece (eq. 4), which converges for a cubic if the middle
piece is split at the inflection point, for a fixed number of steps, with
3.3 as the fallback. It then removes a found root by division (deflation,
3.4, eq. 8, 9) and solves the remaining quadratic. `method.c` uses the
guaranteed search of 3.3 everywhere and has neither of these.

## 4. Higher degrees (section 4)

The same idea, recursively: to cut a polynomial of degree `d` into
monotonic pieces, find the roots of its derivative, of degree `d − 1`,
with the same solver; the recursion ends at the quadratic. The worst case
grows with the square of the degree. `pr_roots` is this recursion.

The paper notes two limits. Evaluating a high-degree polynomial loses
digits, so accuracy is bounded by the arithmetic. And for degrees above 3
a small Newton step does not prove that the root is within ε; the paper
gives a step that tries to land on the other side of the root (eq. 10)
but reports that its experiments never needed it and that its tests use
it only for ε = 0. Not in `method.c`.

## 5. Infinite intervals (section 5)

Not in the demo. The paper handles them by the sign of the leading
coefficient (eq. 11) and by replacing bisection with a fixed shift while
one end is infinite.

## 6. The demo

**The picture.** A ray along z through the point `p` of the picture, and
a cubic curve `c(t)`. The squared distance between them in the picture
plane is `|c(t) − p|²`; its derivative is zero where

    g(t) = (c(t) − p) · c'(t) = 0

a polynomial of degree 5. The nearest point is at one of its roots in
0 … 1 or at an end. The cell is covered if the distance is under the
radius. The paper's section 6.4 calls this the closest point approach,
says that it "does not provide the correct hit point with the hair's
surface", and renders with it by "generating and then solving a degree 5
polynomial"; its main test there is the exact surface of a thick curve,
a polynomial of degree 10, which the demo does not have. The demo's
shading takes the surface as a ball round the nearest point.

**The box.** `g` for the probe and the first curve, with what the solver
does: ticks at the zeros of `g'` (found by the same solver on a degree 4
polynomial, which in turn needs a cubic's and a quadratic's roots),
pieces with a sign change in yellow, the others grey, roots as dots.

## 7. What the check measures

Numbers are in `demos/polyroots/README.md`.

1. The paper's test: polynomials in Bernstein form with random
   coefficients in −1 … 1, converted to powers of x, roots in 0 … 1. The
   shares with no, one, two or more roots are a property of the test, not
   of the solver, so a correct solver must reproduce table 1's first
   columns. The reference is independent of the power form: the
   Bernstein form by de Casteljau's steps in long double, sampled and
   bisected.
2. Cubics at the paper's two thresholds (figure 1: ε = 3.5e-4 and 1e-2):
   largest and mean error.
3. Plain Newton iterations as section 6.1 describes them ("begins
   iterations at the start of the target interval and clamps the guesses
   to the target interval", up to 40 steps): how often an existing root
   is not found.
4. Known roots, and a double root.
5. The nearest point against a search.

## 8. Agreement with the paper

* Table 1's shares: within 1.3 points at every degree from 3 to 10, and
  0.4 at degree 20.
* Mean error of cubic roots: 4.6e-5 at ε = 1e-2 and 2.9e-8 at 3.5e-4;
  the paper's figure 1 gives 5e-5 and 5e-8 (in single precision, with
  its cubic path, so the agreement is of the order of magnitude).
* Plain Newton: 31.5 %, 10.9 %, 17.7 % against the paper's 33 %, 11 %,
  18 % (figure 2). What counts as "found" is this demo's definition:
  within 1e-3 of a root when the step falls under 3.5e-4.
* The largest error stays under ε for cubics (3.8e-3 at 1e-2), as the
  paper says ε bounds it. `method.c` does not split at the inflection
  point, so this is measured here, not guaranteed.
* A double root inside the interval is not reported; the paper lists
  that as a failure case of all methods it tested.
* Degree 20: 4 of the reported roots are more than 1e-7 from a root of
  the Bernstein form, and the largest error of the others is 1e-7. No
  root is missed. The paper warns about the arithmetic at high degree;
  it does not give error numbers that could be compared.

Times: 0.17, 5.1 and 45 µs a polynomial at degrees 3, 10 and 20 with
ε = 0. The paper's "Ours (full)" column has 0.17, 5.45 and 43.13 µs on
an Intel Xeon E5-2643 v3. The closeness is a coincidence of two machines
and two implementations and is not a result.

## 9. Not implemented

Sections 3.2 and 3.4 (the cubic fast path and deflation), eq. 10,
section 5, single precision, the comparisons with the analytical
solutions, Flocke's method and RPOLY, the degree 10 thick-curve
polynomial of section 6.4.
