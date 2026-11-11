# Particle Merging-and-Splitting — the paper and `demos/mergesplit`

Nghia Truong, Cem Yuksel, Chakrit Watcharopas, Joshua A. Levine, Robert
M. Kirby: *Particle Merging-and-Splitting*, IEEE Transactions on
Visualization and Computer Graphics, 2021 (arXiv 2107.08093).
<https://graphics.cs.utah.edu/research/projects/merging-and-splitting/>

Sources: the authors' PDF on that page was read from the abstract to the
conclusion. The reference list was not read, and no source code was
looked for or read; `demos/mergesplit` is written from the paper's
equations. Equation, section and figure numbers are the paper's.

## 1. The problem

Particles of two objects must not pass through each other. The usual
answers are a penalty force that grows with the overlap, or an impulse
that turns the closing speed round at once. The paper's objection
(section 1, 2): a penalty force never removes the overlap altogether and
depends on a stiffness somebody has to choose; both can be unstable or
miss collisions, and one particle with a wrong velocity can make a
brittle object crumble.

## 2. The idea

When two particles collide they are **merged** into one meta-particle.
The integrator moves it for one time step as if it were an ordinary
particle. Then it is **split** into the two particles again. So the
collision lasts a whole time step, and during it everything attached to
either particle (springs, neighbours, weight) acts on both together.

## 3. Merging (section 3, eq. 1 to 7)

Particles 1 and 2 with masses m, positions x, velocities v:

    m12 = m1 + m2                               (1)
    x12 = (m1 x1 + m2 x2) / m12                 (2)
    v12 = (m1 v1 + m2 v2) / m12                 (3)

This is a perfectly inelastic collision: momentum is kept, kinetic
energy is not. What is lost,

    ΔE = m1 m2 / (2 m12) · (v1 − v2)²           (4)

is remembered in what the paper calls a virtual bond. The vector
n = x2 − x1 at the start of the step (eq. 5) is kept too, and the
particles keep their places relative to the meta-particle while it
moves (eq. 6, 7); a meta-particle does not turn.

Forces between a meta-particle and its neighbours are worked out from
the positions of its own particles and act on the meta-particle as a
whole.

In the code: `ms_merge` is eq. 3 and 4. A node of the merge tree in
`ms_step` holds mass, velocity, summed force, the bond and n.

## 4. Splitting (eq. 8 to 16)

After the step the meta-particle has a new velocity v12′. The split
wants v1′ and v2′ with

    m12 v12′ = m1 v1′ + m2 v2′                                (8)
    α ΔE + ½ m12 v12′² = ½ m1 v1′² + ½ m2 v2′²                (9)

α between 0 and 1 says how much of the bond's energy comes back; the
paper uses 1 in its tests. Putting (8) into (9) leaves one condition:
v1′ lies on a sphere round v12′,

    (v12′ − v1′)² = s²,   s² = 2 α ΔE / (m12 · m1 / m2)       (10, 11)

The paper looks for v1′ on the line through the old v1 along the
direction n̂ = n / |n|, allowing a sideways part ε:

    v1′ = v1 + μ n̂ + ε,   ε · n̂ = 0                           (12)

With ε = 0 this gives a quadratic in μ,

    μ² − 2 n̂·(v12′ − v1) μ + (v12′ − v1)² − s² = 0            (13)

and the **smaller** root is taken, because then the two leave without
closing in on each other:

    (v2′ − v1′) · n̂ ≥ 0                                       (14)

If (13) has no real root the line misses the sphere. Then the point of
the line nearest the sphere's centre is taken,

    μ = n̂ · (v12′ − v1)                                       (15)

and ε moves it sideways onto the sphere:

    ε = (|w| − s) w / |w|,   w = v12′ − v1 − μ n̂              (16)

v2′ follows from (8).

In the code: `ms_split`. It returns whether (13) had a root.

Two things can be said from the equations alone, and the check confirms
both. If nothing acted on the meta-particle (v12′ = v12) and α = 1, the
result is the textbook elastic collision along n̂. If the bond is empty
(the two had the same velocity, as in resting contact), s = 0, (13) has
no root unless the change of velocity is along n̂, and (15), (16) give
v1′ = v2′ = v12′: the two simply keep moving together.

## 5. More than two (section 3)

A meta-particle is a particle, so a third particle merges with it in the
same way, and the splits are done in the reverse order. The paper
reports that the order of merging made no difference in its tests.

In the code: the merge tree in `ms_step`; a later merge has a higher
node index, and the splits run from the highest down.

## 6. When to merge (section 5.1)

    rule 1:  |x2 − x1| < r1 + r2                              (21)
    rule 2:  n · (v2 − v1) < 0                                (22)

Intersecting is tested at the start of the step only, so the step must
be short enough that a particle moves less than its radius. The split is
done after the positions are updated, which leaves the two still
intersecting at the next step; rule 2 keeps them from being merged for
ever, since after a split they do not approach.

Resting contact needs more. Two particles lying on each other do not
approach at the start of a step, but they would by its end. So the step
has **two stages**: the first merges pairs that pass both rules and
marks pairs that pass only rule 1. "If the marked particles at the end
of the time-step integration have velocities pointing towards each
other, we recompute the time-step integration in a second stage by
merging those marked particles in the beginning of the time-step." More
stages could be needed in principle; the paper stops at two ("we have
not observed any practical consequences of not adding extra stages").

Positions are never corrected, because moving a particle out of an
overlap adds or removes energy (section 5.1, 7). Particles joined by a
spring are not collided with each other.

In the code: `ms_step`. Rule 2 is tested with the two particles' own
velocities even when one already belongs to a meta-particle; the paper
does not spell this case out, and this choice makes the set of merged
pairs independent of the order in which pairs are visited.

## 7. A limit on the size (section 5.2, 6.4)

Without a limit, a block of fluid lying on a cloth merges with all of it
and the cloth turns rigid (figure 11b). The paper lets a meta-particle
hold at most n particles, n chosen between n_min and n_max, and found
8 and 64 to give similar results in all its tests; with n_min = 4 some
fluid got through the cloth (figure 11f).

In the code: `Ms.limit`, one number (64 by default), not a random choice
between two.

## 8. What the paper does beyond this demo

* Section 4 couples two different integrators: a meta-particle is
  advanced by both, the two results are averaged by mass (eq. 17), and
  the kinetic energy that the averaging loses (eq. 19) goes into the
  bond with a second factor β (eq. 20). Not implemented; this demo has
  one integrator.
* Solids integrated implicitly, fluids by SPH and FLIP, brittle solids
  by peridynamics, fracture by fluid (figures 4 to 9). Not implemented.
* Its discussion (section 7) names the limits of the method: no
  physical friction (ε only makes the sideways exchange as small as it
  can be), no turning of meta-particles, and holes in a sparse material
  let particles through.

## 9. The demo

Discs in the plane, on the CPU, every pair tested (no neighbour search).
The integrator is symplectic Euler with 1/1200 s steps: forces from the
positions at the start, new velocities, new positions.

Additions that are not in the paper:

* Walls. A particle that would cross one has that velocity component
  turned round; a meta-particle is turned round when any of its
  particles would cross.
* Held particles (the two ends of the sheet) collide with nothing.
* The two baselines. *Penalty*: a force of stiffness × overlap along
  the line of centres. *Impulses*: before the forces, each intersecting
  approaching pair in turn gets the impulse of a collision with
  restitution √α (elastic at α = 1).
  The paper's figure 3 also compares with penalty forces and impulses,
  but these two are this demo's own simple versions, not the paper's.

## 10. What the check measures

`mergesplit --check`: 168 assertions in about 4 s.

**The split alone.** 500 random pairs approaching along n, merged and
split with nothing in between: the result equals the textbook elastic
collision to 9e-14 m/s. With α = 0.25 the energy after is the energy
before less three quarters of the bond, to 4e-15; with α = 0 the parting
speed is 1e-15. For 2000 splits where the meta-particle's velocity after
the step is any random vector, eq. 8 holds to 4e-15, eq. 9 to 2e-14, and
the smallest parting speed is −6e-15 (eq. 14). 1307 of those 2000 had no
real root, and eq. 15, 16 still met eq. 8 and 9.

**Order of merging.** Three discs, the middle one touching both, all
closing in, merged as (0 1) 2 and as (2 1) 0. Energy and momentum are
the same in both. The final velocities differ by up to 0.83 m/s. So for
three bodies that touch at once the order does matter to the individual
velocities; the paper's statement is about what its simulations looked
like, which this test does not contradict.

**A row of discs.** Five equal discs at rest 1 cm apart, a sixth arrives
at 1 m/s: the collisions happen one after another, the last disc leaves
at 1 m/s and the others stop, exactly. With the five touching, several
merge in a step (4 at most) and the last disc leaves at 0.889 m/s with
another at 0.333; momentum and energy are still exact. The row of
touching balls that passes all its motion to the last one is not
reproduced.

**A box of 150 discs** of different sizes and weights, no gravity, 5 s:
1861 merges, energy unchanged to 2e-16, deepest overlap seen 7.7 % of
the smaller radius (about what a fast pair can gain in the one step
before it is first seen).

**The ball on the sheet** (figures 3 and 7). The sheet is 61 particles
from wall to wall with its ends held; it is not at rest when the ball
arrives but swinging under its own weight. A ball of 37 particles falls
on it. Ball particles weigh 1, 10 and 100 times a sheet particle; the
springs are made stiffer in the same proportion and the step shorter
(2400, 8000, 24 000 steps for 2 s). Cells: ball particles under the
sheet after 2 s / deepest overlap of a ball and a sheet particle in
radii / energy after ÷ before − 1.

| | 1 × | 10 × | 100 × |
| --- | --- | --- | --- |
| merging-and-splitting | 0 / 0.07 / +2.4 % | 0 / 0.07 / +0.3 % | 0 / 0.05 / +2.5 % |
| penalty, 0.1 × the sheet's stiffness | 37 / 1.88 / +9.9 % | 37 / 1.95 / +3.3 % | 0 / 0.87 / 0 % |
| penalty, 1 × | 0 / 0.31 / +0.5 % | 0 / 0.15 / 0 % | 0 / 0.12 / 0 % |
| penalty, 10 × | 0 / 0.05 / +11.6 % | 0 / 0.04 / +0.2 % | 0 / 0.03 / +0.3 % |
| impulses | 0 / 0.39 / −5.9 % | 4 / 1.84 / −21 % | 30 / 1.92 / −45 % |

Rules apply to the first row only (0 under, overlap < 0.5, energy within
3 %, no particle faster than 9 m/s). With merging-and-splitting 7 %,
71 % and 29 % of the steps were done twice, the largest meta-particle
held 4, 9 and 3 particles, and 43 %, 17 % and 12 % of the splits had no
root in eq. 13.

**Resting contact.** A particle laid in the hollow between two that hang
from damped springs, α = 0, 4 s: 4783 of 4800 steps were done twice, the
particle never came closer than 0.99998 of the sum of radii, ended
0.0866 m above the two as it started, the springs were stretched by the
weight ÷ stiffness (9.2e-5 m), and everything was at rest to 2e-13 m/s.

**The limit.** 192 loose particles fall on the sheet at 3 m/s, 2 s.
Cells: under the sheet / more than half inside another loose particle /
largest meta-particle.

| limit | α 1 | α 0.3 |
| --- | --- | --- |
| 2 | 1 / 0 / 2 | 165 / 151 / 2 |
| 8 | 0 / 0 / 8 | 0 / 119 / 8 |
| 32 | 0 / 0 / 22 | 0 / 4 / 32 |
| 64 | 0 / 0 / 22 | 0 / 0 / 64 |
| none | 0 / 0 / 22 | 0 / 0 / 163 |

## 11. Where this agrees with the paper and where it does not

* Agrees: merging-and-splitting kept the ball out at every mass ratio
  with no parameter, as figure 7 shows for 1, 10 and 100. Impulses
  failed for the heavy balls.
* Does not reproduce: that a penalty force must fail. Here a penalty
  with the sheet's stiffness or ten times it also held the ball (with
  deeper overlap at 1 ×, and 12 % more energy at 10 × with the lightest
  ball). A tenth of the stiffness failed. The paper says as much in its
  discussion: "most of the problems we present regarding force-based
  collision formulations can be resolved by increasing the stiffness and
  reducing the time-step size". The paper's hard cases (fracture, a
  cloth one particle thick under fluid, implicit integration) are not in
  this demo.
* Agrees: a limit that is too small lets particles through and into one
  another once they rest on each other (α 0.3: limit 2 let 165 of 192
  through), and 64 was enough. With α 1 the loose particles never come
  to rest and the limit hardly matters. The rigid cloth of figure 11b
  was not measured.
* Energy: merging and splitting themselves keep it to rounding (the box
  of discs). On the sheet it drifts by up to 2.5 % in 2 s, presumably
  the integrator together with the changing set of meta-particles; the
  paper makes no promise there ("the velocity update provided by the
  integrators do not necessarily conserve energy or momentum").
* Not explained: with impulses the energy falls by 6 to 45 % although
  each impulse alone keeps the pair's kinetic energy. The cause was not
  tracked down.

## 12. Not implemented

Section 4 (two integrators, β), fluids, peridynamics and fracture,
implicit integration, three dimensions, a neighbour search, more than
two stages, a random limit between n_min and n_max, friction, collisions
with held particles.
