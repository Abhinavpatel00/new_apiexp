# Soft Body Locomotion — the paper and `demos/softloco`

Jie Tan, Greg Turk, C. Karen Liu: *Soft Body Locomotion*, ACM Trans.
Graph. 31(4) (SIGGRAPH 2012).
Paper: <https://sites.cc.gatech.edu/home/turk/my_papers/soft_body_locomotion.pdf>

Section, equation and table numbers below are the paper's.

## 1. The problem

A soft body with no skeleton is to move by itself: reach, crawl, roll.
It has muscles inside. The user gives goals such as a path for a point of
the body; the method finds, every time step, how short each muscle should
want to be. Two things make this hard. The body moves only by pushing on
the ground, so the answer depends on the contact forces. And each contact
can stay, slide or lift, which makes the problem a choice among cases and
not one smooth problem.

## 2. The body (section 4.1)

Tetrahedra with corotational linear elasticity, damping proportional to
the stiffness, and one implicit step (eq. 8, 9):

    (M + dt C + dt² K) v' = M v + dt (f − K (p − R x) + A (ld − l)) + contact
    p' = p + dt v'

`ld` are the lengths the muscles want, `l` the lengths they have. The
paper reports that volume changes stay below 15 per cent without being
enforced.

## 3. Muscles (sections 4.2, 4.3)

A muscle is a chain of segments. A segment contracts along its direction
and neither extends nor bends. Its pull becomes a stress along that
direction in the elements near it, weighted by distance and normalised
within the muscle's group (eq. 3 to 5); the matrix `A` (eq. 6) holds the
force on every node for every segment. The paper names three kinds:
longitudinal muscles shorten or bend the body, radial ones across a
section make it longer because the volume keeps, helical ones twist it.

## 4. The programme of one step (section 5.1)

The goal is a convex quadratic function of the next state. With eq. 8 the
next velocity is linear in `ld` and the contact forces, so the step is
(eq. 11)

    minimise G(ld, f⊥, f∥)
    with     0.5 l0 ≤ ld ≤ l0
             0 ≤ (f⊥, f∥, λ)  ⊥  (Nᵀv', Dᵀv' + Eλ, μ f⊥ − Eᵀ f∥) ≥ 0

The second line is Coulomb contact with the friction cone taken as a few
directions `D` (eight a contact, section 7): a normal force or a
separating speed; sticking or friction on the cone. Earlier work assumed
that contacts stay static; the paper says that is "too restrictive" for
soft bodies and keeps the complementarity conditions.

## 5. Goals (section 5.2)

Linear momentum (eq. 12, 13), angular momentum (eq. 14), the area of the
base seen from above (eq. 15, 16), the position of a point, of the centre
of mass or the difference of two points (eq. 17), and velocity (eq. 18).

## 6. The search (section 6)

Fix which side of every complementarity holds and the problem is a convex
quadratic programme. Start with a guess, for example all contacts static
(eq. 19 to 22) or the previous step's answer. Solve. Every inequality the
answer touches is a candidate: switching it gives a new programme (a
contact goes from static to sliding, from sliding to static, or lets go).
The programmes form a graph. Each node is visited twice: first it is
solved and put in a priority queue by its objective; when it comes to the
top its candidates are switched one at a time, and children already seen
are dropped. The search ends when the queue is empty or a limit of nodes
is reached; the answer is the best programme found. The objective need
not fall from parent to child.

If neither first guess can be met, the paper keeps the last muscle
lengths, solves the contact problem alone and starts from its cases.

## 7. What the paper reports (sections 7, 8)

Letter-shaped bodies meshed with TetGen, contact patches, eight friction
directions a patch. Examples: exercise, balance, slide, jump, roll,
crawl, walk. For the roll an "O" is asked for a change of angular
momentum of (0, 0, −200) in the first 90 frames, and with static contacts
only it "never begins rolling". The crawl uses a ground whose friction
backwards is ten times that of the other directions; the front end stays
while the rear slides, then the other way round, and this "emerges
automatically".

Section 8 tests the solver on 10 problems with 98 variables and 40
complementarity pairs against solving all 10 000 combinations. The gap
ratio (the static programme's distance from the best, over the search's)
averages 6.29; the search used 17 programmes on average, 3 at best, 31 at
worst (table 2).

The paper's stated limits: the choice is for one step only, which gives
large contractions and "unnatural vibration" in the roll, and no motion
that needs planning ahead.

## 8. The demo

`demos/softloco/method.c` is the body, the muscles, the goals, the
programme and the search; `main.c` is three scenes, each with two copies
of the body: blue uses the search, orange the one first programme.

| scene | body | goal |
|---|---|---|
| 0 reach | column 0.4 × 1.2 × 0.4 m on four feet, 20 longitudinal muscle segments | the head at a point that goes round a circle of radius 0.25 m |
| 1 crawl | bar 0.8 × 0.1 × 0.2 m, feet at the two ends, 32 muscle segments | each end 0.12 m forward in turn, 1.2 s a cycle; friction backwards ten times the rest |
| 2 roll | ring of 16 segments, 32 muscle segments | angular momentum rate ∓50 for 1.5 s of every 3 s |

## 9. What the check measures

* Muscles are internal: for each segment the forces of its column of `A`
  sum to 1.4e-15 of the largest, their torque to below 1e-15.
* The body alone: the column with no goals comes to rest (fastest node
  8.6e-12 m/s) at 0.987 of its height. All muscles at half length: height
  0.69. The muscles of one edge at 0.7: the head leans 0.196 m and
  0.190 m towards that edge and the column stays up.
* The programme: in the air, with the head asked where a step with known
  lengths takes it, the programme's objective is 0.78 of that step's, and
  not above any of 300 steps really taken with random lengths. On the
  ground, of 300 such steps 102 leave the feet static; none of those is
  better than the programme.
* Every planned step: the step taken meets the conditions of eq. 11
  (breach below 9e-10 in impulses over weight × dt and speeds over g × dt)
  and gives the objective the programme promised (within 1.5e-10).
* The scenes:

  | | search | one programme |
  |---|---|---|
  | reach, 8 s: mean / worst distance head to target | 0.043 / 0.147 m | 0.231 / 0.413 m |
  | crawl, 4.8 s, 0.48 m asked: gone | 0.475 m | 0.003 m |
  | crawl: worst distance of an end from its target | 0.017 m | |
  | roll: gone at 3 s | 1.71 m | 0.81 m |
  | roll, asked back: gone at 6 s | 0.29 m | 0.74 m |
  | programmes a step, reach / crawl / roll | 5.4 / 14.5 / 4.4 | 1 |
  | volume / rest, least over the scenes | 0.949 | 0.897 |

* The search against all programmes (the paper's table 2, small): 5
  states of the column with 4 contacts, so 10⁴ programmes each. Mean
  objective: one programme 1.82e-05, search of 16 5.56e-06 (9.4 solved),
  search of 64 5.56e-06 (28.6 solved), all 5.56e-06. Both searches found
  the best programme in 5 of 5.

## 10. Agreement with the paper, and where the demo says less

* Agrees: the search does much better than the static programme on the
  same goals; the crawl's pattern of one end holding while the other
  slides comes out of the search without being asked (the orange body
  does not move); a few programmes a step are enough; volume keeps
  within 15 per cent.
* The search matching the exhaustive answer in 5 of 5 states is for 4
  contacts. In another state tried while writing it the search with 16
  programmes was 1.5 times the best. With more than 4 contacts the
  exhaustive search does not finish, so nothing is known there. The
  paper's gap ratio of 6.29 is for larger problems and is not reproduced.
* The roll is weaker evidence than the paper's. The static programme
  rolls too (0.81 m against "never"). The distance with the search is
  1.20 m with at most 8 programmes a step, 1.71 with 16, 1.77 with 64,
  and at the paper's rate of 200 it was 0.88 m. The check therefore only
  asks for more than 0.5 m and that the body comes back when asked.
* In the reach 4 of 480 steps with the search (10 with one programme)
  had no programme that could be met; the muscles then keep their
  lengths. Why those steps fail was not looked into.

## 11. The demo's own choices

* Contacts are nodes within 2 cm of the ground, the lowest 24 at most,
  not patches. A node below the ground is pushed out by a fifth of its
  depth a step.
* Impulses are (normal, x, z) for each contact and the cone is its eight
  edges, each with its own coefficient. A contact is in one of ten cases
  (free, static, sliding on edge 0..7); each gives three equalities, so
  the impulses are eliminated and a programme has only the muscle lengths
  as unknowns. It is solved by the dual active-set method of Goldfarb and
  Idnani, written here; it was not compared with another solver, only
  with steps really taken (section 9).
* First guess: nodes on the ground static, nodes just above it free; or
  the last step's choice.
* A muscle segment runs between two mesh nodes; its stress is
  `stiffness` × (wanted − length) / rest length.
* A cost of 1e-4 × Σ((wanted − rest) / rest)² is added to every goal, so
  that muscles with no effect on the goal stay at rest length.
* Meshes are regular grids cut into tetrahedra; feet are bottom nodes
  set lower in the rest shape.
* Time step 1/60 s, Young's modulus 4e5 (ring 1e6), Poisson's ratio
  0.45, friction 0.8.

## 12. Not implemented

Balance, jump, slide, walk; goals on the centre of mass as in the
paper's exercise (a one-step goal on the centre of mass did not move the
column; the goal on the head does); helical muscles; the paper's fall
back to a pure contact problem when no first guess can be met (the demo
keeps the muscle lengths and resolves the contacts by projected
Gauss–Seidel); contact patches; self-collision.
