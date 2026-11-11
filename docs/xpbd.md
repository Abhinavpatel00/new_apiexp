# XPBD — the paper and `demos/xpbd`

Miles Macklin, Matthias Müller, Nuttapong Chentanez: *XPBD: Position-Based
Simulation of Compliant Constrained Dynamics*, Motion in Games 2016.
<https://matthias-research.github.io/pages/publications/XPBD.pdf>

Sources: the paper was read from the abstract to its conclusion. Equation
and section numbers below are the paper's. No code was cloned for this
one; the paper gives none.

## 1. What was wrong with PBD

Position-based dynamics (Müller et al. 2007) moves particles in three
steps: let every particle fly freely for one time step, then push the
positions until the constraints hold, then call (new − old) / Δt the
velocity. A constraint is any function C(x) that should be zero: the
distance of two particles minus a rest length, the volume of a
tetrahedron minus its rest volume. Pushing means, for one constraint at a
time (eq. 1 and 2),

```
s  = −C / Σ wᵢ |∇ᵢC|²          wᵢ = 1 / mass of particle i
Δxᵢ = k s wᵢ ∇ᵢC
```

That is one Newton step on C along the direction that costs the least
mass-weighted movement, scaled by a stiffness k between 0 and 1.

The trouble is k. With k = 0.5 one pass removes half of the error, two
passes three quarters, ten passes all but a thousandth. So the material
gets stiffer with every pass and with every cut of the time step, and
with enough passes everything is rigid. k is not a property of the
material. An artist who doubles the iteration count to fix a collision
gets a different cloth.

## 2. The idea

Give every constraint a real stiffness and derive the push from implicit
time integration, so that more passes mean a more accurate answer to the
same question and not a different question.

A constraint with **compliance** α (one over stiffness) is defined to
stand for the energy (eq. 5)

```
U(x) = C(x)² / 2α
```

For a distance constraint this is a spring of stiffness 1/α. α = 0 is an
infinitely stiff, hard constraint, and that case causes no division by
zero below, which is why the paper works with α and not with stiffness.

## 3. The derivation

Implicit Euler written with positions only (eq. 4):

```
M (xⁿ⁺¹ − 2xⁿ + xⁿ⁻¹) / Δt² = −∇U(xⁿ⁺¹)ᵀ = −∇Cᵀ α⁻¹ C
```

Split the force into a direction ∇Cᵀ and a size, the multiplier
λ = −α̃⁻¹ C with α̃ = α / Δt² (eq. 7). Then the step is two equations in
two unknowns x and λ (eq. 8 and 9):

```
g:  M (x − x̃) − ∇C(x)ᵀ λ = 0            x̃ = xⁿ + Δt vⁿ, the free flight
h:  C(x) + α̃ λ = 0
```

Newton's method on g and h needs ∂g/∂x, which contains second derivatives
of C. The paper makes two approximations (section 4):

1. **∂g/∂x ≈ M.** This drops the second derivatives. It changes how fast
   the iteration converges, not where a converged iteration ends.
2. **g = 0 during the iteration.** It is true at the start (x = x̃,
   λ = 0) and stays nearly true while the gradients of C change slowly.
   *This* one changes the answer, and is why XPBD is "only an
   approximation of an implicit Euler integrator" (section 7).

What remains is a small linear system for Δλ (eq. 16),

```
(∇C M⁻¹ ∇Cᵀ + α̃) Δλ = −C − α̃ λ           Δx = M⁻¹ ∇Cᵀ Δλ     (eq. 17)
```

and solving it one row at a time (Gauss-Seidel) gives the formula the
whole method comes down to (eq. 18):

```
Δλⱼ = (−Cⱼ − α̃ⱼ λⱼ) / (Σ wᵢ |∇ᵢCⱼ|² + α̃ⱼ)
```

Compare with PBD's s: there are two new terms. The α̃ in the denominator
makes the push smaller for a softer constraint. The α̃ λ in the numerator
remembers how much has been pushed already in this time step: once
C = −α̃ λ, which says "the spring force balances what it has been asked
to carry", Δλ is zero and further passes do nothing. With α = 0 the
formula is PBD with k = 1 exactly.

The loop (Algorithm 1) is PBD's with three lines added: set λ = 0 at the
start of the time step, compute Δλ by eq. 18, add it to λ.

```
x̃ = x + Δt v + Δt² g                 predict
λ = 0
repeat `iterations` times:
    for every constraint:  Δλ by eq. 18;  x += w ∇C Δλ;  λ += Δλ
v = (x − x_old) / Δt
```

`xpbd_step` in `demos/xpbd/method.c` is this loop.

## 4. Forces for free

λ is the size of the constraint force times Δt². So `λ / Δt²` is the
tension in a link, without any extra work. The paper suggests using it
for joints that break above a force and for haptic devices. The demo's
check reads the tension of a chain's top link this way and compares it
with the weight hanging below: they agree to 2e-11.

## 5. Damping (section 5)

A damping force that resists the rate of change of C comes from the
dissipation potential ½ Ċ β Ċ (eq. 19). It acts along the same direction
as the elastic force, so both go into the one multiplier, and the update
becomes (eq. 26)

```
Δλ = (−C − α̃ λ − γ ∇C·(x − xⁿ)) / ((1 + γ) Σ wᵢ |∇ᵢC|² + α̃)
γ  = α̃ β̃ / Δt,     β̃ = Δt² β
```

∇C·(x − xⁿ) is how much C has changed since the start of the time step,
that is Δt Ċ. β is a damping coefficient (N s/m for a distance), not an
inverse like α. Note that γ is zero when α is zero: a hard constraint
cannot be damped this way, there is nothing moving to damp.

## 6. The constraints in the demo

`xpbd_value` returns C and its gradients for two kinds:

* **Distance.** C = |p₁ − p₀| − rest. The gradient at p₁ is the unit
  vector from p₀ to p₁, at p₀ its negative. Compliance in m/N.
* **Volume of a tetrahedron.** C = (p₁−p₀)·((p₂−p₀)×(p₃−p₀)) / 6 − rest.
  The gradient at p₁ is (p₂−p₀)×(p₃−p₀)/6 and so on round the three
  edges; at p₀ it is minus the sum of the other three, because moving
  all four corners together changes nothing. The demo sets the
  compliance to rest volume / bulk modulus, so that the energy C²/2α is
  ½ K V (ΔV/V)².

Cloth is distance constraints along the sides and diagonals of the
squares plus weaker ones that skip a node, which resist bending. The
block is distance constraints on the edges of its tetrahedra plus one
volume constraint per tetrahedron. This is the simplest soft body, not
the finite-element one of the paper's section 6.3, where the strain
entries of an element are the constraints and α is the inverse of the
material's stiffness matrix (eq. 27, 28).

**Contacts** are constraints with zero compliance that only push, solved
after each pass: a node below the floor or inside the ball is moved to
the surface, and loses a share of its sideways motion of this step
(friction). The paper says only that "collisions are handled exactly as
in the original PBD method" with zero compliance; the details here are
the demo's.

## 7. The reference solver

The paper validates XPBD against "a traditional non-linear Newton solver
applied directly to the discrete equations of motion (8, 9)" (section
6). `xpbd_reference_step` is that: it minimises

```
Σ mᵢ |xᵢ − x̃ᵢ|² / 2Δt² + Σ Cⱼ(x)² / 2αⱼ
```

by Newton's method with the full second derivatives of a length and
dense elimination with row exchange, until a Newton step moves nothing by
more than 1e-13 m. There is no line search; the tests it is used for did
not need one. It handles distance constraints only. Whatever XPBD differs from this by after many
passes is the error of approximation 2 above.

## 8. What the check measures

All numbers are from `xpbd --check` on lavapipe; they are properties of
`method.c` and do not depend on the GPU.

**One spring.** A 2 kg mass on a spring with α = 0.001 m/N must hang
α m g = 19.62 mm low. XPBD does, to 8.7e-10, with 1, 10 or 100 passes
and with Δt = 1/60 or 1/240 s. PBD with k = 0.5 on the same six runs
gives anything from 0 to 2.7 mm. This is the paper's Figure 2 at rest.

**A chain** of 15 links with a 1 kg weight. Each link should stretch by α
times the weight below it.

| passes | XPBD: length error | PBD k = 0.5: stretch / right stretch |
|---|---|---|
| 5 | 1.05 % too long | 0.77 |
| 20 | 1.4e-6 | |
| 80 | 4.1e-12 | |
| 320 | 4.1e-12 | 0.010 (rigid) |

So XPBD converges to one answer and PBD has no answer to converge to.
But the first row matters too: **too few passes make XPBD too soft**,
because the weight's pull has not travelled up the chain yet. The paper
says so in section 7: "low iteration counts that terminate before
convergence will result in artificial compliance", and "XPBD does not
converge faster than PBD". A heavy weight on light links is the worst
case: with 5 kg on 0.02 kg nodes the same chain needed hundreds of
passes.

**A swinging chain**, compared with the Newton reference after 1 s in
which the weight travels 1.4 m: 1.318 mm apart with 20 passes, 1.316 mm
with 200 and with 2000. The remaining 1.3 mm does not shrink with more
passes. It is approximation 2, the price of not solving g = 0.

**Hard constraints.** With α = 0, XPBD and PBD with k = 1 give the same
positions bit for bit after 240 steps, as section 4.1 says.

**Cloth and block.** The cloth ends up outside the ball to 0.006 mm; its
most stretched side is 9.6 % long, with a stiffness of 10^3.5 N/m and 20
passes (reported, not judged: part of it is real stretch, part the
artificial compliance again). The block with a bulk modulus of 1e9 Pa
keeps its volume to 2e-5 while lying on the floor.

## 9. What was not implemented

* The Jacobi variant on the GPU that the paper's 3-D results use.
* Finite-element constraints with a compliance matrix (section 6.3), the
  balloon with one global volume constraint (6.5).
* Warm starting λ from the previous time step, which section 7 proposes
  as future work.
* Rigid bodies, self-collision.
