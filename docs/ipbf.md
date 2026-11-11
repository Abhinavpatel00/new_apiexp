# Implicit Position-Based Fluids — the paper and `demos/ipbf`

Sources. Diaz, Hsu, Montalvo-Ruiz, Giles, Yuksel, "Implicit Position-Based
Fluids", SIGGRAPH Asia 2025 Conference Papers
(`https://graphics.cs.utah.edu/research/projects/ipbf/ipbf.pdf`), read in
full. For the comparison: Macklin and Müller, "Position Based Fluids"
(ACM TOG 2013, `https://mmacklin.com/pbf_sig_preprint.pdf`), section 3
(eq. 1 to 12) and its Algorithm 1. No code of the authors was
available. Equation numbers below are the IPBF paper's unless "PBF" is
written.

## 1. The problem

A fluid of particles should keep its density. Position based fluids move
particles until a density constraint holds, which is fast, but how stiff
the result is depends on the iteration count and the time step. Implicit
pressure solvers for SPH are accurate but can blow up when they are given
few iterations. The paper wants a solver that stays stable with two or
three iterations and large time steps.

## 2. The idea

Write the step as the minimisation that implicit Euler is (eq. 1, 2):

    x = argmin  1/(2h²) |x − y|²_M + E(x),      y = xᵗ + h vᵗ + h² a   (eq. 3)

with the energy of a stiff spring on every density constraint (eq. 4 to 6):

    E = Σ ½ k C_i²,      C_i = ρ_i / ρ0 − 1,

`C_i` set to zero where it is negative, which the paper calls the typical
way to avoid clumping. Dividing by `k` and writing `alpha = 1/k` gives
(eq. 7)

    alpha/(2h²) |x − y|²_M + ½ Σ C_i²

and `alpha = 0` is then allowed: an infinitely stiff fluid, and the
problem is to bring every `C_i` to zero starting from `y`. The paper uses
`alpha = 0` for most of its results and says that convergence is not
guaranteed then.

## 3. One particle at a time (sections 3.3, 3.4)

For particle `i` with all others held, the energy that depends on it is
(eq. 8)

    alpha m_i/(2h²) |x_i − y_i|² + ½ Σ_{j ∈ P_i} C_j²

`P_i` are the particles within a kernel radius of `i`, that is, every
constraint whose density `i` adds to, its own among them. One Newton step
for it is `H_i Δx_i = f_i` (eq. 9) with the force (eq. 10) and Hessian
(eq. 11) of that energy; the density is `ρ_i = Σ m_j W(x_i − x_j)`
(eq. 12) with a cubic spline kernel and its derivatives give those of `C`
(eq. 13). In the paper `H_i` is 3 × 3 (eq. 14); in the plane it is 2 × 2.

Going through particles one after the other would be too slow on a GPU, so
all particles compute their step from the same positions and apply half of
it: `x_i ← x_i + Δx_i / 2` (section 3.3).

`fl_local` in `method.c` is eq. 10 and 11, `fl_step` Algorithm 1.

**Units of alpha.** The paper gives `alpha` as numbers (0, 1e-6, 1e-3 …)
for its particle size 0.5 and does not give the mass, so its numbers do
not carry over. The demo's `--compliance c` means
`alpha = c × S / (m × 240²)`, where `S` is the size of the pressure term of
the Hessian for a particle in a lattice at rest. `c = 1` therefore makes
inertia weigh as much as pressure at a step of 1/240 s.

## 4. The Hessian (section 3.5)

For one constraint (eq. 15)

    ∂²E_j/∂x_i² = (∂C_j/∂x_i)ᵀ (∂C_j/∂x_i) + C_j ∂²C_j/∂x_i²

The first term is positive semi-definite. The second is not, and the paper
replaces it by a diagonal matrix made of the norms of its columns (after
Andrews et al. 2017), so that `H_i` can always be inverted.

The check finds 352 of 1024 particles with an indefinite exact Hessian in
the block at seven times the rest density, and none with the
approximation. Run with the exact term (`--exact`) the block throws
particles out at 407 m/s instead of 21: the paper's figure 15.

**One choice the paper leaves open.** When `C_j` is clamped to zero, the
second term vanishes. Whether the first term stays is not said. Read
literally, eq. 13 and 15 have no case for the clamp, so the first term
stays for every neighbour; that is what the demo does. Leaving clamped
constraints out (`--active`) gives the exact Hessian of the clamped
energy, but then a particle with only two or three active constraints
around it solves for them exactly with a nearly singular matrix, and a
column at rest swells to 1.49 times its height. This was this demo's first
version and looked like a mistake in the method; it was the reading.

## 5. Damping (section 3.6)

With `alpha = 0` the solver adds energy (the paper lists this as its
limitation). The paper's remedy: during the last iteration, compute a
second position `x_i*` from the same start with a larger compliance
`alpha*`, turn both into velocities (eq. 16), and where the stiff solution
is the faster one, take kinetic energy off it (eq. 17), by a share `d`
that fades with the distance between the two solutions (eq. 18,
`beta = 60` kernel radii). The paper uses `alpha* = 1/1000`; the demo
uses compliance 1 in the units of section 3. In a trial outside the check
with the column at rest, 1, 10 and 100 left the fastest particle at 0.15
to 0.20 m/s, and 0.1, 0.01 and 0.001 at 1.3, 1.7 and 2.1 m/s.

In the check, a column at rest without the damping has particles moving at
3.3 m/s after four seconds, with it at 0.15 m/s.

## 6. Position based fluids, for comparison

PBF uses the same constraint (PBF eq. 1, 2). Each iteration computes one
multiplier a particle, `λ_i = −C_i / (Σ_k |∇_k C_i|² + ε)` (PBF eq. 11),
and moves particle `i` by `1/ρ0 Σ_j (λ_i + λ_j) ∇W` (PBF eq. 12). The
demo's version uses the cubic spline for both the density and the
gradient (the PBF paper uses two different kernels), clamps `C` the same
way, and leaves out the tensile correction, vorticity confinement and
viscosity of the PBF paper. `ε` is 0.01 of `S`. It is a baseline, not a
tuned implementation.

## 7. The demo

Box 4 × 3 m, particle spacing 0.05 m, kernel radius 0.1 m (the paper's
ratio: diameter 0.5, kernel size 1), gravity 9.81 m/s². Neighbours are
found with a grid of cells one kernel radius wide.

Walls are not described in the paper. Here three layers of fixed particles
outside the box add to the density of the fluid, and a position that still
gets past a wall is mirrored back in. (Clamping it onto the wall put many
particles on one point in a corner, where the kernel's gradient is zero
and nothing separates them: in an early version the mean density went to
about 100 times rest at 1/30 s.)

## 8. What the check measures

See `demos/ipbf/README.md` for the tables. In short:

1. The kernel integrates to 1 and its derivatives match differences.
2. Eq. 10 and eq. 11 (exact) match differences of eq. 7.
3. Indefinite exact Hessians exist; the approximated ones are positive
   definite.
4. The energy of eq. 7 falls with every doubling of the iterations.
5. A column at rest: density error by iterations, with and without the
   damping, with `--active`, and for compliances 0.1, 1, 10 (the column
   sinks to 0.90 of its height at 10).
6. Two columns at 1/240 to 1/30 s with 2 iterations.
7. The block at seven times the rest density.

## 9. Where this agrees with the paper and where it does not

Agrees:

* Stable at 1/240 to 1/30 s with 2 iterations, with heavy compression at
  1/30 s (33 % here). The paper's figure 6 shows the same in kind.
* The packed block comes apart without an explosion (densest 1.22 after 20
  steps, fastest particle 21 m/s; PBF 59 m/s).
* Without the approximation of section 3.5 it is unstable; without the
  damping at `alpha = 0` a fluid does not come to rest.
* A larger `alpha` gives a softer fluid.

Does not agree, or could not be compared:

* **Density error against PBF.** The paper's table 1 gives IPBF an error
  of 9.2e-4 with 2 iterations against 5.6e-3 for PBF with 4. Here both
  have the same error at the same iteration count and 1/240 s (0.21 % at 4
  iterations at rest), and at larger steps PBF has the smaller error,
  though it has not settled after 3 s. The scenes, dimension and PBF variant differ
  from the paper's, so this neither confirms nor contradicts the table.
* **Convergence.** The paper's figures 9 and 10 show convergence in under
  ten iterations. In the one-step test here 8 iterations leave 16 % of the
  energy and PBF is lower from 8 iterations on.
* The paper's timings (CUDA, RTX 4090, up to a million particles) have no
  counterpart: this runs 1600 particles on one CPU core.

## 10. Not implemented

Three dimensions; the GPU; viscosity (the paper puts it into `a` of eq. 3);
the comparisons with IISPH, SISPH and DFSPH; a surface for rendering.
