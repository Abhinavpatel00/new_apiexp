# turing — patterns from reaction and diffusion

```
Seed ─► Step × 32 (two images take turns) ─► Shade ─► Present
```

Two chemicals on a 256² grid that wraps (the Gray–Scott model):

    u += dt (Du ∇²u − u v² + F (1 − u))
    v += dt (Dv ∇²v + u v² − (F + k) v)

v feeds on u, u is topped up at rate F, v drains at rate F + k, and u
spreads twice as fast as v (Du = 1, Dv = 0.5, dt = 1). ∇² is the
nine-point stencil: 0.2 for the four neighbours, 0.05 for the diagonals,
−1 for the cell. Starting from 28 small patches of v, the grid fills with
a pattern whose kind depends only on F and k:

| `--preset` | F | k | grows |
|---|---|---|---|
| 0 | 0.0545 | 0.062 | coral: branching stripes |
| 1 | 0.0367 | 0.0649 | spots that divide like cells |
| 2 | 0.029 | 0.057 | a maze |
| 3 | 0.030 | 0.062 | spots |
| 4 | 0.018 | 0.051 | waves that never settle |

`--map` varies them across the grid instead (k 0.052 → 0.068 left to
right, F 0.012 → 0.070 bottom to top), which puts the kinds side by side.
Because the grid wraps, the picture tiles: `--tiles 2` shows it.

## Acceptance (`--check`)

| Line | Rule | Result |
|---|---|---|
| **one step against the same arithmetic on the CPU, every cell** | < 1e-6 | 6.0e-8 |
| after 6 400 steps of coral: values that are not numbers | 0 | 0 |
| lowest / highest amount | within 0..1 | 0.004 / 0.916 |
| part of the grid with v > 0.1 (it started at 3 %) | 20–80 % | 68 % |
| spacing of the pattern, from the peak of its power spectrum | 6–30 cells | 17.1 cells |
| **diffusion alone, one Fourier wave, 384 steps: amplitude ÷ 0.25·λ³⁸⁴ − 1** | < 1e-4 | see below |
| mean amount − 0.5 | < 2e-5 | 1.3e-6 … 4.1e-6 |
| u = 1, v = 0 (nothing to react) after 4 frames: largest change | < 1e-6 | 0 |

λ = 1 + D · (0.4 (cos θx + cos θy) + 0.2 cos θx cos θy − 1) with
θ = 2π · wave number / 256 is what the stencil does to a wave in one step.

| wave | D | part of the wave left | measured ÷ predicted − 1 |
|---|---|---|---|
| (3, 0) | 1 | 0.535 | +8.9e-6 |
| (3, 0) | 0.5 | 0.732 | +3.7e-6 |
| (5, 2) | 1 | 0.133 | −5.7e-6 |
| (5, 2) | 0.5 | 0.366 | +1.9e-6 |

## What the numbers say

- **The step is the formula**: the GPU and the CPU agree to the last bit
  of single precision.
- **The diffusion is right to five digits over 384 steps**, for an
  axis-aligned and an oblique wave, for both chemicals. A wrong weight,
  a missed wrap or a swapped Du and Dv would show here.
- **Diffusion keeps the total** to 4e-6. It is not exact: each step
  rounds every cell to single precision.
- **The pattern has a scale of its own**, 17 cells, although nothing in
  the starting state has it. That is Turing's point.

## Limits

- The pattern checks (cover, spacing) only say that a pattern of a
  sensible scale grew. There is no closed form to compare it with.
- Explicit Euler with dt = 1: stable for these D, not for much larger ones.
- 256² only. On the map the patterns are a few cells wide (the map runs
  with a fifth of the diffusion to fit more of them in) and line up with
  the grid in places.
- Two-dimensional and flat; Turk's paper grows the pattern on a mesh.
- Lavapipe only; 200 frames × 32 steps take about 20 s there.

## Sources

- A. Turing, *The chemical basis of morphogenesis*, Phil. Trans. R. Soc.
  B 237 (1952) — patterns from reaction plus unequal diffusion.
- P. Gray, S. Scott, *Autocatalytic reactions in the isothermal,
  continuous stirred tank reactor*, Chem. Eng. Sci. 39 (1984) — this model.
- J. Pearson, *Complex patterns in a simple system*, Science 261 (1993) —
  the map of patterns over F and k.
- G. Turk, *Generating textures on arbitrary surfaces using
  reaction-diffusion*, and A. Witkin, M. Kass, *Reaction-diffusion
  textures*, both SIGGRAPH 1991 — reaction–diffusion as a texture
  generator. They use other reactions than Gray–Scott.
- The stencil weights, Du, Dv and the F and k values are ones in common
  circulation (Karl Sims' *Reaction-Diffusion Tutorial*, karlsims.com/rd.html;
  the presets of pmneila's WebGL Gray–Scott page). They were not derived
  here.
