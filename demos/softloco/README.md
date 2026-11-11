# softloco — soft bodies that move themselves by muscles

```
Draw (3-D canvas; the method runs in C: method.c) ─► Present
```

Jie Tan, Greg Turk, C. Karen Liu: *Soft Body Locomotion* (ACM Trans.
Graph. 31(4), SIGGRAPH 2012).

A body of tetrahedra with no skeleton. Its muscles are segments inside it
that can want to be shorter, down to half their rest length. Every time
step the wanted lengths are chosen so that the body comes as near as it
can to a goal ("this point is there", "the angular momentum changes this
fast"). The choice depends on what each point on the ground does in that
step: stay, slide or let go. The paper chooses both together, by a search
over those cases; each set of cases is one convex quadratic programme.

Each scene shows two copies of one body with the same goals:

| blue | orange |
|---|---|
| the paper's search (`--visits` programmes a step at most) | the one programme in which whatever touches the ground stays where it is |

* `--scene 0` (default) a column on four feet, 20 muscles; its head is
  asked to follow a point on a circle of radius 0.25 m.
* `--scene 1` a flat bar, 32 muscles; its two ends are asked forward in
  turn, 0.12 m in every 1.2 s. The ground holds ten times better against
  sliding backwards (the paper's crawl).
* `--scene 2` a ring, 32 muscles; asked for angular momentum for 1.5 s,
  then nothing for 1.5 s, then the other way.

Muscles are drawn grey at rest length and red when they want to be short.
Dots under a body are its contacts: green stays, yellow slides. A red dot
with a white line is where a goal's point is asked to be.

```sh
build/cm/softloco                       # window; F1 panel: scene, visits, cold start, pause, restart
build/cm/softloco --scene 1 --skip 400  # the crawl after 400 steps
build/cm/softloco --check               # 97 assertions, about 50 s
```

Options: `--scene N`, `--visits N` (1..64, default 16), `--skip N` (steps
of 1/60 s before the first picture), `--cold` (each search starts from
the first guess, not from the last step's choice).

## What the check measures

| | search | one programme |
|---|---|---|
| reach, 8 s: mean distance of the head from its target | 0.043 m | 0.231 m |
| reach: worst | 0.147 m | 0.413 m |
| crawl, 4.8 s, 0.48 m asked: centre of mass gone forward | 0.475 m | 0.003 m |
| roll, asked for 1.5 s: gone at 3 s | 1.71 m | 0.81 m |
| roll, asked back from 3 s to 4.5 s: gone at 6 s | 0.29 m | 0.74 m |
| programmes solved a step (reach / crawl / roll) | 5.4 / 14.5 / 4.4 | 1 |

On 5 states of the column with 4 contacts, compared with solving all 10⁴
programmes: the search with at most 16 and with at most 64 programmes
found the best one in all 5 (9.4 and 28.6 solved on average); the one
programme's objective was 3.3 times the best.

Also checked: a muscle's forces sum to no force and no torque (1e-15);
the column with no goals comes to rest (8.6e-12 m/s); every planned step
taken meets the contact conditions of eq. 11 (breach below 1e-9 in the
plan's units) and gives the objective the programme said (1.5e-10); the
programme's objective is not above that of 300 steps really taken with
other muscle lengths and the same contact cases; volume stays between
0.897 and 1.03 of rest (the paper: changes below 15 per cent).

## What it is not

* Contacts are single nodes within 2 cm of the ground (24 at most), not
  the paper's contact patches from layered depth images.
* The roll asks a rate of 50, the paper 200. In this demo the distance
  rolled changes a lot with the rate and with `--visits`, and not in one
  direction: 1.20 m with 8 programmes a step, 1.71 with 16, 1.77 with 64.
  The paper says the body with static contacts only "never begins
  rolling"; here it rolls 0.81 m.
* The paper's balance, jump, slide and walk are not built. Its exercise
  moves the centre of mass; here a goal on the centre of mass did not
  move the column, so the goal is on the head.
* Meshes are regular boxes cut into tetrahedra, not TetGen meshes of
  letters.

More in [docs/softloco.md](../../docs/softloco.md).
