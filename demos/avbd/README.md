# avbd — Augmented Vertex Block Descent

```
(method.c on the CPU, 1 step of 10 iterations) ─► Draw ─► Present
```

Rigid boxes in the plane with contacts, friction, hard joints and
springs, after Giles, Diaz and Yuksel, *Augmented Vertex Block Descent*
(SIGGRAPH 2025), following the authors' 2-D reference demo.

* `--scene 0`: a pyramid of 105 boxes.
* `--scene 1`: a rope of 15 links on hard joints carrying a box 200 times a link's
  mass.
* `--scene 2`: the paper's figure 2. A weak spring, a block, a spring
  10 000 times stiffer, a block. AVBD on the left, plain VBD on the
  right, which sags. Try `--iterations 5`.
* `--scene 3`: boxes with friction 0.1 to 0.7 on a slope whose tangent is
  0.365. Those above it hold, those below slide.

`--vbd` runs plain vertex block descent (no multipliers, no stiffness
ramp; hard constraints become springs of 1e6). `--no-post` uses
α = 0.99 instead of the reference demo's post-stabilisation (the paper's
α is 0.95).
White dots are contacts that hold, red ones slide.

The paper is explained in [`docs/avbd.md`](../../docs/avbd.md).

## The method

Each iteration has two halves.

**Primal**: every body in turn moves to where its own energy is least,
with the others held: `H Δx = f`, a 3 × 3 system (x, y, angle).

```
f = −M (x − y) / Δt² − Σ J clamp(k C + λ, fmin, fmax)
H =  M / Δt²         + Σ (k J Jᵀ + diag(|columns of the 2nd derivative| × |force|))
```

**Dual**: every hard constraint takes the force into its multiplier,
`λ ← clamp(k C + λ)`, and every constraint that is inside its bounds
gets stiffer, `k ← k + β |C|`, up to its real stiffness if it has one.

A contact pushes only (`fmax = 0`), and its friction row is bounded by
μ times the normal force found so far. k and λ are kept for the next
step, scaled by γ.

## Acceptance (`--check`, under a second)

| check | measured | rule |
|---|---|---|
| contact routine: resting box, box on a corner, box just above | 2, 1, 0 points; positions to 3e-16 | exact |
| gradients, second derivatives against differences | 3.7e-10, 9.7e-11 | < 1e-8 |
| free fall against implicit Euler's formula | −4e-12 m | < 1e-11 |
| two springs, ratio 10 000, 5 iterations: weak spring at rest / (weight / stiffness) − 1 | 0.0014 | < 0.01 |
| its longest stretch on the way (0.01 m at rest): AVBD 5 iterations, 1 iteration | 0.0155, 0.0209 m | < 0.02, < 0.03 |
| the same, plain VBD: 5, 100 iterations | 2.30, 0.43 m | > 0.5, reported |
| plain VBD, 1000 iterations: at rest / right − 1 | 1.2e-4 | < 0.01 |
| rope, 200 : 1, hanging 10 s: widest joint gap, AVBD 10 / 30 iterations | 5.2, 2.4 mm | < 10, < 5 |
| plain VBD with stiffness 1e6 | 1.05 mm = weight / stiffness | reported |
| force in the top joint / weight below − 1 | 3.1e-4 | < 1e-3 |
| rope swinging 10 s: widest gap, AVBD 10, 30; VBD 10 | 23, 25, 31 mm | < 50, reported |
| stack of 10 boxes, 10 s: fastest box; top box height − 9.5 | 3e-15 m/s; −5.4 mm | < 1e-3; < 10 mm |
| slope, μ 0.5 > tan: moved in 1 s | 5e-9 m | < 1e-3 |
| μ 0.2 < tan: distance / Coulomb's law − 1 | 5.2e-6 | < 0.03 |
| box starting 0.3 m in the ground, fastest speed out: α 0, α 0.95, post-stabilisation | 18, 0.90, 5e-5 m/s | reported, < 1/5, < 1e-3 |
| pyramid after 5 s: fastest box | 1.6e-4 m/s | < 0.01 |

## What is not done

* Two dimensions, boxes only, on one CPU thread. The paper's solver is
  3-D on a GPU with graph colouring and a bounding volume hierarchy;
  here pairs are found by trying all of them.
* No soft bodies or cloth, no motors, no joints that break, no static
  and dynamic friction coefficients, no stiffness rescaling of eq. 14
  (the reference demo does not use it either).
* On the rope, plain VBD with a stiffness of 1e6 holds the joints as
  well as AVBD does. AVBD's gain in these tests is with mixed
  stiffnesses, bounded forces and stacking, not there.
