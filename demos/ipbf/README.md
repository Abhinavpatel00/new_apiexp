# ipbf — a fluid kept at its rest density by one Newton step a particle

```
(method.c on the CPU: neighbours, densities, a 2x2 solve for each particle) ─► Draw ─► Present
```

Diaz, Hsu, Montalvo-Ruiz, Giles, Yuksel: *Implicit Position-Based Fluids*
(SIGGRAPH Asia 2025), Algorithm 1, for particles in the plane, with
*Position Based Fluids* (Macklin and Müller 2013) beside it.

* `--scene 0`: two columns of 800 particles fall and meet (after the
  paper's figure 2).
* `--scene 1`: 1024 particles packed to seven times the rest density
  (after figure 5).
* `--scene 2`: a column at rest (after figure 7).

Particles are blue at the rest density and turn red towards 25 % over it.

| option | |
|---|---|
| `--method 0..1` | 0 implicit position-based fluids, 1 position based fluids |
| `--iterations N` | iterations in a time step (4) |
| `--rate N` | time steps a second, 30 to 960 (240) |
| `--compliance X` | the paper's alpha, rescaled (see `docs/ipbf.md`, section 3); 0 is incompressible |
| `--no-damping` | without the damping of section 3.6 |
| `--exact` | the exact second-order term instead of the approximation of section 3.5 |
| `--active` | leave clamped constraints out of the Hessian (see below) |

## The method

A step minimises `alpha / (2 h²) |x − y|² + ½ Σ C_i²` with
`C_i = max(density_i / rest − 1, 0)` (eq. 7). Every particle takes a
Newton step on that energy with the others held, all at once, and half of
it is applied (eq. 9 to 14, section 3.3). The part of the Hessian that can
be indefinite is replaced by a diagonal (section 3.5). With `alpha = 0`
only the density term is left. `docs/ipbf.md` goes through it.

## What `--check` measures

132 assertions, about 8 s. The numbers below are from that output.

* Kernel: integral 1, both derivatives agree with differences; a box full
  of the lattice has the rest density to 3e-15.
* Force and Hessian (eq. 10, 11, 15) agree with differences of eq. 7 to
  3e-8 and 3e-10.
* At seven times the rest density 352 of 1024 particles have an indefinite
  exact Hessian; with the approximation the smallest eigenvalue is positive.
* One step from a packed block, energy of eq. 7 over the energy before:

  | iterations | 1 | 2 | 4 | 8 | 16 | 32 |
  |---|---|---|---|---|---|---|
  | IPBF, alpha 0 | 0.73 | 0.58 | 0.37 | 0.16 | 0.026 | 3.5e-4 |
  | IPBF, compliance 0.5 | 0.78 | 0.69 | 0.60 | 0.557 | 0.5497 | 0.5495 |
  | PBF | 0.63 | 0.60 | 0.76 | 0.081 | 1.2e-3 | 1.5e-5 |

* A column at rest, fourth second, 1/240 s: mean density over rest with 2,
  4, 8 iterations is 0.45 %, 0.21 %, 0.092 % for IPBF and 0.43 %, 0.21 %,
  0.094 % for PBF. Without the damping the fastest particle moves at
  3.3 m/s instead of 0.15. With `--active` the column swells to 1.49 times
  its height.
* Two columns, 2 iterations, 3 s:

  | step | IPBF error | IPBF fastest | PBF error | PBF fastest | PBF height at 3 s / level |
  |---|---|---|---|---|---|
  | 1/240 s | 0.47 % | 5.5 m/s | 0.44 % | 6.6 m/s | 1.00 |
  | 1/120 s | 1.9 % | 5.0 | 1.7 % | 6.5 | 0.98 |
  | 1/60 s | 8.7 % | 7.1 | 1.1 % | 15.3 | 2.03 |
  | 1/30 s | 33 % | 7.0 | 6.1 % | 11.4 | 1.66 |

  IPBF's height at 3 s is 0.99, 0.98, 0.91, 0.81 of the level surface.
* The block at seven times the rest density, 20 steps of 1/120 s with 3
  iterations: fastest particle 21 m/s with IPBF, 59 m/s with PBF, 407 m/s
  with the exact second-order term.

## What this says

* IPBF stays calm at every step tried and pays in compression: at 1/30 s
  with 2 iterations the fluid is a third over its rest density. The paper
  reports the same kind of result (stable, "noticeable compression").
* PBF as written here has less compression at the large steps but throws
  the fluid about: at 1/60 s and 1/30 s it has not settled after 3 s (its
  mean height is 2.0 and 1.7 times that of a level surface).
* At 1/240 s the two methods have the same density error here. The
  paper's table 1 has IPBF several times better than PBF; that is not
  reproduced.
* The approximation of section 3.5 is needed (407 m/s without it), and so
  is the damping when alpha is 0, as the paper says.

## Not done

Three dimensions, the GPU, the paper's particle counts, its viscosity, and
its comparisons with IISPH, SISPH and DFSPH. The walls are this demo's own.
PBF is without its tensile correction, vorticity confinement and viscosity.
