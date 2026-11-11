# mergesplit — collisions by merging particles for a step and splitting them again

```
(method.c on the CPU: merge, integrate, split) ─► Draw ─► Present
```

Truong, Yuksel, Watcharopas, Levine, Kirby: *Particle
Merging-and-Splitting* (IEEE TVCG 2021), sections 3 and 5, for discs in
the plane.

* `--scene 0`: a ball of 37 particles joined by springs falls on an
  elastic sheet of 61 particles (the paper's figure 3 from the side).
* `--scene 1`: 192 loose particles fall on the sheet.
* `--scene 2`: 150 discs of different sizes and weights in a box, no
  gravity.

`--method 0` is the paper, `1` a penalty force (`--penalty`, log10 of
N/m), `2` an impulse for each pair. `--alpha` is the share of a
collision's energy that is given back (1 in the paper's tests).
`--limit` is the most particles one meta-particle may hold.

The paper is explained in
[`docs/mergesplit.md`](../../docs/mergesplit.md).

## The method

Two particles that intersect and approach become one meta-particle with
their mass and momentum (eq. 1 to 3). The integrator moves it like any
particle for one step, so the springs and the weight of everything
attached decide how the collision ends. Then it is split: the two leave
with velocities that keep the meta-particle's momentum, give back the
kinetic energy the merge took (eq. 4, 9), and do not close in on each
other (eq. 14). Pairs that intersect without approaching are only
marked; if they approach after the step, the step is done a second time
with them merged (section 5.1). Positions are never corrected.

## What `--check` measures

168 assertions, about 4 s. Rules are in brackets; the rest is reported.

| | |
| --- | --- |
| 500 free pairs, α 1 | split − textbook elastic collision 9e-14 m/s; momentum 2e-15, energy 9e-15 |
| α 0.25, α 0 | energy after = before − ¾ of the bond to 4e-15; parting speed 1e-15 |
| 2000 splits, any meta-particle velocity | eq. 8 to 4e-15, eq. 9 to 2e-14, smallest parting speed −6e-15 (≥ −1e-12); 1307 had no root in eq. 13 |
| Three discs, two merging orders | energy the same to 2e-16 in both; final velocities differ by 0.83 m/s |
| Row of five, 1 cm apart, hit at 1 m/s | last leaves at 1, the others stop, to 1e-12 |
| The same row touching | last leaves at 0.889, another at 0.333; momentum and energy exact |
| Box of 150 discs, 5 s | 1861 merges, energy unchanged to 2e-16, deepest overlap 7.7 % of a radius |
| A particle resting on two others, α 0, 4 s | 4783 of 4800 steps done twice; it stays 0.0866 m above them, speed 2e-13 m/s |

Ball on the sheet for 2 s; the ball's particles weigh 1, 10, 100 times a
sheet particle (springs stiffer and steps shorter in proportion). Cells:
ball particles under the sheet of 37 / deepest overlap in radii / energy
after ÷ before − 1.

| | 1 × | 10 × | 100 × |
| --- | --- | --- | --- |
| merging-and-splitting (0, < 0.5, < 3 %) | 0 / 0.07 / +2.4 % | 0 / 0.07 / +0.3 % | 0 / 0.05 / +2.5 % |
| penalty, 0.1 × the sheet's stiffness | 37 / 1.88 / +9.9 % | 37 / 1.95 / +3.3 % | 0 / 0.87 / 0 % |
| penalty, 1 × | 0 / 0.31 / +0.5 % | 0 / 0.15 / 0 % | 0 / 0.12 / 0 % |
| penalty, 10 × | 0 / 0.05 / +11.6 % | 0 / 0.04 / +0.2 % | 0 / 0.03 / +0.3 % |
| impulses | 0 / 0.39 / −5.9 % | 4 / 1.84 / −21 % | 30 / 1.92 / −45 % |

192 loose particles on the sheet for 2 s against the limit on a
meta-particle. Cells: under the sheet / more than half inside another /
largest meta-particle.

| limit | α 1 | α 0.3 |
| --- | --- | --- |
| 2 | 1 / 0 / 2 | 165 / 151 / 2 |
| 8 | 0 / 0 / 8 | 0 / 119 / 8 |
| 32 | 0 / 0 / 22 | 0 / 4 / 32 |
| 64 (rule: 0, 0) | 0 / 0 / 22 | 0 / 0 / 64 |
| none | 0 / 0 / 22 | 0 / 0 / 163 |

## What this says

* Merging-and-splitting held the ball at all three weights with no
  number to choose, with the smallest or nearly the smallest overlap.
* A penalty force also held it when its stiffness was near or above the
  sheet's; at a tenth of that the ball went through. The paper's claim
  that only merging-and-splitting works is not reproduced by this small
  case; its remark that stiffer forces and shorter steps repair a
  penalty is.
* Impulses, one pair after another, let the heavy balls through.
* With bouncing particles (α 1) the limit hardly matters. With particles
  that come to rest (α 0.3) a small limit lets them sink into one
  another and through the sheet, as in the paper's figure 11.
* The order of merging changes the velocities of three discs that touch
  at once. Energy and momentum do not depend on it.

## Not done

Coupling two integrators (section 4, eq. 17 to 20), fluids, fracture,
implicit integration, three dimensions, a neighbour search (every pair
is tested), friction. The limit is one number, not drawn between two as
in the paper. Walls and the two baselines are this demo's own.
