# clay — particles, drawn through a distance volume rebuilt from them

```
[Clear ─► Predict ─► Bin ─► Sort ─► Solve ─► Finish] × 8     simulation, compute
  ─► Clear ─► Bin ─► Sort ─► Bake                             particles ─► 64³ distance volume, compute
  ─► Draw (sphere tracing) ─► Present
```

4 096 particles of 5 cm radius fall into a tray as a block, slump, and are
ploughed by a ball that circles through them. Nothing is drawn as a
particle: every frame the particles are turned into a distance volume, and
the picture is sphere-traced through it.

| | technique | source |
|---|---|---|
| simulation | position-based dynamics: predict, push overlapping pairs apart, velocity from the change in position | Müller et al. 2007 |
| solver | Jacobi, pair corrections averaged and over-relaxed (× 1.5); 8 small steps per frame, 1 pass each | Macklin et al. 2014 (averaging), 2019 (small steps) |
| neighbours | uniform grid of 10 cm cells in the storage buffer, 8 slots per cell, filled with an atomic counter | common practice |
| volume | each voxel takes the minimum over the particles in the 27 cells around it; exact within 10 cm of a centre, clamped beyond | — |
| the idea | simulate clay as particles and rebuild the world's distance field from them | Claybook (Aaltonen, GDC 2018) |

All cited from memory. This is Claybook's idea at toy size, not its method.

## Acceptance (`--check`)

| Line | Rule | Measured |
|---|---|---|
| after 6 s without the paddle: particles in their cell's list, once | == 4 096 | 4 096 |
| particles lost because a cell was full | == 0 | 0 (fullest cell: 4 of 8) |
| nearest pair of centres | > 0.85 diameters | 0.92 |
| pairs overlapping by more than a tenth of a diameter | == 0 | 0 |
| particles through a wall or the floor | == 0 | 0 |
| mean speed | < 0.1 m/s | 0.035 m/s (fastest 0.14) |
| **volume vs every particle on the CPU, all 262 144 voxels, largest difference** | < 0.5 mm | 0.03 mm |
| 4 s of paddle, 12 samples: particles inside the paddle | == 0 | 0 |
| particles through a wall or the floor | == 0 | 0 |
| particles missing from the grid, or lost to full cells | == 0 | 0 |
| nearest pair ever | reported | 0.87 diameters |
| the same 2 s run twice: particles whose state differs in any bit | == 0 | 0 |
| picture: rays ending on clay / on the paddle | > 10 % / > 0 | 17 % / 392 pixels |

What made the difference, measured after 6 s with the paddle off
(`--substeps S --solves K --relax W`):

| steps × passes per frame, relaxation | nearest pair, diameters | pairs overlapping > 10 % | highest centre |
|---|---|---|---|
| 1 × 4, 1.2 | 0.58 | 9 057 | 0.44 m |
| 1 × 8, 1.2 | 0.67 | 3 720 | 0.52 m |
| 1 × 8, 2.0 | 0.69 | 886 | 0.55 m |
| 4 × 2, 1.2 | 0.85 | 27 | 0.74 m |
| 8 × 1, 1.5 (default) | 0.92 | 0 | 1.01 m |
| 8 × 2, 1.5 | 0.96 | 0 | 1.00 m |

Stickiness is applied once per step, so more steps per frame also make the
clay stickier; the last column shows both effects together.

## What the numbers say

- Eight solver passes in one step leave particles squashed to two thirds of
  a diameter; the same eight passes spread over eight small steps do not
  (0.92). Per frame at 64 × 36 that cost 23 ms against 24–29 ms.
- The distance volume is exactly the particles, to the precision of a 16-bit
  float.
- **A run repeats bit for bit only because of the Sort pass.** Without it
  three runs of the same 240 frames gave three different pictures (10 % of
  the bytes differed): which slot a particle gets in its cell depends on
  thread timing, and that changes the order of a floating-point sum.
- After 6 s the pile still creeps at 3.5 cm/s. It behaves like thick mud that
  slowly flattens, not like clay that keeps a shape.

## Time

lavapipe, wall time per frame (160-frame run minus 100-frame run, divided
by 60):

| | |
|---|---|
| 1280 × 720 | 172 ms |
| 64 × 36 (so: simulation and bake) | 24–29 ms |
| 64 × 36, one step of one pass per frame instead of eight | 14 ms |

At 1280 × 720 the sphere tracing is most of the frame. The volume is
clamped 5 cm from the clay, so a ray crossing the tray takes many short
steps; there is no coarse level as in `sdfworld`.

## Not done, not tested

- lavapipe only. The window ran for a few seconds under Xvfb with the
  validation layer; the panel, including its "drop again" button, was not
  operated.
- No static friction and no cohesion: "stickiness" only slows a particle
  that touches something.
- The paddle is held still during a frame's eight steps and jumps between
  frames.
- A cell holds 8 particles; a ninth would be ignored by its neighbours. It
  did not happen here (4 at most) and the check asserts that. Nothing
  prevents it under stronger compression.
- The smooth union used for the picture depends on the order of the list; the
  volume check runs with it switched off. The smoothed volume is unchecked.
- The clay does not act on anything: the paddle is not slowed, the tray is
  fixed, and the volume is not merged into another world as in `sdfworld`.
- One particle count, one radius. Timings for other sizes were not taken.

## Running

```
./build/cm/clay                                  # window; left drag orbits
./build/cm/clay --headless out.ppm --frames 240
./build/cm/clay --headless out.ppm --check --size 320 180
```

`--view N` (0 lit, 1 steps per ray, 2 normal), `--substeps N`, `--solves N`,
`--relax X`, `--sticky X`, `--no-paddle`.

## References

- Matthias Müller, Bruno Heidelberger, Marcus Hennix, John Ratcliff,
  *Position Based Dynamics*, 2007.
- Miles Macklin, Matthias Müller, Nuttapong Chentanez, Tae-Yong Kim,
  *Unified Particle Physics for Real-Time Applications*, SIGGRAPH 2014.
- Miles Macklin et al., *Small Steps in Physics Simulation*, SCA 2019.
- Sebastian Aaltonen, *GPU-based clay simulation and ray-tracing tech in
  Claybook*, GDC 2018.
