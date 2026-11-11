# projdyn — Projective Dynamics

```
(method.c on the CPU, 1 step of 10 local/global iterations) ─► Draw ─► Present
```

An elastic bar of 1536 tetrahedra by the local/global solver of Bouaziz,
Martin, Liu, Kavan and Pauly, *Projective Dynamics: Fusing Constraint
Projections for Fast Simulation* (SIGGRAPH 2014).

* `--scene 0`: held at one end, it swings down.
* `--scene 1`: held at both ends and twisted a full turn.
* `--scene 2`: pulled to 1.6 times its length.

`--stiffness` is log10 of the strain weight in Pa, `--limit 0.15` lets
singular values within 1 ± 0.15 cost nothing (strain limiting),
`--volume 6.5` adds volume constraints, `--iterations` is the number of
local/global rounds a time step.

The paper is explained in [`docs/projdyn.md`](../../docs/projdyn.md).

## The method

One time step of implicit Euler is the minimum of
`|M^½ (q − s)|² / 2h² + Σ Wᵢ(q)`, where s is where the nodes would fly
to without elastic forces. The paper takes each Wᵢ to be w/2 times the
squared distance from the tetrahedron's deformation gradient F to a set
of allowed matrices, and alternates:

1. **Local**: for every tetrahedron, alone, the allowed matrix nearest
   to F. For strain: singular values clamped to [low, high]. For volume:
   singular values moved the least so their product is in [low, high].
2. **Global**: with those matrices fixed the objective is quadratic in
   q. One linear solve, with a matrix that never changes and is factored
   once.

Neither step can raise the objective, so the step is stable at any time
step, and many rounds give implicit Euler exactly.

## Acceptance (`--check`, lavapipe, 29 s)

| check | measured | rule |
|---|---|---|
| singular values of 2000 random F: U S Vᵀ − F, orthogonality, det − 1 | 1.0e-15, 1.4e-15, 1.3e-15 | < 1e-12 |
| strain projection: is the nearest point (against differences) | 3.0e-10 | < 1e-7 |
| volume projection: is the nearest point | 9.2e-9 | < 1e-6 |
| factor of the 425 × 425 matrix: \|L Lᵀ − A\| / \|A\| | 5.6e-16 | < 1e-13 |
| 40 steps of 10 iterations: largest rise of the objective | 0 | ≤ 1e-13 |
| 1000 iterations: \|gradient\| / at the start (0 is implicit Euler) | 1.7e-13 | < 1e-6 |
| 10 iterations against 1000: distance / move of the step | 0.0069 | reported |
| hanging bar: drop of the lower end / (ρ g L² / 2E) − 1 | −9.1e-5 | < 0.02 |
| free spinning bar, 2 s: change of momentum | 2.6e-11 | < 1e-10 |
| angular momentum, after / before | 0.81 | reported |
| time step 0.5 s, 120 steps: fastest node (started at 3.5 m/s) | 0.44 m/s | < 3.5 |

## What is not done

* The matrix is dense (425 × 425, shared by x, y and z) and factored by a
  plain Cholesky. The paper uses a sparse factor; a dense one stops being
  usable at a few thousand nodes.
* Only tetrahedra: strain (section 5.1), volume (5.2) and attachments
  (the positional constraints of section 6). No triangles or edges, no
  example-based constraints (5.3), bending (5.4) or collisions (6).
* The local step runs on one thread.
