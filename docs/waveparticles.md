# Wave Particles — the paper and `demos/waveparticles`

Cem Yuksel, Donald H. House, John Keyser: *Wave Particles*, ACM
Transactions on Graphics 26(3), SIGGRAPH 2007.
<http://www.cemyuksel.com/research/waveparticles/>

Sources: the author's preprint (`waveparticles.pdf` on that page) was
read from the abstract to the middle of section 5 (implementation). The
end of section 5, the results, discussion and conclusion (sections 6 to
8) and Yuksel's dissertation of 2010 were **not** read. No source code
is published on the project page; `demos/waveparticles` is written from
the paper's equations. Equation and section numbers are the paper's.

## 1. The idea

A height field z(x, y) of water whose waves all travel at one speed v
obeys the wave equation (eq. 1). Write the height as rest level plus a
sum of small bumps (eq. 2, 3), each of fixed shape, each moving at v:

```
z(x, t) = z0 + Σ_i a_i W_i(x − x_i(t))                      (eq. 3, 4)
```

x_i(t) is a point moving in a straight line: a *wave particle*. A shape
that moves unchanged at speed v solves the wave equation along its
direction of travel, and sums of solutions are solutions. So the whole
simulation is: move points in straight lines, add up bumps. There is no
time-step limit because nothing is integrated.

## 2. The bump (3.1, 3.2)

Across a wave the paper wants a cosine hump (eq. 5):

```
W(u) = ½ (cos(2π u / l) + 1)        for |u| < l/2, else 0
```

It is zero with zero slope at its ends, and humps and hollows placed
alternately half a wavelength apart add up to a clean cosine train.
Along the wavefront, particles sit side by side and their
bumps must blend into an even crest (eq. 6).

The form the paper actually uses is round (eq. 7), because a round bump
is cheap to draw:

```
D_i(x) = a_i/2 (cos(π |x − x_i| / r) + 1)     for |x − x_i| < r, else 0
```

It says that with neighbours closer than r/2 the front's shape is
within 3 % of ideal and the crest even to 0.1 %.

The demo's check: with neighbours exactly r/2 apart the crest is even
to rounding (the cosines cancel exactly at that spacing) and its height
is `a r / spacing`. Across the front the shape differs from eq. 5 by
**7 %** of the peak, not 3 %, and by 6.3 % however close the particles
are: the side view of a row of round cosine bumps is not a cosine. How
the paper measured its 3 % is not said in the part read.

## 3. Fronts that widen: subdivision (3.2)

A ripple's front grows. Particles on it move apart, and gaps would
open. So each particle also carries a **dispersion angle**: the angle
of the front it stands for. Its distance to its neighbours is that
angle times the distance travelled, with no need to look at any other
particle. When that passes r/2, the particle becomes three: each with a
third of the amplitude and a third of the angle, the same radius, the
outer two turned by a third of the angle.

Consequences the check measures on a ripple 3 m out:

* the sum of amplitudes never changes, so the volume of displaced water
  is conserved (to 2·10⁻⁸ on the grid);
* crest height is `a r / 2π d`: it falls as 1/distance (measured:
  0.989 to 1.002 of that round the ring);
* there are 243 = 3⁵ particles, the first power of three whose spacing
  is below r/2.

A ripple from a point is a single particle with an angle of 2π
(section 4.1 uses this for waves made by objects). It splits at once
and keeps splitting as it grows.

Particles do not interact, as real small waves do not.

## 4. Walls (3.3)

A particle bounces off a wall like a ball. At a straight wall that is
the same as folding the plane along it, so the demo computes a
particle's place as "origin + v × age, folded into the pool": no event
is needed. (At a curved wall the paper also changes the dispersion
angle; not implemented.)

One thing the demo adds: a bump within r of a wall has part of itself
outside. The demo adds the mirror image of that bump, which puts the
missing part back inside. That is the reflected wave beginning before
the particle itself has turned, and it keeps the water in the pool
constant during a reflection (measured: to 2·10⁻⁵). The check also
compares a ripple near a wall with open water holding a second ripple
behind the wall: equal to 10⁻¹⁵.

## 5. Water moves in circles: the extended height field (3.4)

In a real water wave a bit of surface does not just go up and down. It
goes round a circle: forwards on the crest, backwards in the
trough. That makes crests sharper and troughs flatter. The paper adds a
horizontal deviation to each bump (eq. 9 to 11):

```
D_i^L(x) = −sin(π u / r) D_i(x) û_i         u = û_i · (x − x_i)
```

û_i is the particle's direction of travel. For a cosine train of
height a this sums to (−a sin, a cos): a circle of radius a. The
check builds such a train from particles and finds the circle to 12 %
(the round bumps again) and the surface moving forwards at a crest at
0.996 of a π v / r.

The demo draws the surface with each grid point moved sideways by this
amount; `--no-warp` turns it off.

The paper also keeps the surface's gradient and velocity for use with
floating objects; the demo does not.

## 6. A time step (5)

```
IterateWaveParticles     subdivisions and reflections due in this step
ComputeObjectForces      (section 4; not implemented)
IterateObjects           (not implemented)
GenerateWaveParticles    (not implemented; the demo emits at points)
RenderHeightFields       particles → height field
```

**The time table.** A particle's position follows from where and when
it began. Only subdivision changes it, and the moment of that is known
at birth: `birth + r / (2 angle v)`. `method.c` keeps these moments in a
heap; `wp_advance` pops those that are due and touches nothing else.
In the check, 120 steps of a ripple visit 121 particles in all, one for
each subdivision, and 7 long steps give the same surface as 240 short
ones to 6·10⁻¹⁴.

**Removal.** In the paper a particle that is due to subdivide and whose
amplitude is below a threshold set by the user is removed instead. The
demo also lets amplitudes fall with age, by exp(−damping × age);
whether the paper damps in the same way could not be re-checked. A
particle of a straight front never subdivides, so the demo gives it an
event at the time damping takes it below the threshold. That rule is
the demo's, and the time is worked out when the particle is emitted: a
later change of the damping slider does not move it.

**The height field.** All particles have the same radius, so the paper
draws each as one point into a texture and then filters the texture
with eq. 7 as the kernel, or cheaper, with eq. 5 horizontally and then
vertically. `wp_field_filtered` does both in C. Against the exact sum,
with a radius of 9.6 samples, the first is off by up to 10.5 % of the
peak (a point snaps to the nearest sample) and the second by 15.7 %
(a product of two cosines is square, not round).

## 7. What was not implemented

* All of section 4: buoyancy, drag and lift on floating objects
  (eq. 13 to 16), and waves generated by moving objects from the volume
  their faces displace (eq. 12). This is half of the paper, and the
  half that its videos show.
* The GPU implementation, curved boundaries, surface velocity and
  gradient fields, rendering of water (reflection, refraction).
