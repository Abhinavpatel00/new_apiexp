# Go Green — the paper and `demos/gogreen`

Source. Jiong Chen and Mathieu Desbrun, "Go Green: General Regularized
Green's Functions for Elasticity", SIGGRAPH 2022 Conference Proceedings
(`https://jiongchen.github.io/files/gogreen-paper.pdf`); sections 1 to 3
read, the supplemental material not. The authors' C++ code
(`https://gitlab.inria.fr/geomerix/public/gogreen`) was not looked at.
Equation numbers are the paper's.

## 1. The problem

Push a point inside a block of rubber: everything around it moves a
little, less with distance. For an infinite, isotropic body that
displacement field has a formula (Kelvin's solution), and de Goes and
James built sculpting brushes on it ("Kelvinlets"): grab, scale, twist,
pinch, without a mesh or a solver. Real materials and artistic intent are
often anisotropic: wood, cloth, muscle are stiffer in one direction. For
them no formula exists. The paper gives a representation that is nearly as
cheap as the formula.

## 2. The equation (section 2.1)

Linear elasticity for a displacement `u` under a load `f` (eq. 1):

    C_ijkl ∂²u_k / ∂x_l ∂x_j + f_i = 0

`C` is the elasticity tensor: 21 independent numbers in general, two for
an isotropic material. The Green's function `G` (a 3 × 3 matrix at every
point) is the answer to a unit load in each direction, with the load not
a point but a smooth bump `g_ε` of width `ε` (eq. 2), so that `G` stays
finite at the centre. A load `h` at `x'` then gives `u(x) = G(x − x') h`
(eq. 6).

In Fourier space the equation is algebraic (eq. 3):

    Ĝ(ξ) = (C_ijkl ξ_j ξ_l)⁻¹ ĝ_ε(ξ)

The matrix `A(ξ) = C_ijkl ξ_j ξ_l` is homogeneous of degree 2 in `ξ`, so
its inverse is `A(ξ/|ξ|)⁻¹ / |ξ|²`: a function of direction over a
function of length. Transforming back (eq. 4) is the hard part.

## 3. Three factors (section 2.2)

A plane wave is a sum of spherical waves (eq. 7): products of a spherical
Bessel function `j_l` of the distances and spherical harmonics of the two
directions. Put into eq. 4, the integral over `ξ` splits into one over
its length and one over its direction, and (eq. 9, 10)

    G(x) = 1/(2π²) Σ_l Σ_m  i^l  Y_lm(x/|x|)  R_l(|x|)  P_lm(C)

    R_l(r)  = ∫₀^∞ ĝ_ε(ξ) j_l(r ξ) dξ               (the load's shape)
    P_lm(C) = ∫_sphere A(ξ)⁻¹ conj(Y_lm(ξ)) dS      (the material)

Only even degrees `l` are non-zero. For an isotropic material only
degrees 0 and 2 are, and the sum is Kelvin's solution; the paper states
this and the check confirms it. For other materials the sum is cut at a
degree `l_max`; the paper uses 8, and 20 for extreme anisotropy, and
calls a relative error below 1e-2 visually indistinguishable.

`gg_material` computes the `P_lm`, `gg_green` evaluates the sum.

Two changes that give the same `G`. The demo uses real harmonics: the
sum over `m` of `Y_lm(a) conj(Y_lm(b))` is the same with real ones, `P`
becomes real, and `i^l` is ±1 on even degrees, so the "real part" of
eq. 6 has nothing to remove. And the sphere is integrated with
Gauss-Legendre points in `cos θ` times equally spaced angles (48 × 96)
instead of the paper's Lebedev points.

## 4. The load (section 2.4)

For a Gaussian load `g_ε = exp(−r²/ε²) / (ε³ π^{3/2})`, with transform
`exp(−ε² ξ²/4)`, the radial functions have a closed form (eq. 13) with
Kummer's hypergeometric function:

    R_l(r) = √π/2 · r^l ε^{−l−1} Γ((l+1)/2) · ₁F̃₁((l+1)/2; l + 3/2; −r²/ε²)

`R_0` is `π erf(r/ε) / (2r)`: finite at the centre, `π/(2r)` far away.
All others are zero at the centre. `kummer` in `method.c` evaluates the
function with the transformation that makes all terms positive, and with
its asymptotic series beyond `r/ε ≈ 9`.

The paper's point is that `R_l` can also be chosen directly. Any curve
for `R_l` defines some regularised load, possibly a complicated one, and
the artist edits the curves (as cubic splines) to shape how the
deformation falls off. The demo has only a factor on each of the two
lowest radial functions (`--low`, `--next`), which is what the paper's
figure 7 does with its weights.

## 5. Affine loads (section 2.3)

Scale, twist and pinch are not forces but force pairs, described by a
3 × 3 matrix `H`: `u = ∇G : H` (eq. 11), with `H` a multiple of the
identity for scale, skew-symmetric for twist, traceless for pinch. A
derivative is a factor `i ξ_p` in Fourier space, so `∇_p G` has the same
three-factor form (eq. 12) with odd degrees, a radial function with one
more power of `ξ` (the second line of eq. 13) and matrices that carry an
extra `ξ_p`. `gg_gradient` is eq. 12.

## 6. Handles (the paper's "constrained deformation")

To make `k` points move by given amounts, use `k` loads at those points
and solve the `3k × 3k` linear system whose blocks are `G(x_i − x_j)`.
`gg_fit` does that with Gaussian elimination. The grab brush is the case
`k = 1`.

## 7. The demo

The grid is the slice z = 0 of the infinite body; only the in-plane part
of the displacement is drawn (the materials are turned about z only, so
for the loads used the slice stays in its plane). Materials, chosen for
this demo and not taken from the paper: isotropic; cubic with
`C11 : C12 : C44 = 2.23 : 1.61 : 1`; and a transversely isotropic one
with `C11 = 20` along the fibres against 2.5 across. Affine brushes are
scaled so that the largest displacement on the grid is 0.12.

## 8. What the check measures

Numbers are in `demos/gogreen/README.md`.

1. The harmonics satisfy the addition theorem.
2. Eq. 13 equals the integrals it was derived from, by Simpson's rule
   with an independent spherical Bessel function.
3. Isotropic: the higher material matrices vanish; `G` is Kelvin's
   solution far from the load. (At distance `r` a Gaussian load differs
   from a point load by a term of relative size `ε²/r²`, so the
   comparison is made 980 widths away.)
4. Anisotropic, far from the load: against
   `1/(8π² r) ∮ A(ξ)⁻¹ dφ` over the unit circle perpendicular to `x`,
   which follows from eq. 4 for a point load and is computed without
   `method.c`.
5. Near the load: the residual of eq. 2 by differences.
6. The gradient against differences; the handles; the quadrature.

## 9. Agreement with the paper

* Isotropic materials need degrees 0 and 2 (1 and 3 for affine loads)
  only, and give Kelvin's solution: confirmed.
* Cutting at degree 8: 0.64 % and 0.55 % far-field error for the two
  anisotropic materials here, under the paper's 1e-2. The paper measures
  its error over a region and for other materials (its figures 9 and 12),
  so the numbers are not the same quantity.
* The paper's timing (under 0.015 ms a displacement at degree 8) was not
  measured against.

## 10. Not implemented

Spline editing of the radial functions; elasticity tensors from
homogenised two-material patterns (the paper's figure 12); 3-D models;
the Kelvinlet load `15 ε⁴ / (8π (r² + ε²)^{7/2})` that the paper
mentions; low-rank updates for added handles; a GPU version (the paper
suggests one).
