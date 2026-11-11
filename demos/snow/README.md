# snow — the material point method for snow

```
(Clear ─► P2G ─► G2P) × 10 ─► Draw ─► Present
```

Snow as particles that talk through a 64³ grid one metre across, after
Stomakhin, Schroeder, Chai, Teran and Selle, *A material point method for
snow simulation* (SIGGRAPH 2013), with the force derivatives of its
technical document. A snowball of 11 500 particles is thrown at a wall at
10 m/s and packs into a cake.

`--scene 1` is two snowballs meeting in the air, `--scene 2` a slab
falling on a fixed ball, `--scene 3` a ball ploughing through a bed of
snow at 4.7 m/s. `--sticky` makes the side walls and the ceiling hold
whatever touches them (the paper's sticky collision). `--view 1` colours
by speed, `--view 2` by permanent change (blue packed, red torn),
`--view 3` by elastic squeeze. `--implicit` runs the paper's implicit
step on the CPU. `--hardening`, `--compression`, `--stretch`,
`--stiffness`, `--flip` and `--friction` are the paper's material
parameters.

The paper and the technical document are explained at length in
[`docs/snow-mpm.md`](../../docs/snow-mpm.md).

## The method

A particle carries position, velocity, its elastic deformation F_E (a
3 × 3 matrix) and J_P, the volume change it has kept for good. One time
step (0.1 ms) is three compute passes:

1. **Clear** empties the grid.
2. **P2G**: each particle adds mass, momentum and dt × force to the
   4 × 4 × 4 nodes round it with cubic B-spline weights. Its force on
   node i is `−V (∂Ψ/∂F_E) F_Eᵀ ∇w_i` with
   `Ψ = μ |F_E − R_E|² + λ/2 (det F_E − 1)²`; R_E is the rotation nearest
   to F_E, found from the eigenvectors of F_EᵀF_E by Jacobi rotations.
3. **G2P**: a node's velocity is momentum ÷ mass, plus force and gravity,
   then the collision rule, in the frame of the wall or the ball: stick
   if friction can hold it, else slide; a sticky wall holds it always.
   A particle takes 5 % of the new grid velocity and 95 % of its own plus
   the change (PIC and FLIP), and the velocity gradient:
   `F_E ← (1 + dt ∇v) F_E`. The singular values of F_E are then kept
   inside [1 − 0.025, 1 + 0.0075]. What is cut off is permanent and
   multiplies J_P; μ and λ are multiplied by `exp(10 (1 − J_P))`, so
   packed snow is stiffer and torn snow falls apart.

The grid is integers and the adds are atomic, as in `demos/mpm`, so a
run repeats bit for bit. Lengths are cells throughout.

The ball moves between steps, `centre += dt × velocity`, and stops where
the scene says.

`method.c` has the same step in double, the particle volumes of the
first step, and the implicit solve: `(1 + dt² m⁻¹ ∂²Φ/∂x²) v = v*` by
conjugate residuals, where the product with ∂²Φ/∂x² is equations 3 and 4
of the technical document, including the change δR of the rotation.
Nodes whose velocity a collision changed are left out of the solve and
keep that velocity, as in section 8 of the paper.

## Acceptance (`--check`)

| Line | Rule | Result |
|---|---|---|
| after 400 steps: particles that are not numbers / inside a wall | 0 | 0 / 0 |
| fastest particle, cells per time step | < 0.5 | 0.006 |
| singular values of F_E outside [1 − θ_c, 1 + θ_s] by | < 1e-5 | 3e-7 |
| J_P, most packed / most torn | reported | 0.17 / 1.04 |
| R of 200 random F: RᵀR − 1, RS − F | < 1e-12 | 2e-15, 3e-15 |
| **∂Ψ/∂F − differences of Ψ** | < 1e-6 | 1.4e-10 |
| **δ(∂Ψ/∂F) of the technical document, with δR − differences** | < 1e-6 | 2.5e-10 |
| cubic weights: sums, slopes, centre | < 1e-5 | 7e-15 |
| **node force (eq. 2) − differences of the energy (eq. 1)** | < 1e-5 | 7e-10 |
| sum of all node forces ÷ largest | < 1e-6 | 1.5e-14 |
| **force change (eq. 3, 4) − differences of the forces** | < 1e-5 | 2.7e-11 |
| it is symmetric | < 1e-9 | 5e-15 |
| **one step with a moving ball and sticky walls, grid on GPU − CPU: mass / momentum / dt × force** | < 1e-5 / 0.01 / 0.01 | 5e-6 / 2.4e-4 / 1.0e-3 |
| mass on the grid ÷ particles − 1 | < 1e-5 | 1.6e-7 |
| momentum on the grid − of the particles, per particle | < 1e-3 cells/s | 3.0e-5 |
| sum of dt × force, per particle | < 1e-3 cells/s | 1.5e-7 |
| nodes a wall or the ball changed in that step | > 100 | 381 |
| **particles GPU − CPU: position / velocity / F_E / J_P** | < 1e-4 / 0.05 / 1e-5 / 1e-5 | 4e-6 / 2.3e-5 / 7e-7 / 5e-7 |
| **a ball falling 400 steps: distance ÷ g dt² n (n + 1) / 2 − 1** | < 1e-3 | −3.8e-5 |
| sideways drift / change of F_E, J_P | < 1e-3 / < 1e-4 | 1.3e-4 / 3.4e-5 |
| implicit step of 0.5 ms, snow packed on a sticky wall, ball moving in: iterations | ≤ 60 | 19 |
| **residual it reports / done again from eq. 3, 4 over the free nodes** | < 1e-5 | 9.5e-8 / 9.5e-8 |
| nodes held by the wall or the ball / largest change of one in the solve | > 100 / 0 | 383 / 0 |
| **a squeezed elastic ball, steps of 1.5 ms: fastest particle, implicit** | < 40 cells/s | 3.0 |
| explicit, the same step | > 1e5 (blows up) | 1.2e5 |
| 120 steps run twice: bytes that differ | 0 | 0 |
| **collision with a moving ball, 20 000 random velocities: fastest still going into it** | < 1e-9 cells/s | 1.1e-13 |
| speed along it − max(before − friction × speed into it, 0) | < 1e-9 cells/s | 1.7e-13 |
| sticky walls and ceiling: largest speed left | 0 | 0 |
| **a ball pushed 4.5 cells into resting snow: deepest particle inside it** | < 0.25 cells | 0 (none inside) |
| mean velocity of the snow along the push | > 1 cells/s | 44 |

## Read this before trusting it

* **The GPU runs the explicit step.** The implicit solve is on the CPU
  only (`--implicit`, two 0.5 ms steps a frame, about 0.45 s a frame).
  Every iteration needs a pass over the particles and sums over the
  grid, and a solve takes about 20; that does not fit this framework's
  64 passes a frame.
* The explicit step is 0.1 ms, ten times the paper's. The grid is
  coarser than the paper's by about as much.
* The implicit and the explicit step are not checked against each other
  on a whole scene, only on the squeezed ball.
* The hardening factor `exp(ξ (1 − J_P))` is kept between 0.1 and 5. The
  paper does not bound it; without the bound the explicit step would
  need to be far shorter.
* Only J_P is stored, not the matrix F_P: the method uses nothing else
  of it.
* Colliders are the six walls and one ball. The ball can move in a
  straight line; nothing rotates or deforms, and there are no level
  sets of other shapes.
* Every particle has the same material. The paper makes snowballs harder
  outside and adds noise to the stiffness.
* The collision rule changes velocities only. A particle that starts
  inside the ball is not pushed out.
* Rendering is lit discs with a depth buffer. The paper's pictures are
  volume renderings; nothing of that is here. No shadows.
* The wall is not drawn when the camera is behind it.
* lavapipe: 33 ms a step with 11 500 particles; a default frame is 0.3 s.

## Files

`method.h/.c` the step, the derivatives and the implicit solve on the
CPU · `main.c` scenes, passes, checks · `shaders/snow.slang` the three
compute passes · `shaders/draw.slang` particles, floor, wall and ball.
