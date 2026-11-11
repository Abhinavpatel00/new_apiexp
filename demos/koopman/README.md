# koopman — a soft finger whose motion is one matrix

```
Draw (3-D canvas; the method runs in C: method.c) ─► Present
```

Yue Chang, Peter Yichen Chen, Eitan Grinspun, Maurizio M. Chiaramonte:
*Low-Rank Koopman Deformables with Log-Linear Time Integration*
(arXiv 2602.07687).

A finger of 1296 tetrahedra (208 nodes, stable Neo-Hookean, held at its
base) is simulated with implicit Euler while it is pushed along x, along
z and upwards. To 208 pairs of states from those runs one matrix is
fitted that takes a state to the next (dynamic mode decomposition,
sections 3 and 4 of the paper). It is kept as 20 complex eigenvalues and
modes. After that the finger moves by those: N steps at once are the
N-th powers of 20 numbers.

This is the numerical variant of the paper. The neural variant of
section 5, which gives the modes for a family of shapes, is not here.

* `--scene 0` (default) a push, then free swinging, three times:
  implicit Euler with the 4 ms step of the data (grey), the operator
  taking 10 steps at once (blue), implicit Euler with a step of 40 ms
  (orange). The board behind plots the three kinetic energies: grey and
  blue lie on each other, orange dies away (the paper's figure 2).
* `--scene 1` pushes the fit never saw (along the diagonal, twice as
  strong, half as strong and backwards, twice as strong askew): implicit
  Euler (grey) beside the operator (blue) (figures 5 and 11).
* `--scene 2` the operator with its eigenvalues times 1 (blue), 0.99
  (green) and 0.98 (orange): more damping with no new fit (section 7.2).
* `--scene 3` control (section 7.3): every 40 steps the operator gives
  the two pushes that, held for 60 steps, bring the tip (yellow) to a
  goal (white) and to rest there. They go to implicit Euler (grey) and
  to the operator (blue). The goal changes every 1.28 s.

```sh
build/cm/koopman                       # window; F1 panel: scene, rank, steps at once, push, damping, pause, restart
build/cm/koopman --scene 3             # control of the tip
build/cm/koopman --rank 8 --many 50    # 8 eigenvalues, 50 steps at once
build/cm/koopman --check               # 137 assertions, about 55 s
```

Options: `--scene N`, `--rank N` (2..60, default 20), `--many N` (steps
of 4 ms the operator takes at once, 1..100, default 10), `--push A`
(m/s², scenes 0 to 2; the data has 5), `--damp D` (the eigenvalues are
scaled by 1 − D), `--unclamped` (see "Differences").

## What the check measures

Errors are 100 × Σ|u − u of implicit Euler|² / Σ|u of implicit Euler|²
over the nodes and steps of a run, as in the paper's section 6.3.
Implicit Euler with the 4 ms step is the reference everywhere.

Rank (figure 9). One-step error on a run pushed along the diagonal,
which is not in the data; a rollout of 1000 steps (push along x for 125,
then free; the data stops after 250):

| rank | one step (%) | 1000 steps (%) | largest \|λ\| |
|---|---|---|---|
| 4 | 0.28 | 42.0 | 0.9965 |
| 8 | 0.011 | 1.86 | 0.9969 |
| 12 | 0.0076 | 0.13 | 0.9997 |
| 16 | 0.0044 | 0.29 | 0.9980 |
| 20 | 0.0014 | 0.10 | 0.9969 |
| 30 | 5.7e-5 | 0.17 | 0.9980 |
| 40 | 5.3e-6 | 0.13 | 0.9980 |
| 60 | 9.7e-7 | 0.11 | 0.9979 |

The one-step error falls with every rank. The long rollout does not: it
is as good at 12 as at 60. In the paper (section 6.2) the error falls
fast with the rank and then moves little, not always downwards.

Rank 20, 250 steps:

| run | error (%) |
|---|---|
| push along x, as in the data | 0.042 |
| push along z, as in the data | 0.042 |
| push along the diagonal (not in the data) | 0.30 |
| half the push | 0.10 |
| twice the push (the tip moves 0.18 m of the finger's 0.6 m) | 0.68 |

The state (figure 4), fitted to 84 free steps and run 875 steps on:
displacement alone 80.3 %, displacement with its change over a step
0.0083 %.

Step size (figure 2). A push for 0.4 s, then free; kinetic energy in the
fifth 0.4 s over that in the second:

| | energy kept | error against the 4 ms run (%) |
|---|---|---|
| implicit Euler, 4 ms | 0.1004 | |
| operator, 10 steps at once | 0.0985 | 0.188 |
| operator, single steps | | 0.187 |
| operator, steps of 2.5 × 4 ms (eq. 21) | | 0.187 |
| implicit Euler, 40 ms | 0.0007 | |

Ten steps at once and ten single steps of the operator agree to 1.4e-15.
Ten steps at once with the lift to all nodes take 15 µs; ten implicit
Euler steps take about 2100 times that (32 ms; one core, this small
mesh).

Damping (section 7.2). Eigenvalues × 0.99 against implicit Euler with
5/s more damping: 0.91 % (the operator as fitted: 47.8 %). × 0.98 against
10/s more: 4.3 % (as fitted: 97.9 %).

Control (section 7.3), goals of 8 to 12 cm for the tip, pushes given to
implicit Euler:

| goal (m) | miss after 0.32 s | after 1.28 s | speed of the tip then |
|---|---|---|---|
| (0.10, 0) | 23 mm | 0.8 mm | 0.007 m/s |
| (0, 0.12) | 32 mm | 1.4 mm | 0.009 m/s |
| (−0.08, −0.08) | 53 mm | 0.4 mm | 0.022 m/s |
| (0, 0) | 33 mm | 1.2 mm | 0.008 m/s |

Over those 1280 steps the operator's finger against implicit Euler's,
step by step: 1.4 % on average, 77 % of the steps under 2 %, the worst
17.7 %. The paper's finger stays under 6 %; it is moved quasi-statically,
this one is thrown with up to 15 m/s², three times the push of the data.

Also checked: the elastic body (forces against energy differences to
1.5e-9, the change of the force to 8e-10, small stretch and shear against
Lamé's constants, the residual of the implicit step 3e-11, 3 Newton
iterations a step, energy never rises in a free step); the decomposition
on a system of 40 numbers driven by 6 hidden ones whose eigenvalues are
known (found to 1.6e-15; 24 steps at once, half steps, the closed sum of
eq. 25 and the real operator of section 5.2 all to 1e-13).

## Differences from the paper

* **Size.** 208 nodes and 20 modes against the paper's 10k to 15k
  vertices and 25 to 50 (its table 1). The speed-up above is for this
  mesh and says nothing about theirs (table 2: 2 to 8 ms against 334 to
  3018 ms a step).
* **Data.** The paper's mushroom is trained on one force and tested on
  others. With one pushed run this finger's fit has eigenvalues outside the unit circle
  at ranks 12 to 30 (up to 1.005; the 1000-step rollout then errs by up
  to 6 % where 0.12 % is possible). The demo trains on three runs, each a
  push and then free swinging, with a little body damping (1/s); then no
  eigenvalue is outside at any of the eight ranks.
* **The clamp.** Eigenvalues outside the unit circle are put on it. That
  is not in the numerical part of the paper (its neural variant has a
  loss for |λ| ≤ 1). With the demo's data it changes nothing; `--unclamped`
  switches it off.
* **Forces** are accelerations of the whole body along x and z, not
  pressures in chambers. The control solves unconstrained least squares
  (the paper: non-negative, for pressures) for a goal 60 steps ahead and
  solves again every 40, where the paper's finger is quasi-static.
* **Not built:** the neural variant (section 5), shape optimisation
  (7.4), learning from video (7.5). Of the interactive use of 7.1 there
  is the panel: rank, steps at once and push can change while it runs.

More in [`docs/koopman.md`](../../docs/koopman.md).
