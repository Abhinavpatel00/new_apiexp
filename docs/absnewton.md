# Absolute eigenvalue filtering — the paper and `demos/absnewton`

Honglin Chen, Hsueh-Ti Derek Liu, David I. W. Levin, Changxi Zheng, Alec
Jacobson: *Stabler Neo-Hookean Simulation: Absolute Eigenvalue Filtering
for Projected Newton*, SIGGRAPH 2024 conference papers.
<https://www.cs.columbia.edu/cg/abs-psd/>, arXiv 2406.05928.

Sources: the arXiv HTML version (v3) was read from the abstract to the
middle of section 7 (results). The end of section 7, section 8
(conclusion, limits) and the appendix, if there is one, were **not**
read. The authors' code (<https://github.com/honglin-c/abs-psd>, C++
with libigl and TinyAD) was not cloned or read: `demos/absnewton` is
written from the paper's equations. Equation and section numbers are
the paper's.

## 1. The problem

Rubber, fat and muscle hardly change volume. Models of them have a
Poisson's ratio ν near 0.5 (table 1: rubber 0.4999, fat 0.49, muscle
0.47). The paper is about finding the state of least elastic energy of
such a body (eq. 1), a *quasi-static* solve, when the starting state is
far from it.

Newton's method replaces the energy near x by a quadratic (eq. 2) and
jumps to its lowest point (eq. 3):

```
f(x + d) ≈ f(x) + gᵀd + ½ dᵀH d            d = −H⁻¹ g
```

then shortens the jump until the energy really falls (a line search).
The jump goes downhill only if H is positive definite: a bowl. If H has
a negative eigenvalue the quadratic is a saddle and its stationary
point may be uphill.

## 2. Projected Newton (3.1)

The Hessian of a finite-element energy is a sum of small matrices, one
for each element (eq. 4); for a tetrahedron 12 × 12. If every one of
them is positive semi-definite, so is the sum. So the usual fix
(Teran et al. 2005) is: take each small matrix apart into eigenvalues
and eigenvectors, raise negative eigenvalues to zero or a small ε
(eq. 5), put it together again, and add up (eq. 6). This is cheap
because the matrices are small, and it is what most simulators do.

## 3. What goes wrong with clamping (4)

Raising a negative eigenvalue to zero turns a saddle into a *valley*:
flat along that eigenvector. Newton's step divides by the eigenvalue,
so the flat direction, the one in which the true energy is most
strongly curved the wrong way and so least like the quadratic model,
is the direction the step now prefers.

The paper's example (eq. 7) is the sum of two squared distances to unit
circles about (−1, 0) and (1, 0). At (1 − 10⁻⁶, 10⁻⁸) the Hessian has
eigenvalues −1.99·10⁶ and 3.99 (eq. 8). With the first clamped to 10⁻³
the step is −(1.19, 19.99) (eq. 9): twenty units sideways, in the
direction the model knows least about. The demo's check reproduces
these numbers.

## 4. The fix (5)

Use the absolute value instead (eq. 10):

```
Λ, U = eig(H_i)
H_i⁺ = U |Λ| Uᵀ          instead of      U max(Λ, 0) Uᵀ
```

A strongly negative curvature now becomes a strongly *positive* one, so
the step in that direction is short: the more wrong the model is along
a direction, the less the step trusts it. In the example the step
becomes −(0.99, −0.01) (eq. 11), straight towards the minimum at the
origin. The paper notes that this is known for the *whole* Hessian
and cites earlier work on it; there
it costs a full eigendecomposition; doing it for each element costs
nothing more than clamping does.

In `method.c` this is the one line in `nh_filter`:

```c
values[k] = filter == NH_ABS ? fabs(values[k]) : fmax(values[k], epsilon);
```

## 5. Why Neo-Hookean bodies have large negative eigenvalues (6)

The stable Neo-Hookean energy density (eq. 13, after Smith et al. 2018):

```
Ψ = μ/2 (I_C − 3) + λ/2 (J − α)²        I_C = |F|²,  J = det F,  α = 1 + μ/λ
```

F is the deformation gradient, J the change of volume. Six of the nine
eigenvalues of ∂²Ψ/∂F² are (eq. 14 to 19)

```
μ ± σ_k (λ (J − 1) − μ)        σ_k the singular values of F
```

λ = 2νμ / (1 − 2ν) goes to infinity as ν goes to ½: at ν = 0.495 it is
about 100 μ. So an element whose volume is off by (J − 1) has
eigenvalues near ∓100 μ σ (J − 1): large and negative as soon as the
volume change is large. High Poisson's ratio and large volume change
together are exactly when clamping throws away large numbers.

The demo's check draws 500 random F and finds all six numbers among the
eigenvalues of its Hessian to 2·10⁻¹².

## 6. The demo

`method.c`: tetrahedra with linear shape functions; the energy, its
gradient and each element's Hessian written out by hand (the Hessian in
F is `μ I + λ (∂J)(∂J)ᵀ + λ (J − α) ∂²J`, and `∂²J/∂F_ab∂F_cd = ±F_ef`
because the determinant is linear in each entry); Jacobi rotations for
the eigenvalues; a dense Cholesky factorisation; a backtracking line
search. As in the paper there is no test for
inverted elements in the line search (figure 3 shows it stalling the
solve), and clamping uses ε = 0. The stop test is the paper's: Newton
decrement ½ dᵀg below 10⁻⁵ λ.

## 7. Results, and where they differ from the paper

Iterations for a bar of 10 × 3 × 3 cubes (540 tetrahedra), or
16 × 5 × 5 (2 400):

| start | Poisson | cubes along | clamped | absolute |
|---|---|---|---|---|
| every node moved, 3 lengths | 0.3 | 10 | 5 | 5 |
| | 0.45 | 10 | 9 | 7 |
| | 0.495 | 10 | 12 | 8 |
| | 0.499 | 10 | 15 | 9 |
| | 0.495 | 16 | 15 | 8 |
| every node moved, 1.2 lengths | 0.495 | 10 | 6 | 3 |
| squeezed to half | 0.495 | 10 | 5 | 5 |
| only the end moved, 3 lengths | 0.495 | 10 | 11 | 16 |
| | 0.495 | 16 | 21 | 17 |
| only the end moved, 2 lengths and turned | 0.495 | 10 | 11 | 14 |

* **Agrees with the paper:** nothing to gain at ν = 0.3; the gain grows
  with ν (figure 4) and with resolution (figure 14).
* **Does not show the paper's headline:** clamping never diverged or
  stalled here. In the paper's figure 1 it does; the paper's meshes
  are far larger than a dense solver allows.
* **Opposite to the paper on a coarse mesh:** with only the end face
  moved, which is how the paper makes its starting states, the absolute
  filter needs *more* iterations on the 540-element bar (16 against 11)
  and fewer only on the finer one (17 against 21). The absolute filter
  shortens steps in directions that clamping leaves long; where the
  long step happens to be right, that costs iterations.
* **The two-variable example:** the directions of eq. 9 and 11 match.
  The iteration counts of figure 8 (3 and 11) do not: here 6 and 93,
  stopping at a gradient below 10⁻⁸. The paper's stop rule for the
  figure is not stated in the part read. The minimum is at the origin,
  where the circles touch and the function is flat to fourth order
  sideways, so a path that arrives from the side, as the clamped one
  does, converges slowly.
* With `--tolerance -12` both filters go on for more than 45 iterations
  with full steps and a slowly falling decrement. Neither reaches
  Newton's quadratic convergence on scene 1. The paper's stop test is
  met long before that, so the table above counts the fast first phase.
  (Probably because some elements keep negative eigenvalues at the
  solution, so the filtered matrix is never the true Hessian; this was
  not verified.)

## 8. A side result: eq. 13's small-strain behaviour

Pulling a bar by 0.1 % with its sides free, it narrows by 0.1668 of the
stretch with ν = 0.3 put in. Linearising eq. 13 gives
`μ ε:ε + (λ − μ)/2 tr(ε)²`: Hooke's law with λ − μ in place of λ, so
the ratio is (λ − μ) / 2λ = 1/6, which the check confirms to 0.1 %.
Eq. 13 with the plain Lamé parameters is thus softer against volume
change at small strain than the ν that was put in suggests. Smith et
al. 2018 is the place to look for how the parameters should be mapped;
it was not read here, and the demo uses eq. 13 as this paper prints it.

## 9. What was not implemented

Sparse matrices and large meshes; energies other than eq. 13;
collisions; the paper's comparisons with other ways of making the
Hessian positive definite; analytic eigendecompositions. (The figure
numbers of these parts are not given here: they lie in the part of the
paper that was not read or could not be re-checked.)
