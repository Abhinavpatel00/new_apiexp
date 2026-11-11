# Points as Tori — the paper, and what `demos/pat` does with it

Nicole Feng, Ioannis Gkioulekas, Keenan Crane: *Points as Tori: Signed
Distance Approximation via Second-Order Proxy Geometry*, ACM Transactions
on Graphics 45(4), article 53, July 2026 (arXiv 2607.16946, CC BY 4.0).
Code and trained weights: <https://github.com/nzfeng/points-as-tori> (MIT).

This note explains the paper from the ground up, then says exactly what
the demo implements, what it checks, and what it leaves out.

## 1. The problem

Given points p₁ … p_N on a surface, each with a unit normal n_i, find a
function φ(x) that is the signed distance to that surface: negative
inside, positive outside, |φ| the distance to the nearest surface point.
With φ in hand, a level set φ = c is an offset surface, min and max of
two φ's are union and intersection, a ray can be marched to the surface
by stepping φ at a time, and so on.

The surface itself is not known. Only the samples are. So the question
is what to put *between* the samples.

## 2. Where the formula comes from

### 2.1 Distance as the limit of a smooth problem

Distance u to a surface solves the eikonal equation |∇u| = 1 with u = 0
on the surface. It is non-linear and its solution has creases. Adding a
little diffusion, |∇u|² − (1/λ) Δu = 1, smooths it, and the substitution
w = e^(−λu) (Hopf–Cole) turns the smoothed equation into a *linear* one,
the screened Laplace equation

    Δw = λ² w,        w = 1 on the surface.

As λ → ∞ the diffusion vanishes and −(1/λ) log w → u.

The signed version puts the normals on the right-hand side: a screened
Poisson equation Δw − λ²w = −2 (∇·n) μ, with μ the surface measure. Its
solution is an integral over the surface against the kernel of the
screened Laplace operator, the Yukawa potential

    G_λ(r) = e^(−λr) / (4πr),

and the double-layer form of that integral has the kernel
(λr + 1) ⟨x − y, n_y⟩ e^(−λr) / (2πr³). Setting λ = 0 gives back the
generalized winding number; λ > 0 makes far parts of the surface fade
exponentially.

### 2.2 Laplace's method

An integral ∫ h(z) e^(−λ f(z)) dz with large λ is governed by the place
where f is smallest. With f(z) = ‖x − z‖ that place is the surface point
nearest to x. So for any reasonable h,

    −(1/λ) log ∫ h(z) e^(−λ‖x − z‖) dz  →  distance from x to the surface

(the paper's equation 16). All the earlier kernel methods are members of
this family and differ only in h. The paper's observation is that one
can go a step further and skip the logarithm: take an *average* instead
of a log-sum, with weights that single out the nearest sample,

    φ(x) = Σ_i g_i(x) e^(−λ‖x − p_i‖)  /  Σ_i e^(−λ‖x − p_i‖)        (1, 17)

where g_i(x) is the signed distance from x to a simple shape that stands
in for the surface near p_i: a *proxy*. As λ → ∞ this is g of the
nearest sample. For finite λ it blends neighbouring proxies smoothly.

### 2.3 Why the proxy matters

Everything now rests on how well g_i matches the true distance near p_i.

* A **plane** through p_i with normal n_i, g_i = ⟨x − p_i, n_i⟩, matches
  position and normal: first order. Between samples of a curved surface
  the planes are off by (curvature × spacing²)/2, and far from the
  surface they are off by much more, because a plane does not know that
  the surface bends back. This is the baseline called SSPD in the paper
  (equation 29) and `--blend 1` in the demo.
* A shape that also matches the two principal curvatures is second
  order: off by spacing³ near the sample.

The simplest closed shape with two independent curvatures and an exact,
cheap distance function is a **torus**: curvature 1/r around the tube
and, on the outer equator, 1/(R + r) along it. A sphere (both equal), a
cylinder (one zero) and a plane (both zero) are limits of it.

## 3. From a sample to its torus

### 3.1 The local polynomial

Around p_i choose a frame: n_i, and two tangents s_i, t_i. The released
code uses s = normalize(n × e), t = n × s with e = (1,0,0), or (0,1,0)
when |n.x| > 0.9. The surface near p_i is written as a height over the
tangent plane,

    Q*(s, t) = p_i + s·s_i + t·t_i + Q(s, t)·n_i
    Q(s, t)  = a00 + a10 s + a01 t + a11 s t + a20 s² + a02 t²          (18)

Six numbers. a00 lets the surface miss the sample (noise), a10 and a01
let it tilt against the given normal, the last three bend it.

### 3.2 Its curvatures

With A = √(1 + a10² + a01²), the first and second fundamental forms at
the origin are

    I  = | 1 + a10²   a10 a01 |        II = (1/A) | 2 a20   a11  |
         | a10 a01   1 + a01² |                   | a11    2 a02 |

Mean and Gauss curvature follow as

    H = (a02 (1 + a10²) + a20 (1 + a01²) − a11 a10 a01) / A³
    K = (4 a02 a20 − a11²) / A⁴

and the principal curvatures are κ± = H ± √(H² − K) (19). The principal
directions are the solutions w of (II − κ I) w = 0, lifted to space by
v = w_x (s_i + a10 n_i) + w_y (t_i + a01 n_i) (20). The polynomial's own
unit normal is n* = (n_i − a10 s_i − a01 t_i) / A (21).

### 3.3 The torus

Sort the two curvatures by size: κ_max the larger in absolute value,
κ_min the smaller. Then

    r = 1 / |κ_max|                       tube radius
    R = 1 / |κ_min| − sign(κ₊ κ₋) · r     distance from axis to tube centre
    sign(T) = +1 if κ_max < 0, else −1
    c = p_i + a00 n_i − (sign(T) / |κ_min|) n*                         (22)
    u = n* × v̂_min                        the axis direction

and the distance is the textbook one,

    φ_T(x) = ‖ ( ‖(x − c) × u‖ − R ,  ⟨x − c, u⟩ ) ‖ − r               (23)
    g_i(x) = sign(T) · φ_T(x)                                          (24)

Reading it: the surface point Q*(0,0) sits on the torus's equator. The
tube wraps the direction of strongest bending, the equator runs along
the direction of weakest. If the surface bends *away* from its normal
(κ_max < 0, a bump) the torus is solid and its outside is the outside;
if it bends *towards* the normal (a dent) the sign is flipped, so the
solid torus fills the dent from outside and its interior counts as
outside the shape. sign(κ₊κ₋) picks the outer equator (both curvatures
the same way) or the inner one (a saddle).

### 3.4 The blend that is actually evaluated

A single global λ does not work when sampling density varies, so the
weights are made relative to the query (25, 26):

    w_i(x) = exp( −λ_x (‖x − p_i‖ − σ_x) ),
    σ_x = ½ max_i ‖x − p_i‖,   λ_x = C / σ_x,   C = 64

The shift σ_x only keeps the exponentials in range; it cancels in the
ratio. The paper sums over points inside a radius; the released code's
default, which the demo follows, sums over the **32 nearest** points and
takes the maximum over those. The exponents then run from +64 at the
query to −64 at the 32nd neighbour.

The gradient (appendix D), with λ treated as a constant, is

    ∇φ = Σ w_i (∇g_i − λ r̂_i g_i) / Σ w_i  +  φ · λ Σ w_i r̂_i / Σ w_i

with r̂_i the unit vector from p_i to x.

## 4. Where the six numbers come from

Fitting the polynomial by weighted least squares to the neighbours is
the classical answer, and the paper shows it failing: near edges, thin
parts and uneven sampling the fitted curvatures are wild. So the six
coefficients are **predicted by a small transformer** instead.

* Input: p_i and its 64 nearest points (itself included). Offsets are
  expressed in the frame (s_i, t_i, n_i) and divided by σ_i, the median
  distance; normals are expressed in the same frame. Six numbers a
  point, so the input is 64 tokens of 6: independent of position,
  rotation and scale.
* Network: linear 6 → 128 and a layer norm; 8 pre-norm transformer
  blocks (8 heads, feed-forward width 512, GELU); a final layer norm;
  the token of p_i itself goes through a linear 128 → 6. About 1.6
  million weights. There is no positional encoding: a neighbourhood is a
  set.
* Output: (a00, a01, a10, a11, a02, a20) in the scaled frame. Undoing the
  scale: a00 × σ_i, the slopes unchanged, the three second-order
  coefficients ÷ σ_i.
* Loss (27): not on the coefficients. The network's tori are blended
  into φ and φ is compared with the true distance: L1 error plus an
  eikonal term |1 − ‖∇φ‖|, at 120 query points per neighbourhood (a
  third on the surface, a third within 0.2 of it, a third anywhere in
  the cube). The torus construction and the blend are differentiable,
  so the gradient flows back to the coefficients.
* Data: 10 000 CAD models from ABC and 2 000 bumpy balls
  x² + y² + z² = (1 + α·Perlin)², 2 048 points each. Adam, a curriculum
  of 24/8/8/5/2 epochs, 45 hours on an RTX 3090.

Because the loss is on the *distance*, the network is free to output
whatever coefficients make the blend good, including ones that are not
the surface's true curvatures. Section 7 shows that it uses that
freedom.

## 5. What the paper reports

Mean distance error on a 64³ grid with 512 input points: 0.020 (ABC),
0.012 (blobs), 0.033 (Thingi10k), 0.016 (analytic shapes); the plane
baseline gets 0.158 / 0.023 / 0.085 / 0.071, and screened Poisson
reconstruction 0.035 / 0.009 / 0.032 / 0.012. With more points the error
falls: 0.0096 at 1 024 points, 0.0063 at 65 536.

Applications shown: offset surfaces, Booleans, morphological opening and
closing, sphere tracing straight from points, distance to Gaussian
splats, supervising neural implicits, re-orienting normals.

Limits the authors state: φ is not conservative (it can exceed the true
distance, so a sphere-tracing step may overshoot); it does not pass
exactly through the samples; it is always smooth, so sharp edges are
rounded; it was not trained on noise; it is sensitive to sampling
density; and cutting the sum off leaves artifacts far from the surface.

## 6. What `demos/pat` implements

Everything needed to go from a point cloud with normals to pictures of
the distance field, with the authors' trained weights.

| Paper | Here |
|---|---|
| network (4.3) | `pat_net_forward` in `demos/pat/method.c`: plain C, no library. Weights in `demos/pat/data/weights.bin`, written from the authors' pickle by `scripts/pat_export_weights.py` |
| frame, features, scaling | `coefficients` in `demos/pat/main.c` |
| polynomial → torus (18–22) | `pat_fit_torus` in `method.c` |
| torus distance and gradient (23, 24) | `pat_torus_distance` in `method.c`; `patTorus` in `shaders/pat_common.slang` |
| blend and gradient (25, 26, app. D) | `pat_distance` in `method.c`; `patCloud` in `pat_common.slang` |
| tangent-plane baseline (29) | the same two functions with `--blend 1` |
| offsets | `--offset` |
| Booleans | `--combine 1/2/3 --other <cloud>` |
| sphere tracing from points | `shaders/draw.slang` |

`pat --fit` runs the network on five clouds and writes one `.pat` file
each (an 80-byte torus per point); the demo itself only reads those. In
C on one core the network takes 20 ms a point, 40 s for 2 048 points.

Three things are done differently from the released code, none of which
changes the numbers beyond rounding:

**The torus is stored by curvature, not by centre and radius.** Where
the surface is nearly flat or a cylinder, R is in the millions, and
‖(x − c) × u‖ − R in 32-bit floats has no digits left (`--check`
measures an error of 0.5 for radii of 5 × 10⁶). The shader needs
floats. Writing y for x minus the anchor Q*(0,0), a = ⟨y, u⟩,
y′ = y − a u, b = ⟨spoke, y′⟩, k = |κ_min|, m = |κ_max|:

    δ = (2b + k |y′|²) / (√(1 + 2kb + k² |y′|²) + 1)
    g = sign · (m (δ² + a²) + 2·side·δ) / (√((mδ + side)² + m² a²) + 1)

δ is the signed distance to the cylinder through the equator and g the
distance to the tube, each written as (difference of squares) ÷ (sum),
which is equation 23 exactly but stays accurate as k, m → 0, where it
becomes the plane distance.

**Neighbours are found in a grid.** The points are sorted into 16³ cells
on the CPU; a query looks at growing shells of cells until the 32nd
nearest point is closer than the next shell.

**Rays use a baked volume to get near.** 32 neighbours × a torus each
is too much for every step of every ray on a software rasteriser. A
compute pass bakes φ into a 64³ texture once per cloud; a ray steps by
that while it says the surface is more than a voxel away and asks the
points themselves from there on, so where the ray stops, and the
normal there, come from the method and not from the texture. `--direct`
skips the volume altogether; `--check` finds no pixel where the two
disagree about hitting.

## 7. What reproducing it showed

All from `pat --check` (table in `demos/pat/README.md`) unless noted.

* **The C network is the authors' network.** Coefficients differ from
  their JAX code by 2 × 10⁻⁶. Over all five clouds the tori agree to about
  10⁻⁴ of their size, except one point of the snail where the two
  curvatures are equal and opposite and rounding decides which is called
  the larger.
* **Second order is real.** On a polynomial that bends one way, the
  torus leaves the polynomial like h³ (measured order 3.02 and 3.05).
* **It beats planes by 5 to 9 times** on shapes with known distance, with
  2 048 points on a 32³ grid: torus 0.00055 against 0.0038, rounded box
  0.00086 against 0.0064, bumpy ball 0.00034 against 0.0032. The paper's
  figure for 1 024 points on harder shapes is 0.0096.
* **The network does not predict saddles.** On the torus cloud a quarter
  of the points lie on the inner, saddle-shaped half; the predicted
  Gauss curvature is positive at every one of them (and negative at 1
  point in 12 288 over the five clouds). Those points get a spindle
  torus instead. This matters because equation 22, taken as printed and
  as coded by the authors, does not give a touching torus for a true
  saddle: `--check` builds one from a saddle polynomial and finds
  contact of order 2, not 3, and a gradient at the point that is *minus*
  the normal. The demo keeps the construction as it is, since the
  weights were trained through it; the measurement is reported, not
  fixed.
* **The blend is nearly a switch.** With C = 64 a weight falls by
  e^(-1.28) = 0.28 for every 1 % of the farthest neighbour's distance. At
  a query whose 32nd neighbour is 0.15 away, a point 0.01 farther than
  the nearest weighs 0.0002 of it. Using the nearest torus alone gives the same mean error
  (0.00053 against 0.00055 on the torus). What blending buys is
  continuity: along a line the nearest-torus field jumps (389 units of
  change per unit of length at a step of 10⁻⁵), the blended one does not
  (1.3). The 1.3 is the non-conservativeness the authors mention: a true
  distance never changes faster than 1.
* **Rays are safe with a 0.9 step.** The few that overshoot come back by
  Newton's rule; 25 of 36 274 hits end more than 10⁻⁴ off the level, the
  worst by 0.002.
* **Normals matter.** The scans show facets where neighbouring tori
  disagree (visible on the tooth), and the snail's slice has a small
  pocket of the wrong sign inside the body.

## 8. Not done

* **Training.** 45 GPU-hours and 12 000 models; the released weights are
  used. Nothing about the loss, the data or the curriculum is checked.
* Morphological opening and closing, normal re-orientation, Gaussian
  splats, neural-implicit supervision. Each is the same φ fed to
  another algorithm.
* The paper's fixed-radius evaluation (λ = 10³/D): for 2 048 points on a
  unit shape that radius holds less than one point, so the released
  code's 32-nearest rule is the one implemented.
* The comparison methods other than tangent planes (smooth hull
  consensus, screened Poisson reconstruction).
* Checked on lavapipe only.
