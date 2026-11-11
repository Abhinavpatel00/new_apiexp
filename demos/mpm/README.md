# mpm — water and jelly with the material point method

```
(Clear ─► P2G ─► G2P) × 18 ─► Draw ─► Present
```

16 384 particles and a 128 × 128 grid in a box one metre across. A block
of water is let go next to a brick of jelly while a second brick falls
from above. The method is the moving least squares material point method
of Hu et al. (SIGGRAPH 2018), in two dimensions, entirely in compute.

`--scene 1` is one brick falling through empty space, `--view 1` colours
by speed, `--view 2` by how squeezed a particle is, `--stiffness` sets the
jelly's Young's modulus over density (300 m²/s² by default).

## The method

A particle carries position, velocity, the velocity gradient C round it
and its deformation F. One time step (0.2 ms) is three passes:

1. **Clear** empties the grid.
2. **P2G**: each particle adds mass and momentum to the 3 × 3 nodes round
   it with quadratic weights. The momentum it gives node i is
   `w (v + (C − 4 dt · stress/density) (x_i − x_p))`: its own motion, the
   motion round it, and what its stress does in this step.
3. **G2P**: a node's velocity is momentum ÷ mass, plus gravity, minus
   whatever moves into a wall. Each particle takes the weighted velocity
   and its gradient back, moves, and deforms: `F ← (1 + dt C) F`.

Stress ÷ density is `K (J − 1)` for water (J = det F; water then forgets
its shape, F ← √J · 1) and `μ (F Fᵀ − 1) + λ ln J` for jelly
(neo-Hookean, Poisson's ratio 0.2). Neither needs a singular value
decomposition.

**The grid is integers.** Many particles add to the same node at the same
time, so P2G uses atomic adds (`InterlockedAdd`) on fixed-point numbers:
mass in units of 2⁻²⁴ particle, momentum in 2⁻¹⁴ particle·cells/s. Integer
adds give the same sum in any order, so a run repeats bit for bit.

Lengths are in cells throughout. The particles are drawn as discs from
the vertex index, without a vertex buffer.

## Acceptance (`--check`)

| Line | Rule | Result |
|---|---|---|
| after 3 600 steps: particles that are not numbers | 0 | 0 |
| deepest particle, cells past the first wall node | < 1 | 0.05 |
| fastest particle, cells per time step | < 0.5 | 0.14 |
| jelly, area now ÷ area at the start, lowest / highest | 0.5–1.5 | 0.91 / 1.06 |
| water, the same | reported | 0.93 / 1.09 |
| **one step: largest difference between a node's mass on the GPU and on the CPU** | < 1e-5 particle | 0 |
| … momentum | < 1e-3 particle·cells/s | 2.4e-4 (4 units of the integer); 1 241 of 49 152 sums differ |
| **mass on the grid ÷ number of particles − 1** | < 1e-5 | 4e-8 |
| **momentum on the grid − momentum of the particles, per particle** | < 1e-3 cells/s | 2.8e-6 / 2.3e-6 |
| **one step, particles against the CPU (from the GPU's grid): velocity / position** | < 1e-3 cells/s / < 1e-5 cells | 0 / 0 |
| **a brick falling for 360 steps: distance ÷ g dt² n (n + 1) / 2 − 1** | < 1e-4 | −1.6e-5 |
| speed ÷ g n dt − 1 | < 1e-4 | −1.7e-5 |
| sideways drift | < 1e-4 cells | 0 |
| largest change of shape, \|F − 1\| | < 1e-3 | 2.9e-6 |
| **90 steps run twice: bytes that differ** | 0 | 0 |

## What the numbers say

- **Particle-to-grid loses nothing**: the grid holds the particles' mass
  to 4e-8 and their momentum to 3e-6 cells/s per particle, with stress
  and C switched on. Internal forces cancel, as they must.
- **GPU and CPU agree on the grid to 4 integer units** in 2.5 % of the
  sums and exactly in the rest. The differences are floats rounding the
  other way in one or the other's arithmetic; from the same grid the two
  give identical particles.
- **A falling brick falls as a point**, 1.6e-5 short over 360 steps, and
  does not change shape (3e-6). With mass in units of 2⁻¹⁶ instead of
  2⁻²⁴ its speed was 1.1e-4 short: a node whose mass rounded to zero
  threw its momentum away. This check found that, and it is why the unit
  is what it is. Where the remaining 1.6e-5 goes was not traced.
- **Runs repeat exactly**, whatever order the atomic adds happen in.
- **Water is not incompressible here**: its area changes by up to 9 %
  where it hits the wall. The method treats water as a stiff gas
  (K = 400 m²/s²).

## Limits

- Two dimensions, one grid size, one time step, no adaptivity.
- The one-step check does not compare C and F after the step, only
  velocity and position.
- No comparison with a measured dam break or an analytic flow: beyond
  free fall and the conservation sums, the motion is checked by eye.
- Water and jelly have the same density, and they stick where they
  touch: on one grid, materials cannot slide past each other or separate
  cleanly. (The same paper's compatible-particle-in-cell extension
  addresses this; it is not implemented.)
- Walls let go of what leaves and stop what arrives; there is no friction.
- The water shows the gaps and clumps particle methods show when
  stretched; nothing reseeds particles.
- The fixed-point grid overflows above 127 particles' mass or 131 072
  particle·cells/s on one node. The run stays far below (its fastest
  particle does 700 cells/s) but nothing checks it.
- Particles are drawn as flat discs: no surface, no lighting.
- Lavapipe only; 200 frames (3 600 steps) take 12 s there.

## Sources

- Y. Hu, Y. Fang, Z. Ge, Z. Qu, Y. Zhu, A. Pradhana, C. Jiang, *A moving
  least squares material point method with displacement discontinuity
  and two-way rigid body coupling*, ACM Trans. Graph. 37(4), SIGGRAPH
  2018 — the P2G momentum formula used here. Its discontinuity and rigid
  body parts are not implemented.
- C. Jiang, C. Schroeder, A. Selle, J. Teran, A. Stomakhin, *The affine
  particle-in-cell method*, SIGGRAPH 2015 — the matrix C.
- A. Stomakhin, C. Schroeder, L. Chai, J. Teran, A. Selle, *A material
  point method for snow simulation*, SIGGRAPH 2013 — MPM in graphics.
- The step follows the structure of Yuanming Hu's 88-line Taichi example
  (`mpm88`), from which the water model, wall rule, time step and
  K = 400 are taken. The fixed-point grid and the jelly are this demo's.
