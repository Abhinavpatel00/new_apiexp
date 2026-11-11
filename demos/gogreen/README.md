# gogreen — sculpting brushes from the Green's functions of anisotropic elasticity

```
(method.c on the CPU: material matrices once, then a short sum at every point) ─► Draw ─► Present
```

Chen and Desbrun: *Go Green: General Regularized Green's Functions for
Elasticity* (SIGGRAPH 2022).

A grid in the plane z = 0 of an infinite elastic body is displaced by a
load at its centre. The orange line is the direction of the material's
x axis (the fibres).

| option | |
|---|---|
| `--material 0..2` | 0 isotropic, 1 cubic, 2 stiff fibres (default) |
| `--brush 0..4` | 0 grab, 1 scale, 2 twist, 3 pinch, 4 five handles |
| `--angle A` | the material turned about z, degrees (30) |
| `--pull A` | direction in which the centre is pulled, degrees |
| `--epsilon E` | width of the Gaussian load (0.25) |
| `--lmax L` | highest degree of the harmonics (8) |
| `--low W`, `--next W` | factors on the two lowest radial functions of the brush |
| `--poisson N` | Poisson's ratio of the isotropic material (0.3) |

## The method

For an isotropic material the displacement caused by a point force has a
formula (Kelvin's solution; the Kelvinlets brushes use it). For an
anisotropic one it does not. The paper writes it as a sum over spherical
harmonics in which every term is a product of three things: the harmonic
of the direction, a function of the distance that depends only on the
shape of the load, and a 3 × 3 matrix that depends only on the material
(eq. 10). The matrices are integrals over the sphere, computed once. Then
the displacement at a point is a sum of 45 terms (degrees 0 to 8).
`docs/gogreen.md` goes through it.

## What `--check` measures

40 assertions, about 2 s.

* Harmonics: the addition theorem holds to 4e-14 for degrees 1 to 21.
* Eq. 13 (the radial functions of a Gaussian load) against Simpson's rule
  on the integrals they stand for: 3e-13 relative, for loads and for
  affine loads.
* Isotropic: only degrees 0 and 2 (affine: 1 and 3) are non-zero, to
  1e-14; far from the load G equals Kelvin's solution to 3e-7.
* Far from the load, the truncated sum against a direct integral
  (60 directions), relative error:

  | degrees up to | 4 | 8 | 12 | 20 |
  |---|---|---|---|---|
  | cubic | 4.3 % | 0.64 % | 0.12 % | 0.004 % |
  | fibres | 1.7 % | 0.55 % | 0.16 % | 0.02 % |

* The elasticity equation itself (eq. 2) near the load, by differences:
  residual over the load's peak 9e-5 (cubic) and 8e-5 (fibres) with
  degrees up to 8, 6e-9 with degrees up to 20.
* Eq. 12 (the gradient) equals differences of eq. 10 to 5e-10.
* Five handles: every prescribed displacement is met to 1e-16.
* Material matrices from 48 × 96 quadrature points against 96 × 192:
  2e-9 or less.

## What this says

The paper's choice of degree 8 gives an error below 1 % for both
anisotropic materials here, which is the level it calls visually
indistinguishable. The isotropic case reproduces Kelvin's solution, as
the paper states.

## Not done

Editing the radial functions as splines (only a factor on each of the two
lowest ones), homogenised two-material patterns as the source of the
elasticity tensor, deforming a 3-D mesh, Lebedev quadrature (a
Gauss-Legendre grid here), the Kelvinlet regularisation, low-rank updates
when handles are added. The supplemental material was not read.
