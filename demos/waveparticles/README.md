# waveparticles — water waves as particles that carry a bump each

```
(method.c on the CPU: move time on, sum the bumps) ─► Draw ─► Present
```

Yuksel, House, Keyser: *Wave Particles* (SIGGRAPH 2007), section 3 and
the height field part of section 5.

* `--scene 0`: rain on a pool of 8 m with four walls.
* `--scene 1`: a source going round at 2.6 m/s on open water, where the
  waves travel at 1.5 m/s: a wake.
* `--scene 2`: straight waves leave one wall and come back from the
  other.

`--dots` draws the particles. `--no-warp` leaves out the horizontal
motion of section 3.4. `--image 1` and `2` build the height field the
way the paper does on a GPU (particles as points, then a filter).
`--damping`.

The paper is explained in
[`docs/waveparticles.md`](../../docs/waveparticles.md).

## The method

The surface is a sum of bumps, `a/2 (cos(π ρ / r) + 1)` within a radius
r (eq. 7). Each rides on a particle that moves in a straight line at the
wave speed, so nothing has to be solved and nothing can blow up. A
particle knows which angle of its wavefront it stands for; when the
front has widened so far that neighbours are more than r/2 apart, it
becomes three with a third of the amplitude and of the angle each. That
moment is known when the particle is born, so it goes into a heap, and a
time step touches only the particles whose moment has come. A ripple
starts as **one** particle with an angle of 2π.

## Acceptance (`--check`, about 3 s)

| check | measured | rule |
|---|---|---|
| one bump: centre, edge, volume / a r² (π/2 − 2/π) − 1 | a, 0, −2.6e-6 | exact, exact, < 1e-3 |
| front of particles r/2 apart: along the crest (paper: < 0.1 %) | 6e-16 | < 0.001 |
| the same, across the front against eq. 5 (paper: < 3 %) | 7.0 % | < 8 % |
| across the front with particles r/16 apart | 6.3 % | reported |
| ripple 3 m out: particles; widest spacing / r | 243; 0.26 | 243; ≤ 0.5 |
| sum of amplitudes; volume | unchanged to 3e-15; to 2e-8 | |
| crest height round the ring / (a r / 2π d) | 0.989 to 1.002 | 0.97 .. 1.03 |
| crest speed / wave speed | 1.000 | 0.95 .. 1.05 |
| particles visited in 120 steps | 121 = (243 − 1) / 2 | exact |
| three ripples in a pool, 240 steps against 7 steps: height fields | differ by 6e-14 | < 1e-9 |
| ripple at a wall against open water with its mirror image | 1.7e-15 | < 1e-12 |
| water in the pool while the wave reflects | unchanged to 2e-5 | < 2e-3 |
| amplitude 0.05, threshold 1e-4: 729 particles, all removed at 17.4 m | 729, then 0 | exact |
| image from points and eq. 7 as a filter (r = 9.6 samples) | 10.5 % of the peak off | < 15 % |
| image from points and two 1-D filters | 15.7 % | reported |
| wave train: surface point on a circle; height and horizontal against cos and −sin | 12 % off at worst | < 15 % |
| horizontal velocity at a crest / (a π v / r) | 0.996, forwards | 0.95 .. 1.05 |

## What is not done

* Section 4, objects in the water: no boats, no forces on objects, no
  waves made by objects. The sources here are points.
* Curved walls. The pool is a square, where a reflection is a fold of
  the plane and needs no event.
* The GPU. The paper renders particles as points into a texture and
  filters it; here that is done in C (`--image 1`, `2`) and the default
  sums each bump exactly.
* The paper's 3 % for the shape across a front was not reproduced: 7 %.
  The profile of a row of round bumps is not the cosine of eq. 5 however
  close they are (6.3 % at r/16).
