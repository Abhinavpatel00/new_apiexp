# absnewton — absolute eigenvalue filtering for projected Newton

```
(method.c on the CPU, one Newton iteration every 6 frames) ─► Draw ─► Present
```

Chen, Liu, Levin, Zheng, Jacobson: *Stabler Neo-Hookean Simulation:
Absolute Eigenvalue Filtering for Projected Newton* (SIGGRAPH 2024).

Two copies of a nearly incompressible bar (stable Neo-Hookean, Young's
modulus 1e8, Poisson's ratio 0.495) start far from rest and look for
their state of least energy by Newton's method. Each tetrahedron's
12 × 12 Hessian is made positive semi-definite before it is added to
the rest:

* red: negative eigenvalues become 0 (Teran et al. 2005, the usual way);
* green: every eigenvalue becomes its absolute value (the paper).

That is the paper's whole change, one line in `nh_filter`. The ticks
under each bar count its iterations; a bar stops when its Newton
decrement is below 1e-5 λ, the paper's test.

* `--scene 0`: every node moved so the bar is 3 lengths long (each
  element starts with 3 times its volume).
* `--scene 1`: only the end face moved to 3 lengths.
* `--scene 2`: the end face moved to 2 lengths and turned a quarter.
* `--scene 3`: squeezed to half its length.

`--poisson 0.3..0.499`, `--cells 4..16` (resolution), `--tolerance`,
`--trace` (prints every iteration), `--pace`.

The paper is explained in [`docs/absnewton.md`](../../docs/absnewton.md).

## Acceptance (`--check`, about 50 s)

| check | measured | rule |
|---|---|---|
| the paper's two-variable example: f, gradient, eigenvalues (eq. 8) | 2, (4, −0.02), −1.9999e6, 3.9999 | the printed digits |
| its direction, clamped at 1e-3 (eq. 9: −1.19, −19.99) | −1.1999, −19.988 | ± 0.1 |
| its direction, absolute (eq. 11: −0.99, 0.01) | −0.99995, 0.0099995 | ± 0.01 |
| iterations to a gradient below 1e-8: clamped, absolute (figure 8: 11, 3) | 93, 6 | absolute fewer |
| eigenvalue routine, 200 random 12 × 12 | 1e-14 | < 1e-12 |
| eq. 14 to 19 against the eigenvalues of the Hessian in F, 500 random F | 2.3e-12 | < 1e-9 |
| gradient against differences; Hessian along the Newton direction | 1.1e-11; 2.8e-10 | < 1e-7; < 1e-6 |
| bar pulled 0.1 %: narrowing / stretch, against (λ − μ) / 2λ | 0.1668, off by 0.1 % | < 1 % |

Iterations until the decrement is below 1e-5 λ:

| start | Poisson | cubes along | clamped | absolute | rule |
|---|---|---|---|---|---|
| stretched to 3 | 0.3 | 10 | 5 | 5 | within 1 |
| stretched to 3 | 0.45 | 10 | 9 | 7 | absolute fewer |
| stretched to 3 | 0.495 | 10 | 12 | 8 | absolute fewer |
| stretched to 3 | 0.499 | 10 | 15 | 9 | absolute fewer |
| stretched to 3 | 0.495 | 16 | 15 | 8 | absolute fewer |
| stretched to 1.2 | 0.495 | 10 | 6 | 3 | reported |
| squeezed to half | 0.495 | 10 | 5 | 5 | reported |
| only the end pulled to 3 | 0.495 | 10 | 11 | 16 | reported |
| only the end pulled to 3 | 0.495 | 16 | 21 | 17 | absolute fewer |
| end pulled to 2 and turned | 0.495 | 10 | 11 | 14 | reported |

In every row both end within 1.4 % of the same energy.

## What this shows and what it does not

* The gain grows with Poisson's ratio and with resolution, as the
  paper's figures 4 and 14 say.
* On the coarse bar with only the end moved, clamping needs **fewer**
  iterations (11 against 16). The paper says the two are "comparable" at
  moderate volume change; here the absolute filter is the slower one in
  those cases.
* Clamping never blew up or failed to converge here, as it does in the
  paper's figure 1. The paper's meshes are far larger; this one has 540
  to 2 400 elements.
* The matrix is dense (a few hundred nodes at most); the paper uses a
  sparse solver. No collisions, no other energies, no dynamics.
