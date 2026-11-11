# fluid — stable fluids on a staggered grid

Two jets blow dyed fluid into a closed box, 256 × 256 cells, all in compute.
Each step moves velocity and dye along the flow, then removes from the
velocity whatever would pile fluid up in a cell or drain it.

```
AdvectVelocity -> AdvectDye -> CopyDye          (compute)
  -> Divergence                                 (compute: what flows out of each cell)
  -> RelaxRed, RelaxBlack  x 20                 (compute: pressure, in place)
  -> Project                                    (compute: velocity -= pressure gradient)
  -> Show -> Present -> UI
```

| Part | Source | Where |
|---|---|---|
| Advect by looking backwards along the flow; then project | Stam, *Stable Fluids*, SIGGRAPH 1999 | `compAdvect`, `compProject` |
| Velocities on cell edges, pressure in the middle | Harlow & Welch, 1965 (the MAC grid) | header of `fluid.slang` |
| Pressure by relaxation, one checkerboard colour at a time, over-relaxed | textbook SOR with red-black ordering | `compRelax` |
| Plain Jacobi steps between two images | Harris, *Fast Fluid Dynamics Simulation on the GPU*, GPU Gems 1, ch. 38 | `compJacobi` (`--jacobi`) |

The sources are cited from memory; none was fetched while writing this.

## The grid

A cell's texel holds the horizontal velocity on its left edge and the
vertical velocity on its bottom edge. What flows out of a cell is then four
differences of stored numbers, and the pressure gradient that is subtracted
sits on the same edges. The walls are edges whose velocity is zero and which
the pressure solve does not count as neighbours.

Velocity, divergence and pressure are RGBA32F; dye is RGBA16F. `vk.h`'s
storage-image table is one descriptor array, which the shader declares twice
with two formats (`muStorageImages`, `muStorageImages32`).

## Measured

`./build/cm/fluid --headless out.ppm --check`, lavapipe, frame 241.

| Check | Value | Rule |
|---|---|---|
| Divergence pass against the same sum on the CPU, largest difference / largest value | 7.6e-7 | < 1e-5 |
| Projection against the same formula on the CPU | 8.6e-6 cells/s | < 1e-3 |
| Mean outflow per cell before projection | 0.282 cells/s | reported |
| Mean outflow per cell after projection | 0.090 cells/s | reported |
| Mean outflow after / before | 0.32 | < 0.5 |
| Largest outflow after / before | 0.082 | < 0.25 |
| Velocity through the left and bottom walls, cells not exactly 0 | 0 | = 0 |
| Values that are not numbers | 0 | = 0 |
| Fastest cell | 3.8 cells per step | reported |
| Dye lowest / highest | 0 / 0.992 | within 0..1 |
| Mean outflow after 120 more rounds of the solver / before | 0.0034 | < 0.05 |

Solvers, 40 passes per frame each, mean outflow after / before:

| Solver | one frame | + 120 rounds |
|---|---|---|
| Jacobi | 0.42 | 0.17 |
| red-black, factor 1.0 | 0.42 | |
| red-black, factor 1.5 | 0.35 | |
| red-black, factor 1.8 (default) | 0.32 | 0.0034 |
| red-black, factor 1.9 | 0.37 | 0.0075 at 40 rounds |
| red-black, factor 1.95 | 2.35 | |

Time per frame at 1280 × 720 on lavapipe: 76 ms; with Jacobi 98 ms; with 2
pressure passes 33 ms, so the 40 passes cost about 43 ms.

## What the numbers say

**The projection does not make the flow divergence-free. A third of the
outflow is left.** The two passes do what they should (they match the CPU to
rounding), and the solver converges when it is given more passes (0.3 % after
120 more rounds), so the method is right and 40 passes are too few for a
256-cell grid. Relaxation removes fine detail in a few passes and takes
thousands for errors as wide as the box. The largest outflow drops much
further (to 8 %) than the mean because the peaks at the jets are fine detail.

The numbers move with the state of the flow: at frame 601 the one-frame
ratio is 0.19 and the 120-round ratio 0.012. The rules have that much room.

**Over-relaxation helps less than its theory suggests within 40 passes.**
0.32 against Jacobi's 0.42. Its advantage is in the long run (0.0034 against
0.17). At 1.95 one frame is worse than no projection at all (2.35): the
factor that is best in the long run overshoots before it settles.

**In-place relaxation is cheaper per pass than Jacobi here**, 76 ms against
98 ms: each pass writes half the cells and nothing is copied.

## Not done, not tested

- No multigrid, which is the standard cure for the slow wide errors.
- The outflow that is left means fluid is not conserved. It is not measured
  how much dye that creates or destroys.
- Advection is first order, semi-Lagrangian: it blurs. No MacCormack step, no
  vorticity confinement. The fastest cell moves 3.8 cells per step, which
  this scheme tolerates and an explicit one would not.
- No obstacles inside the box, no mouse interaction.
- The `--check` passes with 40 passes only; with `--iterations 20` the ratio
  is 0.71 and the check fails, as it should.
- The picture is stretched to the window; the box is square only in a square
  window.
- Lavapipe only. Run in a window once, under Xvfb, for one screenshot (no
  validation errors); the panel's controls were not operated.

## Running

```
./build/cm/fluid                              # window
./build/cm/fluid --headless out.ppm --check   # 240 steps, then the checks
```

`--view N` (0 dye, 1 speed, 2 pressure), `--iterations K` (even, 2..48),
`--jacobi`, `--omega W`, `--frames N`.

## References

- Jos Stam, *Stable Fluids*, SIGGRAPH 1999.
- Francis Harlow, Eddie Welch, *Numerical calculation of time-dependent
  viscous incompressible flow of fluid with free surface*, Physics of Fluids,
  1965.
- Mark Harris, *Fast Fluid Dynamics Simulation on the GPU*, GPU Gems, 2004,
  chapter 38.
