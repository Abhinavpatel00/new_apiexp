# jfa — distances to any shape by jump flooding

A shape is drawn into a 512 × 288 grid (a ring, a turning bar, a wavy line one
cell thick, 24 dots), and ten compute passes later every cell knows the
closest cell inside the shape and the closest cell outside it. The difference
of the two distances is a signed distance field. It is rebuilt from nothing
every frame.

```
Init (compute: draw the shape)
  -> Flood, step 1, 256, 128, ... 1   (compute, back and forth between two images)
  -> Show -> Present -> UI
```

| Part | Source | Where |
|---|---|---|
| Ask the cells one step away, halve the step | Rong & Tan, *Jump Flooding in GPU with Applications to Voronoi Diagram and Distance Transform*, I3D 2006 | `compFlood` |
| Extra passes before or after (JFA+1, JFA+2, 1+JFA) | the same paper, and its follow-up *Variants of Jump Flooding Algorithm for Computing Discrete Voronoi Diagrams*, 2007 | `k_steps` in `main.c` |
| Two answers per texel, inside and outside, for a signed distance | common practice | header of `jfa.slang` |

Cited from memory; neither paper was fetched while writing this.

## Measured

`./build/cm/jfa --headless out.ppm --check`, lavapipe. The check draws the
shape at eight moments of its animation. For each it finds, on the CPU, the
true closest inside cell and outside cell of every cell by trying every
boundary cell (up to 4 036 of them), and compares squared distances as
integers. 294 912 answers per shape. Worst of the eight shapes:

| Order of steps | Passes | Answers that are not the closest | Largest distance error | Shapes with an error above 1 cell | Rule |
|---|---|---|---|---|---|
| JFA: 256 … 1 | 9 | 0.185 % | 12.0 cells | 2 of 8 | reported |
| JFA+1: 256 … 1, 1 | 10 | 0.170 % | 11.8 cells | 1 of 8 | reported |
| JFA+2: 256 … 1, 2, 1 | 11 | 0.147 % | 11.5 cells | 1 of 8 | reported |
| 1+JFA: 1, 256 … 1 (default) | 10 | 0.0047 % | 0.34 cells | 0 of 8 | < 0.02 %, < 1 cell, = 0 |

No answer ever names a cell of the wrong kind, and no cell is left without an
answer (both = 0, asserted for all four).

Time per frame at 1280 × 720 on lavapipe: 72 ms with 1+JFA, 67 ms with JFA.

## What the numbers say

**Jump flooding is approximate, and how wrong it is depends on the shape.**
On six of the eight shapes plain JFA's largest error is under 1 cell, and on
one it is 1.2 cells. On one (animation step 90) it loses a small dot for a
whole region near the right edge: 223 cells are told the closest shape is 6
to 12 cells further away than it is.

**One pass of step 1 at the start removes the large failure; passes at the
end do not.** A dot of a few cells has few routes by which news of it can
travel, since every hop must land on a cell that does not already know
something closer. The first small step lets its neighbours carry the news
too. Extra small steps at the end only repair errors a cell or two wide.

**1+JFA is not the most accurate on every shape.** On the four shapes whose
numbers were printed one by one (steps 1, 300, 600, 1000), JFA+2 had the
fewest wrong answers (0.0003 to 0.002 %, against 0.002 to 0.005 % for 1+JFA).
1+JFA is the default because it is the only one that never failed badly in
these eight.

Eight shapes are a sample, not a proof. The rules assert what was seen.

## Not done, not tested

- The field is rebuilt from nothing each frame. Nothing is reused from the
  previous frame.
- Distances are between cell centres, shifted half a cell, so the field is
  right to about half a cell at best. No sub-cell distance from coverage.
- Not connected to `sdfworld`. Its volume is 3D and keeps distances only
  within 4 voxels of the surface, and re-distancing it after edits is where a
  3D flood would be used. That is not built.
- JFA² (running the whole sequence twice) is not measured.
- The picture is stretched to the window.
- Lavapipe only. Run in a window once, under Xvfb, for one screenshot (no
  validation errors); the panel's controls were not operated.

## Running

```
./build/cm/jfa                              # window
./build/cm/jfa --headless out.ppm --check   # 90 steps, then the checks
```

`--view N` (0 distance bands, 1 closest inside cell, 2 outline and glow),
`--variant N` (0 JFA, 1 JFA+1, 2 JFA+2, 3 1+JFA), `--still`, `--frames N`.

## References

- Guodong Rong, Tiow-Seng Tan, *Jump Flooding in GPU with Applications to
  Voronoi Diagram and Distance Transform*, I3D 2006.
- Guodong Rong, Tiow-Seng Tan, *Variants of Jump Flooding Algorithm for
  Computing Discrete Voronoi Diagrams*, ISVD 2007.
