# contact — rigid bodies that touch, with friction

```
Draw (3-D canvas; the method runs in C: method.c) ─► Present
```

Sheldon Andrews, Kenny Erleben, Zachary Ferguson: *Contact and Friction
Simulation for Computer Graphics* (SIGGRAPH 2022 course notes).

The notes are a course of 172 pages, not one method. The demo builds the
rigid-body thread of it: where spheres and boxes touch (section 2.1), the
contact problem with two friction sets (sections 1.5, 1.7, 3.4.1) and
three iterative solvers for it (sections 3.2, 3.4), with Moreau's
time step and the removal of overlap after the step (sections 3.4.5,
3.4.6).

* `--scene 0` (default) a block of 1600 kg on a tower of five boxes of
  32 kg, three times: Jacobi (orange), Gauss–Seidel (blue), NNCG (green),
  each with the same number of iterations a step. The board behind plots
  the residual of each solver over 200 iterations on one contact problem
  (one line of the grid is a factor of 10; the white line is where a step
  stops).
* `--scene 1` twelve pucks sent off at 2.5 m/s in twelve directions on
  three grounds: friction as a box (orange), as Coulomb's cone (blue), as
  an ellipse with μ 0.25 along x and 0.75 along z (green). The white
  rings are where they should stop.
* `--scene 2` a ramp of 20° with boxes of μ 0.2, 0.3 and 0.5 and a ball;
  on the ground three balls that spin at 30 rad/s with friction against
  spinning of 0, 5 mm and 2 cm.
* `--scene 3` boxes and balls dropped into a pit.

Yellow dots are contacts, white lines their normal impulses.

```sh
build/cm/contact                        # window; F1 panel: scene, iterations, friction set, solver, pause, restart
build/cm/contact --scene 1 --skip 150   # the pucks when they have stopped
build/cm/contact --check                # 140 assertions, about 2 s
```

Options: `--scene N`, `--iterations N` (2..200, default 40), `--set N`
(0 box, 1 ellipse), `--method N` (0 Jacobi, 1 Gauss–Seidel, 2 NNCG;
scenes 2 and 3), `--bodies N` (scene 3), `--adapt` (the rollback rule of
section 3.4.4), `--skip N` (steps of 1/60 s before the first picture).

## What the check measures

Friction against closed forms, 40 iterations a step:

| | measured | closed form |
|---|---|---|
| puck on the cone, μ 0.5, 2.5 m/s: way | 0.6373 m in every direction | v²/2μg = 0.6371 m |
| puck on the box at 45°: way / way on the cone | 0.7073 | 1/√2 = 0.7071 |
| puck on the ellipse: way along x, along z | 1.2743 m, 0.4250 m | 1.2742 m, 0.4247 m |
| 36 pucks: farthest from where it should stop | 0.2 mm box, 0.2 mm cone, 6.7 mm ellipse | 0 |
| ramp of 20°, box with μ 0.2 / 0.3 / 0.5: way in 1 s | 0.7558 / 0.2949 / 0 m | g (sin − μ cos) t²/2, to 1e-6 m |
| ball on the ramp: way in 1 s | 1.1983 m | (5/7) g sin t²/2, to 1e-10 m |
| ball set sliding at 2 m/s: speed once it rolls | 1.42857 m/s | 5/7 of 2 |
| ball spinning at 30 rad/s, μ_τ 0 / 5 mm / 2 cm: spin after 1 s | 30 / 24.55 / 8.20 rad/s | 30 − μ_τ g / (2r²/5), to 1e-13 |

The ellipse's 6.7 mm come from the step: with 1/240 s they are 1.7 mm.

The solvers:

| | Jacobi | Gauss–Seidel | NNCG |
|---|---|---|---|
| heavy tower, residual after 40 iterations from no impulses | 1.2e-3 | 5.9e-4 | 3.4e-4 |
| after 200 | 7.5e-4 | 4.5e-4 | 1.3e-4 |
| heavy tower stepped for 10 s: height of the top box (1.1 m at first) | 0.08 m | 0.20 m | 1.09 m |

(residuals as a share of the heavy block's weight for a step.)

Also checked: sphere–sphere, sphere–box and box–box contacts against
brute force (3000 random pairs of boxes: the 15 axes agree with edges
that pierce, and moved apart by the reported overlap no pair overlaps);
the closest point of an ellipsoid against a search (4e-13); at converged
impulses of every solver and both sets, 12 bodies in the pit obey the
contact conditions read off the bodies' velocities (no contact closes,
λ_n × opening speed below 2e-10, friction inside its set, sliding
contacts have the friction that takes out most energy); Gauss–Seidel on
the box with r = 1/A_ii equals projected Gauss–Seidel written out on
A (2e-15); r = 0.5, 1 and 1.5 / A_ii give the same velocities (4e-13);
starting from the last step's impulses leaves a residual 5e-14 of the
cold start's after 10 iterations on a tower at rest.

## What came out differently, or needs saying

* **The rollback rule** (undo an iteration whose change grew, make r
  smaller) rescues Jacobi when r is four times too large (residual 1.7e-4
  where it otherwise grows to 9e50). Where r was right to begin with it
  did harm: on the pit with 166 contacts Gauss–Seidel reached 7.7e-4 in
  1000 iterations without the rule and 8.9e-2 with it, after 178 undone
  iterations, because the change is not monotone and r only ever shrinks.
  It is off by default.
* **The order of the contacts**: for a stack of two boxes the notes quote
  less than half the iterations for the best order against the worst.
  Over 500 random orders here it is 65 against 101.
* NNCG is ahead of Gauss–Seidel on the heavy tower, as the notes say, but
  none of the three is near a solution in 40 iterations; the tower stands
  because each step starts from the last step's impulses.

## What it is not

* Rigid spheres and boxes only. No meshes, no continuous collision
  detection, no signed distance fields (sections 2.2, 2.3), no soft
  bodies (chapters 4, 5), no pivoting or non-smooth Newton solvers
  (sections 3.1, 3.3), no cone complementarity model (section 1.8), no
  friction model of chapter 6.
* Starting from the last step's impulses is this demo's addition; the
  notes mention it only in passing.
* A sphere whose centre is inside a box gets the gap −(depth to the
  nearest face) − radius; the notes leave the radius out.

More in [docs/contact.md](../../docs/contact.md).
