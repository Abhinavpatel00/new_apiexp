# lines — toon outlines two ways

```
Scene (colour, normal + object number, depth) ─► Hull ─► Edges ─► Present
```

The two outline methods NiloToon uses together (see
`docs/nilo-toon-study.md`), side by side on one scene: `--mode 1` hull,
`--mode 2` edges, `--mode 3` both, `--mode 0` none.

| | technique | what it gives |
|---|---|---|
| inverted hull | the object drawn again, back faces only, each vertex pushed along its normal — here by a fixed number of **pixels**, in clip space | a line outside the silhouette; nothing inside the object |
| edges in the picture | a fullscreen pass over depth and normals: a line where the neighbour is farther than this pixel's own surface plane would be there, or where the normal turns by more than the crease angle | a line on the near side of every depth break, plus creases; nothing where two surfaces meet at the same depth and angle |

**This is not the roadmap's "toon v2".** That item asks for `demos/toon` and
`demos/slang-toon` to be merged into one host, with MSAA. This demo is only
its outline comparison, on the framework's own scene; the two toon hosts
are untouched and MSAA is still unused.

## Acceptance (`--check`)

640 × 360; the same run passes at 320 × 180 and 1280 × 720.

| Line | Rule | Result |
|---|---|---|
| **ball, hull, 2 / 3 / 5 px asked, at 6 m** | within 0.35 px | 2.02 / 3.01 / 4.96 |
| **… at 14 m** (the ball is 31 px in radius instead of 73) | within 0.35 px | 2.01 / 3.01 / 5.07 |
| hull pixels on the ball itself | 0 | 0 |
| **ball, edges, 2 / 3 / 5 px asked, at 6 m** | 0.90 × asked, within 0.15 px | 1.80 / 2.70 / 4.52 |
| … at 14 m | same | 1.79 / 2.70 / 4.53 |
| edge pixels outside the ball | 0 | 0 |
| box, share of the silhouette with a line within 2 px: hull / edges | > 97 % / 100 % | 99.2 % / 100 % |
| **box, line pixels more than 6 px inside the silhouette: hull / edges** | 0 / > 100 | 0 / 390 |
| **floor seen from 1.5 m up, share of it with a line that is not its edge: plane rule** | 0 | 0 |
| … the same with neighbouring depths compared directly | > 1 % | 29 % (8 % at 1280 × 720) |
| whole scene, share of the picture that is line: hull / edges | 0.5–10 % | 1.7 % / 3.3 % |

Width is measured as a difference of radii: the ball's silhouette is a
circle, so radius = √(area/π) with and without the line pixels.

## What the numbers say

- **Hull width is what was asked for, at any distance**, because the push
  is done in clip space in pixels. (The older `demos/toon` hull pushes in
  world space scaled by distance and field of view; it was not measured
  here.)
- **The edge line is 10 % thinner than asked**, and that is the search
  pattern, not noise: it looks along the two screen axes only, so a line w
  pixels wide there is w/√2 on the diagonals; round a circle the mean is
  0.90·w, and the measurements are within 0.03 px of that.
- **The hull is outside the object, the edge line inside.** With both on
  (`--mode 3`) the outline is the sum of the two widths.
- **Only the edge pass draws the box's inner edges** (390 pixels against 0).
  The hull leaves gaps instead: a box has three normals at each corner, so
  the hull splits there (0.8 % of the silhouette without a line nearby).
  That is why NiloToon bakes smoothed normals for its hull; this demo does
  not.
- **The plane test matters**: comparing a pixel's depth with its
  neighbour's directly, with the same threshold, draws lines over 29 % of a
  floor seen at a low angle. Other fixes exist (a threshold scaled by the
  slope); only this one was tried.

## Time

lavapipe, 1280 × 720, difference of a 10- and a 110-frame run, two repeats:
no lines 23 ms, hull 27–33 ms, edges 61–69 ms; edges at 8 px 118 ms (one
run). The edge pass reads up to 8·w texels per pixel.

## Not done, not tested

- Not merged into the toon demos, no MSAA; see above. The lines are
  aliased, one pixel on or off.
- The thresholds (3 % of depth + 5 cm; crease 40°) are fixed and were set
  by eye on this scene. A small step between two parallel surfaces closer
  than that draws no line.
- Where the ball touches the floor the line breaks up: the depth break
  shrinks to nothing there and the two normals differ by less than the
  crease angle part of the way. Visible in the picture, not measured.
- No lines from object numbers or colour, though the object number is in
  the buffer.
- No smoothed normals for the hull; no per-vertex width, no z-offset (both
  are in `demos/toon`).
- lavapipe only; the window was opened under Xvfb, the panel not operated.

## Running

```sh
./build/cm/lines                  # edges
./build/cm/lines --mode 1         # hull; 3: both; 0: none
./build/cm/lines --width 5 --crease 25
./build/cm/lines --headless out.ppm --size 640 360 --check
```
