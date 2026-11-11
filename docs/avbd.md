# Augmented Vertex Block Descent — the paper and `demos/avbd`

Chris Giles, Elie Diaz, Cem Yuksel: *Augmented Vertex Block Descent*,
ACM Transactions on Graphics 44(4), article 90, SIGGRAPH 2025.
<https://graphics.cs.utah.edu/research/projects/avbd/>

Sources: sections 2.2, 3 (all of it) and 4 up to Algorithm 1 of the
paper were read; the introduction, the rest of section 4 (rotations in
3-D), the results and the limitations were **not**. The authors' 2-D
reference demo, <https://github.com/savant117/avbd-demo2d>, was cloned
to `/tmp/ref/avbd-demo2d` (outside the workspace) and its solver, joint,
spring, contact and collision files were read. `demos/avbd/method.c` is
new code in C that follows that demo's structure and constants.
Equation and section numbers are the paper's.

## 1. Where it starts: vertex block descent

One step of implicit Euler is the minimum of (eq. 1)

```
|x − y|²_M / 2Δt²  +  E(x)            y = x + Δt v + Δt² a_ext      (eq. 2)
```

"Stay near where momentum would carry you, and keep the potential energy
low." Vertex block descent (Chen et al. 2024) does not solve this for
all bodies at once. It takes one body i, holds every other body still,
and moves i to the minimum of its own part of the sum (eq. 3) with one
Newton step (eq. 4 to 6):

```
H_i Δx_i = f_i
f_i = −M_i (x_i − y_i) / Δt² + Σ_j f_ij           forces on i
H_i =  M_i / Δt²             + Σ_j H_ij           their derivatives
```

For a particle that is a 3 × 3 system, for a rigid body in the plane
also 3 × 3 (x, y, angle), in space 6 × 6. Sweeping over the bodies
several times is Gauss-Seidel on the positions.

This is a **primal** method: the unknowns are positions. XPBD (see
`docs/xpbd.md`) is **dual**: the unknowns are constraint forces. Each
has a weakness:

* dual methods handle hard constraints naturally but converge badly when
  a light body carries a heavy one (high mass ratio);
* primal methods do not care about mass ratios, but a force enters as
  `½ k C²`, so a hard constraint would need k = ∞, and forces of very
  different stiffness on one body converge badly (high stiffness ratio).

AVBD keeps the primal sweep and adds a dual variable per constraint to
remove the second weakness.

## 2. Hard constraints: the augmented Lagrangian (3.1)

A constraint C_j(x) = 0 gets, in iteration n, the energy (eq. 8)

```
E_j = ½ k C² + λ C               force on body i:  −(k C + λ) ∂C/∂x_i     (eq. 9)
```

with a **finite** k. After each sweep over the bodies (eq. 11, 12)

```
λ ← k C + λ                      the multiplier takes over the force
k ← k + β |C|                    and the stiffness grows with the error
```

Read the first line like this: the spring `k C` was pulling with some
force while still stretched by C. Put that force into λ, and the same
pull is now there with *no* stretch needed. Repeat, and C goes to zero
while k stays moderate. k only sets how fast λ is found. Both start at
k = k_start, λ = 0 (eq. 10). The paper uses β = 10 and reports that
anything from 1 to 1000 works; the reference demo uses 100 000 with a
note that the right value depends on the units, and so does this demo.

## 3. Bounded forces: contacts and friction (3.2, 3.3)

A contact may push but not pull. Friction may not exceed μ times the
normal force. Both are bounds on the force, and the force is the number
`λ⁺ = k C + λ`, so it is clamped (eq. 13):

```
force = clamp(k C + λ, λ_min, λ_max)
```

in the primal sweep and in the update of λ. This is what projected
Gauss-Seidel does in dual solvers, brought to a primal one. The
stiffness grows only while the force is strictly inside its bounds.

Clamping makes the force constant outside the bounds, so its true
derivative there is zero. The paper offers two answers: use the
unclamped derivative anyway, or rescale k so that the line through the
clamped force is matched (eq. 14). The reference demo and this one use
the first.

A contact point has a normal row and tangent rows (eq. 15): C is the gap
between the two contact points, measured along the normal and along the
tangent. The normal row has bounds (−∞, 0] here (sign as in the
reference demo: a negative force pushes apart); the tangent row is
bounded by ±μ |λ_normal|. If a contact was inside its friction bound on
the last step, its contact points are kept and not recomputed, so a
resting box cannot creep (this is taken from the reference demo). The
demo draws such contacts white.

For contacts C is not re-evaluated from the geometry inside the
iterations. It is taken as a straight line from the start of the step,
`C(x) ≈ C(x_start) + J (x − x_start)`, with J fixed, as the
reference demo does.

## 4. Soft forces with a stiffness ratio (3.4)

Take a body held by a weak spring and a spring 10 000 times stiffer.
When VBD moves that body it satisfies the stiff spring and all but
ignores the weak one, because that *is* the minimum for the body alone.
The body on the other end of the stiff spring does the same. So the pair
defers to each other, and the weak spring's pull on the pair as a whole
gets through only a ten-thousandth at a time. The paper's figure 2 shows
the result: the weak spring sags far too much.

The fix reuses the stiffness ramp: a soft force of real stiffness k*
starts each step with a low k and grows (eq. 16)

```
k ← min(k*, k + β |C|)
```

without a multiplier, since k* is finite. In the first iterations all
forces are of similar, low stiffness, so the weak ones get their say
across the whole system; then the stiff ones are stiffened as far as
their error calls for.

The price, visible in the demo's check: with 5 iterations the stiff
spring has not reached its real stiffness and stretches 0.3 mm where
0.0005 mm would be right. Small, but 600 times too much.

## 5. A Hessian that is always safe (3.5)

The derivative of the force of a constraint is (eq. 17)

```
H_ij = k (∂C/∂x)ᵀ (∂C/∂x) + λ⁺ ∂²C/∂x²
```

The first part is positive semi-definite. The second, G, need not be,
and with large λ⁺ it can make H indefinite, so that the Newton step goes
the wrong way. AVBD replaces G by a diagonal matrix holding the lengths
of G's columns. That is always positive, so H can be factored by LDLᵀ
and every step makes progress; it is a quasi-Newton step.

In the plane: a joint row has one non-zero second derivative (the angle
with itself), a spring has a full 3 × 3 one, a contact none.

## 6. Old error must not become speed (3.6)

Position methods turn constraint error into motion: if a joint is 5 cm
apart at the start of a step, the solver closes it in one step, and
(new − old)/Δt calls that a velocity of 3 m/s. For a hard constraint
the force is unbounded, so this can be violent. AVBD lets hard
constraints ignore most of the error they start the step with (eq. 18):

```
C(x) = C*(x) − α C*(x_start)             α = 0.95 in the paper
```

Only the share 1 − α is corrected per step, so the error decays over
many steps and little speed is added.

The reference demo's default is a variant, **post-stabilisation**: all
iterations use α = 1 (ignore old error entirely), velocities are taken,
and one extra primal sweep with α = 0 then moves bodies to remove the
error. That movement comes after the velocities, so it adds no speed at
all. The demo's check drops a box that starts 0.3 m inside the ground:
it leaves at 18 m/s with α = 0, at 0.9 m/s with α = 0.95, and at
0.00005 m/s with post-stabilisation.

## 7. Warm starting (3.7)

k and λ of the last step are good guesses for this one, but k can only
grow inside a step, so both are scaled down first (eq. 19):

```
k ← max(γ k, k_start)        λ ← α γ λ             γ = 0.99
```

(With post-stabilisation the reference demo keeps λ in full.) A new
contact starts from k_start. Positions are warm started too, as in VBD:
the first guess is x + Δt v plus gravity only in the measure that the
body was actually accelerating downwards in the last step, so a body at
rest on the floor is not first dropped and then pushed back.

## 8. One time step (Algorithm 1)

```
find contacts                                          avbd_collide
y = x + Δt v + Δt² a_ext;   x = first guess
λ, k from the last step, scaled
repeat:
    for each body:   f, H as above;  x ← x + H⁻¹ f     primal
    for each force:  λ ← clamp(k C + λ);  k ← k + β|C| dual
v = (x − x_start) / Δt
```

The paper colours the bodies so that those of one colour share no force
and can be updated in parallel on the GPU. `avbd_step` runs them one
after another.

## 9. The boxes

`avbd_collide` is the box against box routine of Box2D-lite, which the
reference demo also uses: of the four face directions take the one with
the least overlap (the reference face), take the side of the other box
that faces it most squarely (the incident edge), clip that edge to the
reference face's width, and keep the ends that are below the face. That
gives up to two points, each named by the corner and face it came from,
so that next step's contact can inherit this step's k and λ.

A joint holds a point of one body to a point of another (two rows) and
optionally their relative angle (a third row, scaled by a length so that
all three rows are in metres). A spring is `C = distance − rest`.

## 10. What the check measures

* **Figure 2.** Fixed block, 1000 N/m spring, 0.5 kg block, 10⁷ N/m
  spring, 0.5 kg block, released. The weak spring should end stretched
  by 10 N / 1000 N/m = 0.01 m.

  | solver | longest stretch in 10 s | at the end |
  |---|---|---|
  | AVBD, 5 iterations | 0.0155 m | 0.01001 m |
  | AVBD, 1 iteration | 0.0209 m | |
  | VBD, 5 iterations | 2.30 m | |
  | VBD, 100 iterations | 0.43 m | |
  | VBD, 1000 iterations | | 0.010001 m |

  This is the paper's figure, with numbers: 5 iterations of AVBD do what
  VBD does not do with a hundred and does with a thousand.

* **Coulomb friction.** On a slope of 0.35 rad a box with μ = 0.5 moves
  5e-9 m in a second. One with μ = 0.2 slides, and the distance matches
  g (sin θ − μ cos θ) to 5e-6. The bound on the force is exact, not a
  smoothed approximation, which is the point of section 3.2.

* **Stacking.** Ten boxes stand for 10 s with speeds of 3e-15 m/s; a
  pyramid of 105 settles to 1.6e-4 m/s.

* **Hard joints under load.** A rope of ten 0.5 kg links carrying a
  100 kg box. The multiplier of the top joint gives the weight below to
  3e-4. The joints are not closed exactly: 5.2 mm of gap with 10
  iterations, 2.4 mm with 30, after 10 s at rest. A possible
  reason, not verified: with post-stabilisation the single correcting
  sweep uses the current k, and k decays by γ every step while nothing
  moves. Plain VBD with springs of 10⁶ N/m in place of the joints leaves
  1.05 mm, exactly weight / stiffness. So on this test the penalty
  method is as good; it would need a larger stiffness for a heavier
  load, and that is the stiffness-ratio problem again as soon as
  anything softer is in the scene.

## 11. What was not implemented

* Three dimensions, the GPU implementation, colouring, the bounding
  volume hierarchy.
* Particles, soft bodies and cloth with the same solver.
* Separate static and dynamic friction coefficients, stiffness rescaling
  (eq. 14), joints that break, motors.
