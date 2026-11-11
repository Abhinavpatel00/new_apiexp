# ocean — a wave spectrum through an inverse FFT in compute

```
Spectrum (compute) ─► FFT x0 … x6 ─► FFT y0 … y6 (compute, one butterfly stage each)
  ─► Ocean (a grid displaced in the vertex stage) ─► Present
```

The sea is the sum of 16 384 waves (128 × 128), each with its own length,
direction, height and speed. Summing them directly would cost 16 384 terms
per point; the inverse FFT does all points in 14 passes.

| | technique | source |
|---|---|---|
| the waves | Phillips spectrum, Gaussian random amplitudes; every wave turns at the deep-water speed ω = √(g·k) | Tessendorf 2001 |
| choppiness | points are also moved sideways by −i·k/k·H, which sharpens crests | Tessendorf 2001 |
| transform | radix-2 butterflies, one stage per compute pass, ping-pong between two RGBA32F images; the first stage reads in bit-reversed order | Cooley & Tukey 1965 |
| packing | height + i·(displacement x) in one complex transform: both are real, so they come out as its real and imaginary part. Two transforms' worth of data in one image; the fourth channel is spare | common trick |
| drawing | N × N quads per tile from the vertex index, no vertex buffer; normals and the folding measure from neighbouring grid points | — |

Cited from memory.

## Acceptance (`--check`)

Run by `scripts/check_demos.sh` at 64 × 64; the last column is one run at 512 × 512.

| Line | Rule | 64² | 512² |
|---|---|---|---|
| **one wave (wave number 3, 2; amplitude 0.54 m), at 0.7 s and 12.9 s: height vs 2·Re(h0·e^(i(k·x − ωt))), worst** | < 2e-6 m | 4.1e-7 | 4.1e-7 |
| displacement x / z vs the closed form, worst | < 2e-6 m | 3.8e-7 / 1.4e-7 | not noted |
| the spare fourth field, largest | < 2e-6 | 1.3e-7 | not noted |
| full spectrum: spectrum pass vs the formula in double, worst as a share of the largest coefficient | < 1e-5 | 5.5e-8 (0.7 s), 1.6e-6 (12.9 s) | 7.2e-8, 1.1e-6 |
| **height vs a direct Fourier sum of that spectrum, worst** | < 2e-6 m | 2.5e-7 | 3.4e-7 |
| displacement x and z vs the direct sum, worst | < 2e-6 m | 2.7e-7 | 3.7e-7 |
| spare field vs the direct sum (it must be the imaginary part of a real field: 0) | < 2e-6 | 1.4e-7 | 3.5e-7 |
| height, standard deviation; 0.40 m asked for | within 25 % | 0.43, 0.37 | 0.41, 0.36 |
| height, mean | < 1e-6 m | 2.5e-9 | not noted |

The direct sum is the definition, Σ H·e^(2πi·jx/N), row by row and then
column by column in double precision on the CPU — N³ work and no
butterflies, bit reversal or stages in common with the shader.

## What the numbers say

- The transform is exact to single precision (3e-7 m on waves of 0.4 m) at
  every size tried, against a sum that shares no code with it.
- The one-wave test checks what the transform test cannot: that the
  spectrum means what it should. A wave given at wave number (3, 2) comes
  out with 3 crests across and 2 along, the right height and phase, and
  12.2 seconds later it has moved exactly as far as ω = √(g·k) says.
- The spare field staying at zero is the check on the packing: it is the
  imaginary part of the displacement-z transform, and would not vanish if
  the spectrum were not symmetric the way a real field needs. (The wave
  number −N/2 has no partner; its displacement is left out for that
  reason.)
- The standard deviation asked for is an average over time; a single
  moment differs by ±10 %, as expected.

## Time

lavapipe, difference of two runs of different length:

| | 64 × 36 picture, 1 tile | 1280 × 720 |
|---|---|---|
| 128² | 23 ms | 55 ms (1 tile), 227 ms (4 × 4 tiles, the default) |
| 256² | 111 ms | 786 ms (4 × 4 tiles) |
| 512² | 382 ms | 433 ms (1 tile) |

The left column is mostly the transform and the vertex stage for N² quads;
they were not timed apart. The default picture's cost is the 1.6 million
vertices of 16 tiles, each of which fetches five texels.

## Not done, not tested

- **Nothing about the look is tested**: the spectrum's shape, the shading,
  the foam. The foam term reacts to the surface folding and hardly ever
  fires at the default settings; `--height 0.8 --chop 1.4` shows a rougher
  sea.
- One patch of 64 m repeated: from high up the repetition is plain to see.
  No second, larger patch (cascades), no level of detail — every tile has
  the full grid.
- Normals are per vertex, from the 0.5 m grid; no slope transform for
  per-pixel normals, so the glitter is coarse.
- The spectrum pass recomputes cos/sin for every wave every frame; the
  butterfly weights are computed in the shader rather than looked up.
- No interaction, no shore, no depth.
- lavapipe only; the window was opened under Xvfb, the panel not operated.

## Running

```sh
./build/cm/ocean
./build/cm/ocean --height 0.8 --chop 1.4       # rougher
./build/cm/ocean --grid 256 --tiles 2
./build/cm/ocean --view 1                      # height; 2: folding
./build/cm/ocean --headless out.ppm --grid 64 --check
```

`--grid N` (power of two, 16–512), `--patch L`, `--tiles T`, `--wind V`,
`--height H`, `--chop X`, `--view N`.

## References

- J. Tessendorf, *Simulating Ocean Water*, SIGGRAPH course notes, 2001.
- J. W. Cooley, J. W. Tukey, *An Algorithm for the Machine Calculation of
  Complex Fourier Series*, 1965.
