# Low-Rank Koopman Deformables — the paper and `demos/koopman`

Yue Chang, Peter Yichen Chen, Eitan Grinspun, Maurizio M. Chiaramonte:
*Low-Rank Koopman Deformables with Log-Linear Time Integration*,
arXiv 2602.07687 (version 2, June 2026).
Paper: <https://arxiv.org/abs/2602.07687>

Section, equation, figure and table numbers below are the paper's.

## 1. The problem

A soft body is simulated by solving a non-linear system every time step.
Reduced models make the system small but still solve it step by step, so
the cost grows with the number of steps and a large step changes the
motion (implicit Euler damps it).

The paper fits one linear operator to the motion: next state = K × state.
A linear operator can be diagonalised. Then N steps are the N-th power of
its eigenvalues, which costs the same as one step. The price is that the
operator is fitted to data and only knows the motions it has seen.

## 2. Dynamic mode decomposition (section 3)

Run the simulator and collect states. X has the states X_0 … X_{T−1} as
columns, X′ the states one step later, X_1 … X_T. The matrix that best
maps one to the other is K = X′ X⁺ (eq. 11), which is too large to form.
With the singular value decomposition cut to rank r,

    X ≈ U_r Σ_r V_rᵀ                        (eq. 12)
    K′ = U_rᵀ X′ V_r Σ_r⁻¹      r × r        (eq. 13)
    K′ = φ Λ φ⁻¹                             (eq. 14)
    Φ = U_r φ                                (eq. 15)
    K ≈ Φ Λ Φ*                               (eq. 16)

Λ is diagonal with r eigenvalues, Φ has r modes as columns. K′ is real
but not symmetric, so eigenvalues and modes are complex and come in
conjugate pairs. A pair is one damped oscillation: the modulus of λ is
what is left of it after a step, the angle is how far it turns.

## 3. The state and forces (sections 4.1, 4.2)

A step is X_{t+1} = Φ Λ Φ* X_t (eq. 17): project, multiply r numbers,
lift.

The state is the displacement of every node together with its change
over the last step, X_t = [U_t; U_t − U_{t−1}], 6 numbers a node
(eq. 18). Displacement alone does not say which way the body is moving;
the paper's figure 4 shows that a fit to it is unstable.

A force is added to the state before the step, as an impulse in the
second half only: X_t⁺ = X_t + [0; f h²], then X_{t+1} ≈ K X_t⁺ (eq. 19).
The paper calls this a special case of a controlled update (DMD with
control). So one operator fitted to a few forces can be driven by others
(figure 5).

## 4. Many steps at once (section 4.3)

    X_{t+N} = Φ Λ^N Φ* X_t                   (eq. 20)

The power of a diagonal matrix is r powers of numbers; computing λ^N
takes time logarithmic in N at most. A step of another size h′ is

    Λ(h′) = exp((h′/h) log Λ(h))             (eq. 21)

## 5. The neural variant and the real form (section 5)

Section 5 replaces the fixed modes by a network that gives the modes at
any point of a shape, and the eigenvalues by a second network, trained
with losses (eq. 22 to 24) that also ask for |λ_i| ≤ 1. That lets one
model cover a family of shapes. **The demo does not build this.**

Section 5.2 notes that the complex form "is benign when applying the
operator one step at a time but can lead to bigger error when
exponentiating the operator for long-horizon or large-step integration,
as small imaginary components accumulate over time". Its remedy writes
Φ = A + iB and Λ = C + iD as real matrices of twice the size and drops
the imaginary part of the state inside the operator:

    Φ_R = [A −B; B A],  Λ_R = [C −D; D C],  P = [I 0; 0 0]
    K_real = Φ_R* P Φ_R Λ_R        2r × 2r

## 6. What the paper measures (section 6)

Data come from a full simulator with the stable Neo-Hookean energy.
Table 1, the numerical examples: 25 to 50 modes, 10k to 15k vertices, 73
to 720 snapshots, 0.63 to 10.74 s to fit. Table 2: 2 to 8 ms a step
against 334 to 3018 ms for the full simulation.

* 6.1, figure 2: with h = 0.04 instead of 0.004 implicit Euler damps the
  motion strongly; the operator's motion barely depends on the step.
* 6.2, figure 9: the one-step error falls fast with r and then moves
  little, not always downwards (the paper names a bias–variance
  trade-off); the cost stays within milliseconds.
* 6.3: error in per cent of the mean square against the full simulation.
  Mushroom (figure 5), trained on one force and tested on half and twice
  of it: 0.048 %, 0.065 % and 0.416 % over the three runs. Armadillo
  (figure 10), with 2/3 and 4/3 of the force: 5.57 %, 0.07 %, 3.55 %.
  Direction of the force (figure 11): 0.86 % and 0.85 % on the two
  directions of the data, 0.45 % on one between.

## 7. Uses (section 7)

* 7.1: interactive use; the step size can change while the user pulls.
* 7.2: more damping without a new fit, Λ̃ = (1 − μ) Λ, shown for 1 % and
  2 % (figure 14).
* 7.3: a pneumatic finger with three chambers. Pressure gives the force
  F = A(X) C. The motion is quasi-static, so the end state is a sum over
  many steps, X_final ≈ Φ (Σ_{t=1..N} Λ^t) Φ* A(X) C (eq. 25), and the
  sum of powers has a closed form. The three pressures are found by
  non-negative least squares so that one vertex ends at a goal, with 5
  iterations because A depends on X. The error against the full
  simulation is under 2 % for most frames and under 6 % for all
  (figure 16).
* 7.4: shape optimisation by finite differences. 7.5: learning from
  video.

## 8. Limits the paper names (section 8)

The model is data-driven and "cannot reliably extrapolate beyond the
range of dynamics it has observed". Material, boundary conditions and
integrator are fixed by the data. More modes do not always help. Strong
non-linear deformation and rotation degrade it mildly. There is no
contact.

## 9. What the demo builds

`method.c`, plain C, about 640 lines:

* **The simulator** that makes the data and is the reference:
  tetrahedra with the stable Neo-Hookean energy, lumped mass, damping
  proportional to mass, implicit Euler by Newton's method with a line
  search. Each Newton step is solved by conjugate gradients, with the
  Cholesky factor of the matrix at rest (banded) as the preconditioner.
* **The fit** (eq. 12 to 15): Householder QR of X, one-sided Jacobi on
  R for the singular values, the r × r matrix K′, its eigenvalues by
  Hessenberg form and shifted QR, eigenvectors by back substitution in
  the triangle that leaves. No library.
* **The operator**: project, advance by any number of steps, whole or
  not (eq. 20, 21), with or without a kick before every step (eq. 19 and
  the closed sum of eq. 25), lift. The real operator of section 5.2.
* **Control** (section 7.3) by least squares.

The body is a finger 0.6 m high, 0.15 m wide at its base and narrower at
the top, 208 nodes, 1296 tetrahedra, E = 4·10⁵ Pa, ν = 0.4, 1000 kg/m³,
held at its base. The state has 1248 numbers. The data are three runs
with a step of 4 ms: a push of 5 m/s² along x for 125 steps and 125 free,
the same along z, and 10 m/s² upwards for 60 steps and 60 free. Every
third step is a pair of the fit: 208 pairs. A pushed step enters as the
pair (X_t + kick, X_{t+1}), so K is the step without force.

For implicit Euler the kick of eq. 19 is exact, not an approximation:
the step sees the velocity it starts with only as h·v, and a constant
acceleration a over the step turns that into h·v + h²·a.

## 10. Measured (`--check`, 137 assertions)

All on this finger; errors as in section 6.3 against implicit Euler with
the 4 ms step.

| claim of the paper | measured here |
|---|---|
| figure 2: implicit Euler with 10 × the step damps strongly, the operator does not | kinetic energy kept over 1.2 s: 0.100 (4 ms), 0.098 (operator, 10 steps at once), 0.0007 (40 ms) |
| eq. 20: N steps at once | equal to N single steps to 1.4e-15; error against the 4 ms run 0.19 % either way |
| eq. 21: other step sizes | steps of 2.5 × 4 ms: 0.19 % |
| figure 4: displacement alone is not enough | 80 % against 0.008 % with both halves |
| figure 9: error falls fast with r | one step: 0.28 % at r 4, 0.0014 % at r 20, 1e-6 % at r 60 |
| figure 9: then no clear gain | 1000 steps: 42 % (4), 1.9 % (8), 0.13 % (12), 0.29 % (16), 0.10 % (20), 0.17 % (30), 0.13 % (40), 0.11 % (60) |
| figure 5: other strengths of the force | half: 0.10 %; twice: 0.68 % |
| figure 11: other directions | diagonal: 0.30 %; the two of the data: 0.042 % |
| section 7.2: Λ (1 − μ) is more damping | × 0.99 follows a body with 5/s more damping to 0.91 %, × 0.98 one with 10/s more to 4.3 % |
| section 7.3: goals reached through the operator | tip within 0.4 to 1.4 mm of goals 8 to 12 cm away after 1.28 s, at 0.007 to 0.022 m/s |
| figure 16: under 2 % for most frames, under 6 % for all | 77 % of the steps under 2 %, mean 1.4 %, **worst 17.7 %** |
| section 5.2: the real operator | equal to Λ^N on states that came from real ones, to 4e-12 |
| table 2: much faster than the full simulation | 15 µs for 10 steps with the lift against 32 ms (about 2100 ×), on 208 nodes |

Where this differs and why:

* **The worst control step is 17.7 %, not under 6 %.** The paper's
  finger is moved slowly. This one is thrown: the controller asks for up
  to 15 m/s², three times the push of the data, and the goals change
  abruptly. Right after a change the finger is far from anything in the
  data. The tip still arrives because the pushes are found again every
  40 steps from where the operator's finger is.
* **Twice the push errs by 0.68 %** and this does not improve with the
  rank. A linear operator cannot hold the stiffening of the material;
  the paper names this limit.
* **Eigenvalues outside the unit circle.** With the demo's data there
  are none at any rank. Fitted to a single pushed run of 250 steps there
  are one or two at ranks 12 to 30 (up to 1.005) and the 1000-step
  rollout errs by 5.0 % at rank 12 and 6.1 % at rank 30; with those
  eigenvalues put on the circle, 1.9 % and 0.12 %. The demo keeps that
  clamp (`--unclamped` removes it). It is an addition: the numerical
  part of the paper does not have it.
* **The imaginary part** that section 5.2 warns about stays at 1e-14 of
  the displacement here over whole steps, so the real operator makes no
  difference to the pictures. It matters for fractional steps of
  negative real eigenvalues; the fit has none.

## 11. Not reproduced

* The neural variant of section 5 and with it everything about families
  of shapes.
* The paper's meshes and timings (10k to 15k vertices); its error
  figures for the mushroom and the armadillo are quoted above, not
  reproduced.
* The pneumatic finger as such: pressures in chambers, a force that
  depends on the state (F = A(X) C) with its 5 iterations, non-negative
  least squares. The demo has two body pushes that do not depend on the
  state, plain least squares, and a goal 60 steps ahead instead of a
  quasi-static end.
* Shape optimisation (7.4) and learning from video (7.5).
* The paper builds on PyDMD; the decomposition here is written out.
