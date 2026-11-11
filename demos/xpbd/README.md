# xpbd — position-based dynamics with compliance

```
(method.c on the CPU, 2 steps) ─► Draw ─► Present
```

Cloth, chains and a soft block by XPBD, after Macklin, Müller and
Chentanez, *XPBD: Position-Based Simulation of Compliant Constrained
Dynamics* (Motion in Games 2016). `--pbd` runs the 2007 method the paper
improves on, in the same code, so the two can be compared.

* `--scene 0`: a 30 × 30 cloth pinned at two corners falls over a ball.
* `--scene 1`: three chains carry the same weight with a quarter of, the
  same, and four times `--iterations`. With XPBD they hang alike. With
  `--pbd` each has its own length.
* `--scene 2`: a block of tetrahedra with edge springs and volume
  constraints falls on the floor.

`--stiffness`, `--bend`, `--bulk` are log10 of the stiffnesses,
`--damping` is the paper's β, `--pbd-stiffness` is PBD's k in 0..1.

The paper is explained in [`docs/xpbd.md`](../../docs/xpbd.md).

## The method

A constraint `C(x) = 0` with compliance α (one over stiffness) stands for
the energy `C² / 2α`. Each constraint keeps a multiplier λ that starts
every time step at zero. One pass over the constraints does, for each,

```
Δλ = (−C − α̃ λ) / (Σ wᵢ |∇ᵢC|² + α̃)        α̃ = α / Δt²
xᵢ += wᵢ ∇ᵢC Δλ          λ += Δλ
```

with wᵢ one over the mass of node i. Without the two α̃ terms this is PBD.
With them the passes settle on implicit Euler for the energy above, and
`λ / Δt²` is the force the constraint carries.

`method.c` has two constraints, a length and the volume of a
tetrahedron, the damping of the paper's section 5, contacts with a floor
and a ball, and a reference: the same time step by Newton's method on
the implicit Euler equations, solved to rounding.

## Acceptance (`--check`, lavapipe, 11 s)

| check | measured | rule |
|---|---|---|
| gradient of a length / a volume against differences | 8.6e-11 / 3.3e-11 | < 1e-8 |
| one spring at rest, 1 / 10 / 100 iterations × Δt 1/60, 1/240: stretch / (α m g) − 1, worst | 8.7e-10 | < 1e-6 |
| the same six runs with PBD, k = 0.5: largest stretch (19.62 mm is right) | 2.7 mm | reported |
| chain of 15 links at rest: length / (rest + α × weight below) − 1, 5 iterations | 0.0105 | reported |
| 20 iterations | 1.4e-6 | < 1e-4 |
| 80 and 320 iterations | 4.1e-12 | < 1e-9 |
| PBD, k = 0.5: stretch / right stretch, 5 and 320 iterations | 0.77, 0.010 | reported, < 0.05 |
| force in the top link from λ / Δt² against the weight below − 1 | 2.0e-11 | < 1e-8 |
| compliance 0 against PBD with k = 1, 240 steps | 0 | = 0 |
| swinging chain after 1 s against Newton: 20 / 200 / 2000 iterations | 1.318 / 1.316 / 1.316 mm | < 5 mm |
| cloth after 5 s: deepest node inside the ball | 0.006 mm | < 1 mm |
| block, bulk 1e9 Pa, 5 s: volume / rest − 1 | 2.0e-5 | < 1e-3 |

## What is not done

* The constraints are solved one after another on the CPU. The paper's
  3-D results use a Jacobi solver on a GPU.
* No finite-element constraints (the paper's cantilever beam, section
  6.3), no rigid bodies, no self-collision of the cloth.
* Contacts are the floor and one ball, with zero compliance and a simple
  friction rule. The paper says only that contacts are handled as in PBD.
