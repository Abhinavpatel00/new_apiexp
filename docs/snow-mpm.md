# The material point method for snow — the paper, the technical document, and `demos/snow`

Alexey Stomakhin, Craig Schroeder, Lawrence Chai, Joseph Teran, Andrew
Selle: *A material point method for snow simulation*, ACM Transactions
on Graphics 32(4), article 102, 2013, and its companion *Material point
method for snow simulation* (technical document,
<https://www.cs.ucr.edu/~craigs/papers/2013-mpm-snow/tech-doc.pdf>).
The method was built at Walt Disney Animation Studios for *Frozen*; the
blog post at <https://sungsoo.github.io/2014/02/17/snow-simulation.html>
is the paper's abstract with remarks on the film and has no formulas.

Sources: both were read in full. The paper is at
<https://media.disneyanimation.com/uploads/production/publication_asset/94/asset/SSCTS13_2.pdf>.
Section numbers below ("section 8") are the paper's; equation numbers
1 to 4 are the technical document's.

## 1. Why snow is hard, and the idea

Snow is neither a solid nor a fluid. Squeezed a little, it springs back.
Squeezed more, it packs and stays packed, and is then harder. Pulled, it
breaks into chunks. A mesh cannot follow the breaking and merging; a
fluid solver cannot hold a shape.

The material point method keeps two descriptions and uses each for what
it is good at:

* **Particles** carry the material: mass, velocity, and how deformed the
  material is. They never need to be connected, so tearing and merging
  cost nothing.
* **A fixed grid** is where forces are worked out and velocities are
  updated. A grid is where derivatives are easy, and two bodies that
  overlap the same nodes collide without any collision detection.

Every time step copies the particles' state to the grid, updates the
grid, and copies back. The grid is then thrown away.

## 2. The material

### 2.1 Elastic and plastic deformation

Each particle has a deformation gradient F: the matrix that maps a small
arrow in the snow as it was at the start to the same arrow now. It is
split into a part the snow remembers and a part it has given up on:

    F = F_E F_P

F_E is elastic: it produces stress, and the snow would spring back from
it. F_P is plastic: permanent. All of the method's "snow-ness" is in how
deformation is moved from F_E to F_P.

### 2.2 The energy

The elastic energy per volume is the fixed corotated model,

    Ψ(F_E, F_P) = μ(F_P) ‖F_E − R_E‖²_F  +  λ(F_P)/2 (J_E − 1)²

with F_E = R_E S_E the polar decomposition (R_E a rotation, S_E
symmetric) and J_E = det F_E. The first term charges for any change of
shape other than rotation; the second for change of volume. Rotating
the snow costs nothing, as it must.

The stiffnesses depend on the plastic state:

    μ(F_P) = μ₀ e^(ξ (1 − J_P)),   λ(F_P) = λ₀ e^(ξ (1 − J_P)),   J_P = det F_P

J_P < 1 means the snow has been packed for good, and it is stiffer by
e^(ξ(1−J_P)). J_P > 1 means it has been pulled apart, and it is weaker,
so it crumbles. ξ is the hardening coefficient (10 in the paper's
reference snow). μ₀, λ₀ come from Young's modulus 1.4 × 10⁵ Pa and
Poisson's ratio 0.2; the density is 400 kg/m³.

### 2.3 Yielding

After each step F_E is decomposed into singular values, F_E = U Σ Vᵀ,
and each singular value (a stretch along one principal direction) is
forced back into

    [1 − θ_c, 1 + θ_s]        θ_c = 2.5 × 10⁻², θ_s = 7.5 × 10⁻³

The clamped matrix is the new F_E; the total F is unchanged, so what was
cut off moves into F_P = V Σ⁻¹ Uᵀ F. Snow can thus be squeezed 2.5 %
or stretched 0.75 % elastically; anything more is permanent. The two
thresholds and ξ are the artist's controls: small θ make dry powder,
large ones make wet, chunky snow.

Only the determinant of F_P is ever used (in the hardening), and it
changes by the product of (singular value before ÷ after), so the demo
stores J_P alone.

### 2.4 Stress and its derivatives (the technical document)

The technical document exists to give the derivatives an implementation
needs. With the grid nodes moved to x̂, the total energy is

    Φ(x̂) = Σ_p V_p⁰ Ψ(F̂_Ep(x̂), F_Pp),
    F̂_Ep(x̂) = (I + Σ_i (x̂_i − x_i)(∇w_ip)ᵀ) F_Ep                    (1)

V_p⁰ is the particle's volume at the start, w_ip the weight of node i at
particle p. The force on node i is minus the derivative:

    −f_i = Σ_p V_p⁰ (∂Ψ/∂F_E)(F̂_Ep) (F_Ep)ᵀ ∇w_ip                    (2)

    ∂Ψ/∂F_E = 2μ (F_E − R_E) + λ (J_E − 1) J_E F_E⁻ᵀ

For an implicit step one also needs how the forces change when the
nodes move by δu:

    −δf_i = Σ_p V_p⁰ A_p (F_Ep)ᵀ ∇w_ip                                (3)
    A_p   = ∂²Ψ/∂F_E² : (Σ_j δu_j (∇w_jp)ᵀ F_Ep)                      (4)

and the product in (4), for a change δF of F, is

    2μ δF − 2μ δR + λ JF⁻ᵀ (JF⁻ᵀ : δF) + λ (J − 1) δ(JF⁻ᵀ)

Two terms need care.

* **JF⁻ᵀ** is the cofactor matrix. Its rows are cross products of the
  rows of F, so δ(JF⁻ᵀ) is a sum of two cross products per row. No
  inverse is taken; it works for a flattened F.
* **δR**, the change of the rotation. Differentiating F = RS and using
  that RᵀδR is skew gives

      Rᵀ δF − δFᵀ R = (Rᵀ δR) S + S (Rᵀ δR)

  a linear equation for the three numbers of the skew matrix RᵀδR. If
  that skew matrix is a turn about ω, the right side is a turn about
  (tr(S) I − S) ω, so ω comes from one 3 × 3 solve, and δR = R [ω]×.

The demo implements exactly these and checks each against finite
differences: ∂Ψ/∂F against Ψ, the product against ∂Ψ/∂F, equation 2
against equation 1 on a whole snowball, equations 3–4 against
equation 2. All agree to 10⁻⁹ or better.

## 3. One time step

Weights are cubic B-splines, one per axis, multiplied:

    N(x) = ½|x|³ − x² + ⅔            for |x| < 1
           −⅙|x|³ + x² − 2|x| + 4⁄3   for 1 ≤ |x| < 2

so a particle reaches 4 × 4 × 4 nodes. They are smooth enough that the
force, which uses ∇w, does not jump when a particle crosses a cell.

1. **Particles to grid.** m_i = Σ m_p w_ip, v_i = Σ v_p m_p w_ip / m_i.
2. **Volumes (first step only).** The grid's density at a particle,
   ρ_p = Σ m_i w_ip / h³, gives its volume V_p⁰ = m_p / ρ_p.
3. **Forces** on the grid by equation 2 at x̂ = x.
4. **Velocities**: v_i* = v_i + Δt f_i / m_i, plus gravity.
5. **Collisions on the grid**, applied to v* (3.1 below).
6. **Implicit solve** (optional):
   (I + β Δt² M⁻¹ ∂²Φ/∂x̂²) v^(n+1) = v*. β = 0 is the explicit step,
   ½ the trapezoidal rule, 1 backward Euler. The matrix is never built;
   conjugate residuals only needs its product with a vector, which is
   equations 3–4. Nodes that collided in step 5 are taken out of the
   solve: they keep the velocity the collision gave them. The paper
   reports 10 to 30 iterations without a preconditioner.
7. **Deformation.** ∇v_p = Σ v_i (∇w_ip)ᵀ, F̂_E = (I + Δt ∇v_p) F_E,
   then the clamp of 2.3.
8. **Velocities back**: v_p = (1 − α) v_PIC + α v_FLIP with
   v_PIC = Σ v_i w_ip and v_FLIP = v_p + Σ (v_i − v_i^old) w_ip, α = 0.95.
   PIC alone is stable but smears motion out; FLIP alone keeps detail
   but is noisy.
9. **Collisions on the particles**, the same rule, on v_p.
10. **Move**: x_p += Δt v_p.

Why implicit: the explicit step must be short enough that sound cannot
cross a cell, and hardening makes packed snow stiff, hence fast. The
paper needs Δt of about 10⁻⁵ s for the explicit step and uses
0.5 × 10⁻³ s with the implicit one for every example.

### 3.1 Collisions (section 8)

A collider is a level set φ; a point is inside where φ < 0, and the
normal is n = ∇φ. The collider may move, so the rule is applied to the
velocity as the collider sees it, v_rel = v − v_co:

* v_n = v_rel · n ≥ 0: the snow is leaving. Nothing is done.
* Otherwise split off the part along the surface, v_t = v_rel − v_n n.
  If ‖v_t‖ ≤ −μ v_n, friction holds it: v_rel′ = 0.
* Else it slides and loses μ times what was pushed in:
  v_rel′ = v_t + μ v_n v_t/‖v_t‖.

Then v′ = v_rel′ + v_co. Nothing bounces. A **sticky** collider sets
v_rel′ = 0 whichever way the snow moves; the paper uses it so that snow
stays on walls and under overhangs (its figure 2 shows the same snowball
on a wall with and without it). Applying the rule twice, on the grid and
on the particles, is what keeps snow out of thin colliders.

### 3.2 What the paper does besides (sections 9, 10)

* **Rendering.** Particles are spread onto a grid with the same weights
  and the density is path traced as a volume, with a Henyey–Greenstein
  phase function (g = 0.5), extinction 724 m⁻¹ and albedo
  (0.9, 0.95, 1.0).
* **Seeding.** 4 to 10 particles a cell, placed at random, never
  re-seeded. Snowballs get a harder, heavier shell and noise in the
  stiffness so that they break unevenly.
* **Sizes** (table 3): 3 × 10⁵ particles on a 600 × 300 × 600 grid for a
  dropped snowball at 5 minutes a frame, up to 7 × 10⁶ particles for the
  rolling snowball at 36 minutes a frame, on 8 cores.
* **Controls** (table 2, figure 8): lower hardening ξ makes the snow
  ductile, higher brittle; lower θ_c, θ_s powdery, higher chunky; ice is
  high ξ and E₀, slush the opposite.
* **Limits the authors name**: parameters are tuned by hand, air is not
  simulated, the rendering aliases, and a GPU version is left as future
  work.

## 4. What `demos/snow` implements

| Paper / technical document | Here |
|---|---|
| steps 1, 3 | `compP2G` in `shaders/snow.slang`; `snow_p2g` in `method.c` |
| step 2 | `snow_volumes` in `method.c`, once, on the CPU |
| steps 4, 5 | inside `compG2P` (a node's velocity is worked out where it is read); `snow_velocities` |
| section 8: friction, moving collider, sticky | `collide` in the shader, `snow_collide` in `method.c`; the ball of scene 3 moves, `--sticky` makes the side walls and ceiling sticky |
| step 6, eq. 3–4 | `snow_implicit`, `snow_force_change`, `snow_stress_change` in `method.c` (CPU); collided nodes are left out of the solve; Δt = 0.5 ms, β = 1 |
| steps 7–10 | `compG2P`; `snow_g2p` |
| Ψ, ∂Ψ/∂F, eq. 1–2 | `snow_energy`, `snow_stress`, `snow_forces` |
| polar decomposition, singular values | Jacobi rotations on F_EᵀF_E: `eigen` in the shader, `snow_eigen` on the CPU |

Things worth knowing about how:

* **Units.** Lengths are cells, mass is particles, stress is divided by
  the density of fresh snow, so the only material numbers are E/ρ =
  350 m²/s² and ν.
* **Singular values without an SVD.** Because F_E is always within a few
  percent of a rotation, FᵀF is well conditioned; its eigenvectors V and
  eigenvalues σ² give the clamped F_E as F̂ V diag(clamp(σ)/σ) Vᵀ and
  R_E as F V diag(1/σ) Vᵀ. No U is needed.
* **The grid is integers** with atomic adds (mass in 2⁻²⁴ particle,
  momentum 2⁻¹⁴, dt × force 2⁻¹⁸ particle·cells/s), so the sum does not
  depend on the order the GPU happens to run particles in.
* **The implicit system is solved in y = √m v.** Node masses differ by
  millions between the middle of the snow and its rim. In v the solver
  needed more than 60 iterations for a residual of 10⁻²; in y, 19 to 24
  for 10⁻⁷. The eigenvalues are those of the paper's matrix.
* **The time steps.** The shaders' explicit step is 10⁻⁴ s, ten times
  the paper's, which a 64³ grid allows: cells are 1.6 cm, several times
  the paper's. The implicit step is the paper's 0.5 ms.
* **Depth.** The picture is discs with a depth buffer; z/w is
  a + b/distance, as it has to be for a large flat wall seen at a slant.

## 5. What the checks show

From `snow --check` (table in `demos/snow/README.md`):

* The four derivative formulas match finite differences to 10⁻¹⁰.
* The force-change operator is symmetric to 5 × 10⁻¹⁵, as the second
  derivative of an energy must be.
* All node forces add up to zero: the snow cannot push itself.
* One GPU step equals the CPU step to rounding: positions to 4 × 10⁻⁶
  cells, F_E to 7 × 10⁻⁷.
* A ball in the air falls by g Δt² n(n+1)/2 to 4 × 10⁻⁵ and does not
  deform.
* With a step of 1.5 ms (sound crosses about 2 cells), an elastic ball
  let go from a 1.5 % squeeze reaches 3 cells/s under the implicit step
  and passes 10⁵ cells/s within 60 steps under the explicit one.
* The implicit step of 0.5 ms on snow packed against a sticky wall with
  a ball moving into it: 383 nodes are held, the solve leaves them
  exactly alone, and the other nodes satisfy the equation to 10⁻⁷.
* The collision rule at 20 000 random velocities inside a moving ball:
  nothing is left moving into the ball, and the speed along it drops by
  friction × the speed into it, both to 10⁻¹³.
* A ball pushed 4.5 cells into resting snow at 300 cells/s (4.7 m/s)
  leaves no particle inside it.

## 6. Not done, or done differently

* The implicit solve is not on the GPU and is not what the demo runs by
  default; see the README for why.
* The hardening factor is bounded to [0.1, 5]. The paper does not bound
  it.
* Colliders are the six walls and one ball that may move in a straight
  line. No level sets of arbitrary shapes, no rotating or deforming
  colliders, no characters.
* All particles have the same material. The paper varies it inside a
  snowball.
* No volume rendering, no shadows. The paper's look is not reproduced.
* The paper's large scenes (millions of particles) are out of reach
  here: 11 500 to 28 000 particles in a 64³ grid.
* Results were not compared with the paper's figures or with any other
  implementation, only with the equations.
