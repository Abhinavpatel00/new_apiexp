# Point Light Attenuation Without Singularity — the paper and `demos/pointlight`

Source. Cem Yuksel, "Point Light Attenuation Without Singularity",
SIGGRAPH '20 Talks, 2 pages, DOI 10.1145/3388767.3407364
(`http://www.cemyuksel.com/research/pointlightattenuation/pointlightattenuation.pdf`).
Read in full. Equation and figure numbers are the paper's.

## 1. The problem

The light that leaves a surface point towards the viewer is (eq. 1) the
integral over all incoming directions of incoming light × BRDF × cos θ.
For one point light of intensity I this becomes (eq. 2)

    Lo = I · V · fa(d) · fr · cos θ,     fa(d) = 1 / d²

with V the shadow test and d the distance. 1 / d² is right for a point,
but it grows without limit as d → 0. In practice it is patched with
functions chosen by hand for positions that are "too close to the
light". The paper's point: the fault is not the function but calling
the light a point, since a point light usually stands for a small light
of some size.

## 2. The derivation (section 2)

Take a ball of radius r that glows evenly and sends out the same power.
Its radiance is I / (π r²) in every direction from which it is visible
(eq. 3). Then three simplifications:

1. BRDF, cos θ and the shadow test use one direction, towards the
   centre, and come out of the integral (eq. 4).
2. What is left is the solid angle under which the light is seen,
   2π (1 − cos α), assumed to lie wholly above the surface (eq. 5). This
   gives fa = 2π (1 − cos α) / (π r²) (eq. 6).
3. For a ball, cos α is not defined for d < r. So the ball is replaced
   by a disc of radius r that faces the surface point, for which
   cos α = d / √(d² + r²). The result is eq. 7:

       fa(d) = 2 / r² · (1 − d / √(d² + r²))

The paper states plainly that this "does not simply convert point lights
to spherical lights": the radius is "only used for computing their
attenuation". At d = 0 the value is 2 / r²; for r → 0 or d ≫ r it goes
to 1 / d².

## 3. The usual patch (section 3)

Adding a constant to the denominator, with the constant now tied to r
(eq. 8) so that it "produces the same attenuation value at d = 0 as
Equation 7". The equation is garbled in the extracted text ("1" over
"d² + r²" with a stray "2"). The demo reads it as

    f̃a(d) = 1 / (d² + r²/2)

because that is the form with numerator 1 that gives 2 / r² at d = 0,
and because it lies between eq. 7 and 1 / d², as the paper says eq. 8
does. The paper adds that the physical meaning of this patch "is
unclear".

## 4. The demo

`attenuation()` in `shaders/pointlight.slang` has the three functions.
Eq. 7 is evaluated as

    fa(d) = 2 / (d² + r² + d √(d² + r²))

which is the same function (multiply and divide by √(d² + r²) + d) and
has no subtraction; section 6 says why.

Scene 0 is the situation of the paper's figure 1: virtual point lights.
A spot light sends `lights` rays, evenly over its cone; where a ray
ends there is a light of intensity (power of the ray) × albedo / π ×
cos(angle to the surface normal). Every pixel adds up all of them, with
a shadow test. Such a light sits on a surface, so its neighbours along a
corner are arbitrarily near. Scene 1 has three lights placed by hand
that fly 0.04 from a surface, the case the paper says the formulation
"is designed for".

## 5. What the check measures

Numbers are in `demos/pointlight/README.md`.

1. The shader's three functions against the formulas (a view that
   writes them into a picture with d along x).
2. The paper's statements: the value at 0, eq. 7 ≤ eq. 8 ≤ 1 / d², the
   limit for d ≫ r.
3. Eq. 7 is the solid angle of a facing disc over π r² (summed in rings).
4. What a glowing ball really does to a surface that faces it: the sum of
   cos θ over the directions that meet it.
5. The printed form of eq. 7 in single precision.
6. The picture: brightest pixel, change with the number of virtual
   lights, light lost.
7. The brightest point under a light 0.04 away.

## 6. Agreement with the paper, and what the paper does not say

* All statements about the functions hold (value at 0, order, limits).
* The spots of figure 1a and their removal in 1b are reproduced in kind:
  with 32 lights the brightest pixel is 1.53 with 1 / d² and 0.207 with
  eq. 7 (r = 0.25 in a room 4 wide), and the picture changes 2.7 times
  less between 32 and 256 lights. Not the paper's scene or renderer.
* A glowing ball lights a facing surface with exactly I / d² for every
  d ≥ r (measured: 1 to nine digits). Eq. 6 with the ball's own solid
  angle is above that (2 at d = r), eq. 7 is below (0.586 at d = r,
  0.845 at 2r). So the reduction near the light comes from the
  simplifications, not from the ball. The paper's conclusion says so:
  "it does not closely approximate spherical lights, particularly for
  short distances". Its introduction says the formulation "does provide a
  close approximation of near illumination from spherical lights"; for a
  facing surface the measurement supports the conclusion's wording, not
  the introduction's. Surfaces that do not face the light, where part of
  the ball is below the horizon, were not measured.
* Not in the paper: written as printed, eq. 7 in single precision is off
  by up to 0.6 % for d from 100 r to 200 r, 47 % from 1000 r to 2000 r,
  and gives 0 from 10000 r to 20000 r.
  The rearranged form stays at 1.5e-7.
* Eq. 7 takes light away (4.9 % of the picture's mean here, eq. 8 4.0 %).
  The paper does not give such a number.

## 7. Not implemented

Stochastic lightcuts and the paper's scene (figure 1), more than one
bounce, glossy surfaces, the comparison with virtual spherical lights
(Hašan et al. 2009) that the paper mentions.
