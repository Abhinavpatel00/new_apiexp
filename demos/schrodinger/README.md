# schrodinger — one quantum particle and two slits

```
Seed ─► Half ─► (StepR ─► StepI) × 20 ─► Shade ─► Present
```

The wave function ψ = R + iI of one particle on a 384 × 216 grid
(ħ = m = 1, one cell = one unit of length):

    i ∂ψ/∂t = −½ ∇²ψ + V ψ

A Gaussian wave packet with wave number 0.7 (wavelength 9 cells) is thrown
at a wall with two slits 48 cells apart. A plate 163 cells behind the wall
adds up |ψ|² over time, and the bar at the right of the picture shows what
it has collected. Colour is the phase of ψ, brightness is |ψ|².

`--slits 1` closes one slit, `--slits 0` removes the wall, `--view 2`
shows the real part alone, `--hard-walls` lets the edge of the grid
reflect instead of absorb.

## The method

Visscher's scheme keeps R at whole time steps and I at half steps:

    R(t + dt)    = R(t)        + dt · H I(t + dt/2)
    I(t + 3dt/2) = I(t + dt/2) − dt · H R(t + dt)        H = −½∇² + V

Each line is one compute pass that reads the other part and updates its
own image in place, so there is no second copy of the state. The `Half`
pass after `Seed` moves I half a step forward. ∇² is the fourth-order
stencil (−1/12, 4/3, −5/2, 4/3, −1/12) on each axis; dt = 0.2 (stable
below 2 / (16/3 + V) = 0.27 with the wall at V = 2). In a border of 24
cells the wave is damped so that it does not come back.

The scheme keeps Σ R(t)·R(t + dt) + I(t + dt/2)² constant exactly (in
exact arithmetic): the particle is somewhere with probability one.

## Acceptance (`--check`)

| Line | Rule | Result |
|---|---|---|
| **two slits: bright bands on the plate** | ≥ 5 | 5 |
| the middle band, cells from the axis | < 0.5 | 1e-6 |
| **the bands next to it ÷ where one wavelength of path difference puts them − 1** | within 5 % | −1.9 % (30.8 cells against 31.4) |
| dark band between them ÷ the middle band | < 0.1 | 0.028 |
| dark rows (under a tenth of the brightest) within 70 cells of the axis | ≥ 10 | 20 |
| **one slit: dark rows within 70 cells of the axis** | 0 | 0 |
| brightest row, cells above the axis (the slit is at 24) | 15–40 | 29.5 |
| **free packet, 1 201 steps: what is kept ÷ at the start − 1** | < 1e-5 | −1.7e-8 |
| width at the start (8 asked) | 7.99–8.01 | 8.000 |
| **speed ÷ group velocity of the grid − 1** | < 1e-3 | 1.5e-5 |
| speed ÷ momentum − 1 | reported | −0.78 % |
| sideways drift | < 0.01 cells | 5e-7 |
| **growth of the variance ÷ t²·var(v) − 1, along / across the motion** | within 1 % | 0.05 % / 0.11 % |
| width across at the end ÷ sqrt(8² + (t/16)²) − 1 | reported | 0.04 % |

Where the bands should be: a point is bright where its distances to the
two slits differ by n wavelengths. That is a hyperbola, which meets the
plate at y = a·sqrt(1 + L²/b²) with a = nλ/2 and b² = (48/2)² − a².

Group velocity of this grid and time step, for wave number k along an
axis: ω = 5/4 − 4/3 cos k + 1/12 cos 2k, v = (4/3 sin k − 1/6 sin 2k) /
sqrt(1 − (ω·dt/2)²). The packet holds wave numbers spread as a Gaussian
of width 1/(2σ); the check averages v and v² over them.

## What the numbers say

- **Two slits make five bands; one slit makes none.** With one slit the
  plate is lit smoothly; open the second and rows that were lit go dark
  (to 3 % of the middle band). Nothing but adding two waves does that.
- **The bands are where the path difference says**, to 2 %. The slits are
  8 cells wide, not points, and L is measured from the middle of the wall.
  Which of the two the 2 % comes from was not looked into.
- **Nothing is lost**: 2e-8 over 1 201 steps, which is single-precision
  rounding.
- **The packet moves at the group velocity and spreads as it must.** A
  packet 8 cells wide holds a spread of momenta 1/16, so it is 17 cells
  wide after 240 time units: the uncertainty principle, measured to 0.1 %.
- **The grid makes the particle 0.8 % slow** at this wavelength (9 cells).
  The check compares with the grid's own dispersion, so this is reported,
  not hidden. With the usual second-order stencil it would be 8 % (sin 0.7 against 0.7).

## Limits

- The check of the scheme is against the scheme's own dispersion relation
  and against textbook formulas; there is no independent solver.
- The absorbing border is plain damping. It reflects a little at grazing
  angles: with one slit the plate shows ripples of a few percent near the
  top and bottom edges.
- The plate adds up |ψ|², not the flow through it, and it does not
  absorb: the wave passes on through it into the border.
- One particle, no spin, two dimensions, fixed grid and time step.
- Single precision. R and I are a half step apart, so the |ψ|² shown is
  off by terms of order dt².
- Lavapipe only; a 110-frame run (2 200 steps) takes about 12 s there.

## Sources

- P. B. Visscher, *A fast explicit algorithm for the time-dependent
  Schrödinger equation*, Computers in Physics 5 (1991) — the scheme and
  its conserved sum.
- R. Feynman, *Lectures on Physics* vol. III, ch. 1 — the experiment.
- Not from a SIGGRAPH paper. The SIGGRAPH work nearest to it is
  A. Chern et al., *Schrödinger's Smoke* (2016), which uses a
  Schrödinger equation to simulate fluids; that is a different method
  and is not implemented here.
