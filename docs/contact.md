# Contact and Friction Simulation for Computer Graphics — the notes and `demos/contact`

Sheldon Andrews, Kenny Erleben, Zachary Ferguson: *Contact and Friction
Simulation for Computer Graphics*, SIGGRAPH 2022 Courses, 172 pages,
doi 10.1145/3532720.3535640.
Notes: <https://siggraphcontact.github.io/assets/files/SIGGRAPH22_friction_contact_notes.pdf>

Section, equation, algorithm and table numbers below are the notes'.
Only the sections named here were read for the demo; the list at the end
says which were not.

## 1. What the notes are

A course, not one method. Chapter 1 states the contact problem, chapter 2
finds contacts, chapter 3 solves the problem numerically, chapters 4 and
5 treat soft bodies, chapter 6 a new friction model. The demo follows the
rigid-body thread: sections 1.5, 1.7, 1.9, 2.1, 3.2 and 3.4.

## 2. A contact (sections 1.5, 1.7)

A contact has a point, a normal n from body A to body B and two tangents.
With u the velocities of all bodies, the relative velocity at the contact
is v = J u, one row of J for each direction (eq. 22, 43):

    row for direction d:   [ −d,  −(r_A × d),  d,  r_B × d ]

An impulse λ there changes u by M⁻¹ Jᵀ λ, so with f the other forces

    v = A λ + b,    A = J M⁻¹ Jᵀ,    b = J (u + h M⁻¹ f)       (eq. 45, 227)

The normal part must satisfy

    v_n ≥ 0,   λ_n ≥ 0,   v_n λ_n = 0

and friction must lie in a set that grows with λ_n; when the contact
slides, it must be the member of the set that takes out most energy
(eq. 24, 26). Coulomb's set is the cone ‖λ_t‖ ≤ μ λ_n (eq. 23). The
notes' table 1 gives coefficients, for example 0.42 for dry steel sliding
on steel and 0.05 for ice on wood.

Two sets are in the demo.

**Box** (section 1.7): each tangent by itself, −μ λ_n ≤ λ_t ≤ μ λ_n
(eq. 40). This is a pyramid that holds the cone, so on the diagonal it
gives √2 times the friction. With it the problem is a boxed linear
complementarity problem (eq. 41).

**Ellipse** (section 3.4.1): with a third row for the spin about the
normal,

    (λ_t / a)² + (λ_b / b)² + (λ_τ / c)² ≤ 1,
    a = μ_t λ_n,   b = μ_b λ_n,   c = μ_τ λ_n                   (eq. 220, 221)

Equal μ_t and μ_b give Coulomb's cone; different ones friction that
depends on the direction; μ_τ, a length, friction against spinning.

## 3. One fixed point for both (section 3.4.1)

Both conditions are fixed points of a projection, for any r > 0
(eq. 202, 214, 215):

    λ_n = max(0, λ_n − r v_n)
    λ_f = closest point of the set to (λ_f − r v_f)

Figure 28 of the notes shows why the second line is the principle of
maximum dissipation. r does not change the solutions, only how fast
iterating converges.

The closest point of an ellipsoid has no closed form. Algorithm 13
scales the problem into the unit cube, brackets the root of

    g(t) = Σ a_i² z_i² / (a_i² + t)² − 1

growing the bracket by 1.5, and bisects; the notes report that
Newton–Raphson fails here.

## 4. The solvers (sections 3.2, 3.4.2, 3.4.4)

**Jacobi** (algorithm 11, eq. 228): every contact from the last iterate;
the friction set uses the old λ_n. One r for all.

**Gauss–Seidel** (algorithm 12, eq. 229): each contact uses the newest
values, and its friction set the new λ_n. A is never formed:
w = M⁻¹ Jᵀ λ is kept for each body and updated after each contact
(eq. 230). With the box and r = 1/A_ii this is projected Gauss–Seidel,

    λ_i ← clamp(λ_i − (A λ + b)_i / A_ii)                       (eq. 147, 158)

and with r = ω/A_ii projected SOR. The notes say there is no proof of
convergence for general A.

**NNCG** (algorithm 10, section 3.2.7): what a Gauss–Seidel sweep changes
is taken as minus a gradient; β = ‖∇f_k‖² / ‖∇f_{k−1}‖²; β > 1 starts
again, else λ += β p and p = β p − ∇f_k. The notes say it handles large
mass ratios better than projected Gauss–Seidel.

**The rollback rule** (section 3.4.4): if an iteration's residual is
larger than the last one's, undo it and multiply every r by ν; the notes
give ν = 0.5 for one global r and 0.9 for one r a row.

**Order** (section 3.2.3): the order in which a sweep takes the contacts
changes the speed. For a stack of two boxes with 8 constraints the notes
quote a best order that needs less than half the iterations of the worst.

## 5. The step (sections 1.9, 3.4.5, 3.4.6)

**Moreau's midpoint** (section 3.4.5): positions go half a step with the
old velocities (eq. 246); contacts, J and the impulses are found there;
the new velocities are u + h M⁻¹ f + M⁻¹ Jᵀ λ; positions go the other
half with them.

**Overlap.** Section 1.9 adds a share of the overlap to the velocity the
impulses must reach (the error reduction parameter, eq. 59). Section
3.4.6 instead moves the bodies after the step: displacements d ≥ 0 along
the normals with

    A d + gap ≥ 0,   d ≥ 0,   dᵀ (A d + gap) = 0               (eq. 254)

solved by the same projection (eq. 255), and positions moved by
M⁻¹ Jᵀ d (eq. 256). The notes say this is a linearisation and may need
several steps.

## 6. Finding contacts (section 2.1)

*Sphere–sphere*: gap = ‖c₂ − c₁‖ − r₁ − r₂, normal along the centres.

*Sphere–box*: the centre in the box's frame is clamped to the box
(eq. 85); outside, the gap is the distance to that point less the radius;
inside, the notes take the depth to the nearest face with that face's
normal.

*Box–box*: 15 candidate separating axes (three face normals of each box,
nine cross products of edges). The axis with the least overlap decides:
an edge pair gives one contact at the closest points of the two edges; a
face gives up to 8, by cutting the other box's most opposed face against
the four sides of the reference face (Sutherland–Hodgman) and keeping the
vertices below it.

## 7. The demo

`demos/contact/method.c` is the method in C with doubles;
`demos/contact/main.c` draws through the 3-D canvas.

| scene | what it shows |
|---|---|
| 0 | a block of 1600 kg on five boxes of 32 kg, three copies: Jacobi, Gauss–Seidel, NNCG at the same iterations a step; a board with each solver's residual over 200 iterations on one contact problem |
| 1 | twelve pucks at 2.5 m/s on three grounds: box, cone, ellipse (μ 0.25 along x, 0.75 along z); white rings where they should stop |
| 2 | a ramp of 20° with boxes of μ 0.2, 0.3, 0.5 and a ball; three balls spinning at 30 rad/s with μ_τ 0, 5 mm, 2 cm |
| 3 | boxes and balls dropped into a pit |

Defaults: h = 1/60 s, 40 iterations, ω = 1, contacts made when the gap is
below 1 cm, 20 % of an overlap beyond 2 mm removed a step by 10 sweeps of
section 3.4.6.

## 8. What the check measures (140 assertions)

**Contacts.** 400 spheres against a sphere and a box: gap and normal to
1e-15; the contact point is on the box and none of 10086 points on the
box is nearer the centre. 3000 random pairs of boxes: 2429 overlap; none
of the 571 others has an edge inside the other box; none deeper than
2 cm is missed by the edge test; moved apart along the normal by the
reported overlap, no pair overlaps any more; every contact point is
within the overlap of both boxes; 822 pairs have one contact, none more
than 8. A box flat on a wider one gives its four corners; turned by 45°
on one of its own size, eight.

**Ellipsoid.** 300 points against a search over the surface: 4e-13;
points inside stay; with one half axis 0 the result is in the ellipse.

**Contact conditions.** 12 bodies in the pit after 2 s, every third one
pushed and spun so that 15 to 18 contacts slide; each solver iterated to
a change of 1e-13 or 20000 iterations; then, from the bodies' velocities
alone:

| | box | ellipse |
|---|---|---|
| iterations: Jacobi / Gauss–Seidel / NNCG | 3138 / 59 / 49 | 20000 each |
| fastest a contact closes (m/s) | 2e-12 | 4e-11 |
| λ_n × opening speed | 2e-11 | 1e-10 |
| friction outside its set | 2e-17 | 1e-16 |
| sliding: friction less the one that takes out most energy, over λ_n | 7e-15 | 2e-16 |

**Friction against closed forms** (40 iterations a step).

| | measured | closed form |
|---|---|---|
| puck on the cone: way, any direction | 0.6373 m | v²/2μg = 0.6371 |
| puck on the box: way along an axis / at 30° | 0.6373 / 0.5039 m | each axis by itself |
| puck at 45°: box / cone | 0.7073 | 1/√2 |
| puck on the ellipse: along x / along z | 1.2743 / 0.4250 m | 1.2742 / 0.4247 |
| 36 pucks: farthest from where it should stop | 0.2 / 0.2 / 6.7 mm | 0 |
| ramp, μ 0.2 / 0.3 / 0.5: way in 1 s | 0.7558 / 0.2949 / 0 m | g (sin θ − μ cos θ) t²/2 |
| ball on the ramp | 1.1983 m | (5/7) g sin θ t²/2 |
| sliding ball: final speed / first speed | 0.714286 | 5/7 |
| spinning ball, μ_τ 5 mm / 2 cm: spin after 1 s | 24.55 / 8.20 rad/s | 30 − μ_τ g / (2r²/5) |

The ellipse's 6.7 mm fall to 1.7 mm with a step of 1/240 s: the path is
curved and the step is of first order.

**Solvers.**

* Gauss–Seidel on the box with r = 1/A_ii against eq. 147 and 158 written
  out on the matrix A, 25 sweeps on a tower of three: 2e-15.
* r = 0.5, 1 and 1.5 / A_ii, both sets, 3000 iterations: the bodies'
  velocities agree to 4e-13.
* Heavy tower, residual from no impulses, as a share of the heavy block's
  weight for a step:

  | iterations | Jacobi | Gauss–Seidel | NNCG |
  |---|---|---|---|
  | 10 | 2.1e-3 | 7.3e-4 | 6.6e-4 |
  | 40 | 1.2e-3 | 5.9e-4 | 3.4e-4 |
  | 200 | 7.5e-4 | 4.5e-4 | 1.3e-4 |

* The same tower stepped for 10 s at 40 iterations: the top box ends at
  0.08 m with Jacobi, 0.20 m with Gauss–Seidel (both fell) and 1.09 m
  with NNCG (1.1 m at the start).
* Warm start, tower of six at rest, Gauss–Seidel, 10 iterations: residual
  0.036 of a box's weight from no impulses, 5e-14 of that from the last
  step's.
* Rollback rule, Jacobi with r four times too large on a tower of three:
  9e50 without, 1.7e-4 with (2 iterations undone).
* Rollback rule, Gauss–Seidel on the pit with 166 contacts, 1000
  iterations: 7.7e-4 without, 8.9e-2 with (178 undone).
* Order, two boxes, 8 contacts, 500 random orders to a relative change of
  1e-8: 65 iterations at best, 101 at worst, 85 in the order found.
* A box 2 cm in the ground: section 3.4.6 brings it to the 2 mm that are
  left alone and it never rises faster than 3e-7 m/s; section 1.9 brings
  it there too and it rises at up to 0.22 m/s.

## 9. Agreement with the notes, and where the demo says less

| the notes | here |
|---|---|
| the box holds the cone (section 1.7) | on the diagonal a puck goes 0.7073 of the cone's way |
| r does not change the solution (section 3.4.4) | velocities agree to 4e-13 |
| projected Gauss–Seidel is the fixed point with the box and r = 1/A_ii | 2e-15 |
| NNCG handles large mass ratios better (section 3.2.7) | residual 0.58 of Gauss–Seidel's after 40 iterations, 0.30 after 200; the tower stands only with NNCG |
| rolling back and shrinking r cures divergence (section 3.4.4) | yes when r is too large; where r was right it stalls the solver (8.9e-2 against 7.7e-4) |
| best order needs less than half the worst's iterations (section 3.2.3) | 65 against 101 over 500 random orders, not all 40320 |
| post-step projection removes overlap (section 3.4.6) | yes, and without the speed that section 1.9's term gives |

None of the three solvers is near a solution of the heavy tower in 40
iterations from no impulses (3e-4 of the heavy weight at best). The tower
stands with NNCG because each step starts from the last step's impulses.

## 10. The demo's own choices

* The first tangent is body A's x axis laid into the contact plane, so
  that direction-dependent friction has a meaning; a contact's
  coefficients are the smaller of the two bodies'.
* The spin row is multiplied by μ_τ, so that its impulse is an impulse
  like the others and the ellipsoid's third half axis is λ_n.
* The friction rows of an ellipse share one r = ω / (sum of their A_ii);
  different r inside the block would change the fixed point.
* Jacobi's r is ω over the largest eigenvalue of A, from 30 steps of the
  power method.
* A contact is made when the gap is below 1 cm and may then close the gap
  within the step (b gets gap / h).
* Each step starts from the impulses of the last step's nearest contact
  of the same pair within 2 cm. The notes say only that the previous step
  is a good first guess.
* 2 mm of overlap are left alone, so that resting contacts persist.
* A sphere whose centre is inside a box has the gap −(depth) − radius,
  so that the gap is continuous with the shallow case; the notes give
  −(depth).
* An edge pair is chosen only if its axis is shallower than the best
  face's by 5 % and 0.1 mm, so that a box lying flat keeps its face
  contacts.
* The gyroscopic term is explicit.

## 11. Not implemented, and not read

Not built: the cone complementarity model (section 1.8); pivoting
methods (3.1); non-smooth Newton methods (3.3); blocked Gauss–Seidel and
shock propagation (3.2.4); contacts of meshes with continuous collision
detection and signed distance fields (2.2, 2.3); soft bodies (4, 5); the
friction model of chapter 6.

Not read for this demo: sections 1.4, 1.6, 1.10 beyond its first part,
2.2, 2.3, 3.1, 3.3, chapter 4 beyond its first page, chapters 5 and 6.
The course site links programming examples; they were not looked at, and
nothing here was compared with them.
