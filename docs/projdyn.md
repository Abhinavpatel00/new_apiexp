# Projective Dynamics — the paper and `demos/projdyn`

Sofien Bouaziz, Sebastian Martin, Tiantian Liu, Ladislav Kavan, Mark
Pauly: *Projective Dynamics: Fusing Constraint Projections for Fast
Simulation*, ACM Transactions on Graphics 33(4), 2014.
<https://www.cs.utah.edu/~ladislav/bouaziz14projective/bouaziz14projective.pdf>

Sources: sections 2 to 6, the start of 7 and appendix A were read. The
rest of section 7 (timings, comparison with Newton) and section 8 were
not read. Equation and section numbers below are the paper's. No code
was cloned; the paper's page offers none.

## 1. The problem

A soft body is nodes joined into tetrahedra. Two families of solvers
existed:

* **Finite elements with Newton's method.** Accurate, and implicit Euler
  makes it stable. But every Newton iteration builds and solves a new
  linear system, and it needs a line search and fixes for matrices that
  are not positive definite.
* **Position-based dynamics.** Fast and never blows up, but the stiffness
  depends on the iteration count and the result on the order of the
  constraints (see `docs/xpbd.md`).

The paper builds a solver that is as robust and as simple as the second,
and that converges to a true implicit Euler solution like the first, for
a restricted but useful family of elastic energies.

## 2. Implicit Euler as a minimum (section 3.1)

Implicit Euler, `q_new = q + h v_new`, `v_new = v + h M⁻¹ (f_int(q_new) +
f_ext)`, is the same as finding the q that minimises (eq. 4)

```
|M^½ (q − s)|² / 2h²  +  Σ Wᵢ(q)           s = q + h v + h² M⁻¹ f_ext
```

s is where the nodes would be if nothing held them together. The first
term says "stay near s, heavy nodes more so". The second is the elastic
energy. The time step h and the masses set the balance. Setting the
derivative to zero gives implicit Euler back, which the check uses: the
size of that derivative measures how far a solver is from it.

## 3. The restriction (section 3.2)

An elastic energy usually does two things in one formula: it says which
shapes are at rest (rotated copies of the rest shape), and how costly it
is to be away from them. The first is what makes the problem nonlinear.
The paper separates them. Each energy term gets a helper variable p that
must lie in a **constraint set** C of allowed shapes, and the cost is a
plain squared distance to p (eq. 7):

```
W(q, p) = w/2 |A q − B p|²        with p in C
```

A and B are fixed matrices, w a weight. All the nonlinearity is in the
set C. Minimising over p is a projection: find the point of C nearest to
A q. The energy of q alone is then "w/2 × squared distance from A q to
the set". What is lost is freedom in the material law: the cost always
grows with the square of the distance, so no general nonlinear material.

## 4. The solver (section 3.3, Algorithm 1)

The whole objective (eq. 8) is minimised by taking turns:

* **Local step** (eq. 9). Hold q. Each constraint finds its own nearest
  point pᵢ. The constraints do not interact, so this is a loop that
  could run in parallel.
* **Global step** (eq. 10). Hold all pᵢ. The objective is now a
  quadratic in q, and its minimum is one linear solve:

  ```
  (M/h² + Σ wᵢ AᵢᵀAᵢ) q = M s / h² + Σ wᵢ AᵢᵀBᵢ pᵢ
  ```

  **The matrix on the left has no q and no p in it.** It is factored
  once, before the simulation, and every global step is a back
  substitution. This is the whole reason for the method's speed.

Each step lowers the objective or leaves it, and the objective cannot go
below zero, so the iteration converges whatever the time step and
however bad the start, with no line search. That holds even though the
sets are not convex (the set of rotations is not).

```
s = q + h v + h² M⁻¹ f_ext
q_new = s                             the start: follow the momentum
repeat `iterations` times:
    for every constraint: pᵢ = nearest point of Cᵢ to Aᵢ q_new
    q_new = solve the fixed system with s and all pᵢ
v = (q_new − q) / h
```

The paper notes that starting from s gives less damping with few
iterations than starting from the last positions. `pd_step` in
`demos/projdyn/method.c` is this loop.

## 5. Why A matters (end of section 3.3)

With A = identity the constraint works on positions, the matrix is
diagonal and the solve is trivial, but a change at one node reaches the
next only through shared constraints, one iteration at a time. If A
takes **differences** of positions (edges, gradients), the matrix
couples neighbours, like a Laplacian, and one global solve spreads a
change through the whole body. The demo's check shows it: after 10
iterations the nodes are within 0.7 % of the step's movement from where
1000 iterations put them.

## 6. Relation to PBD (section 4)

One PBD push of a constraint is the minimum of ½|M^½ Δq|² subject to the
linearised constraint (eq. 12, 13). So PBD is Gauss-Seidel on the sum of
squared distances to the sets (eq. 11), **without the momentum term**:
the free flight is done once, first, and every later pass forgets a bit
more of it. Hence stiffness that grows with the passes. If the sets
cannot all be met at once (a squeezed body between two walls),
Gauss-Seidel jumps between them for ever.

Projective Dynamics makes two changes: project onto all sets at once
from the same q and then find the best compromise (Jacobi style, which
settles even when the sets disagree), and keep the momentum term in the
compromise.

## 7. The constraints of the demo

For a tetrahedron with rest edges X_g and current edges X_f (3 × 3
matrices whose columns are three edges from one corner), the
**deformation gradient** is `F = X_f X_g⁻¹`: the matrix that takes the
rest shape to the current one. F is linear in the node positions, so
"A q" is F, and the potential is (eq. 16, for volumes)

```
W = w/2 × V × |F − T|²            T in the set M,  V the rest volume
```

The factor V, and A built from the rest shape, are what make the result
nearly independent of how the body is cut into tetrahedra (Figure 5).

**Strain (section 5.1, eq. 22 and 23).** M = matrices whose singular
values all lie in [σ_min, σ_max]. Write F = U Σ Vᵀ; the nearest T is
U Σ* Vᵀ with each singular value clamped into the range. If det F < 0,
the tetrahedron is inside out and the last singular value is negated so
that T is never a reflection. With σ_min = σ_max = 1 the set is the
rotations, and W is corotated elasticity with stiffness w. With a wider
range (`--limit`), deformation inside the range costs nothing and only
the excess is resisted: strain limiting, in the same solver.

**Volume (section 5.2, eq. 24 to 26).** M = matrices with σ_min ≤ det ≤
σ_max. Again only the singular values change: find the smallest change D
with Π(Σᵢ + Dᵢ) = σ, where σ is the bound that was passed. There is no
closed form; the paper linearises the product and repeats (eq. 26):

```
D ← ((∇C·D − C) / |∇C|²) ∇C           C(D) = Π(Σᵢ + Dᵢ) − σ
```

Each round is the nearest point to Σ on the tangent plane of the surface
"product = σ" at the current guess. `pd_project_volume` runs 40 rounds of it.

**Attachment (the positional constraints of section 6).** A = identity
on one node, the set is one point. It adds its weight to that node's
diagonal entry. The demo holds the bar's end faces with weight 1e7.

`pd_svd` gets the singular values from the eigenvectors of FᵀF by Jacobi
rotations, with det U = det V = 1 and the sign of an inverted F in the
last singular value, which is what both projections need.

## 8. The linear system here

Because every A acts the same way on x, y and z, the system is three
systems with the same nodes × nodes matrix. For a tetrahedron, AᵀA is
`V G Gᵀ`, where G holds the four gradients of the corner weights (the
rows of X_g⁻¹ and minus their sum). `pd_factor` builds the 425 × 425
matrix densely and factors it by Cholesky. That costs nodes³ once and
nodes² per solve, fine for 425 nodes and useless for 10 000; the paper
uses a sparse factor.

## 9. What the check measures

Numbers from `projdyn --check`; they are properties of `method.c`.

| what | result |
|---|---|
| U Σ Vᵀ = F for 2000 random F, a third flat or inverted | to 1.0e-15 |
| the strain projection lands in the set, and F − p(F) equals the derivative of half the squared distance | 6.7e-16, 3.0e-10 |
| the same for the volume projection | 1.6e-15, 9.2e-9 |
| L Lᵀ = A | 5.6e-16 |
| largest rise of the objective in any of 400 iterations | 0 |
| gradient of the objective after 1000 iterations / at the start | 1.7e-13 |
| share of the objective's fall still to come after 10 iterations | 2.9e-7 |
| 10 against 1000 iterations, distance / movement | 0.0069 |
| hanging bar: stretch against ρ g L² / 2E | −9.1e-5 |
| free bar, 2 s: momentum change; angular momentum kept | 2.6e-11; 81 % |
| time step 0.5 s, 120 steps | stays bounded, slows from 3.5 to 0.44 m/s |

The second and third rows test that each projection really is the
nearest point: the derivative of ½ dist² at F is F − p(F), and it is
compared with differences of ½|F − p(F)|². The sixth is the paper's
claim that the solver "converges to a true implicit Euler solution":
the gradient of the objective of section 2, with the energy of section
3, goes to zero. The hanging bar checks that w really is a Young's
modulus when there is no sideways strain coupling: for this energy a
bar under its own weight stretches by ρ g L² / 2E.

Two things to know. Linear momentum is kept to rounding, but **angular
momentum is not**: 19 % is lost in 2 s. That is implicit Euler's own
damping, which the paper accepts ("numerical damping is not a major
concern"), not an error of the solver. And with few iterations the body
is a little softer and more damped than the converged one, though far
less so than PBD.

## 10. What was not implemented

* Sparse Cholesky. The demo's dense factor limits it to small meshes.
* Triangles and edges (cloth, the 1-D case that equals Liu et al. 2013),
  example-based materials (5.3), bending by the Laplace-Beltrami operator
  (5.4), collisions as half-space constraints added on the fly, tearing
  (section 6 and 7).
* A parallel local step.
