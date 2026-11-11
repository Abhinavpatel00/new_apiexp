#!/usr/bin/env python3
"""Small CPU sanity checks for the equations. They mirror the shader math in
Python; they do not run the shaders (the host does that)."""
import math

def sat(x): return min(1.,max(0.,x))
def mix(a,b,t): return a*(1-t)+b*t
def remap(a,b,x): return float(x>=a) if abs(b-a)<1e-6 else sat((x-a)/(b-a))
def band(c,s,x):
    if s<=1e-6: return float(x>=c)
    t=sat((x-(c-s))/(2*s)); return t*t*(3-2*t)

def close(a,b): assert math.isclose(a,b,rel_tol=1e-7,abs_tol=1e-7),(a,b)
close(band(-0.5,0.05,-0.5),0.5)
close(band(0,0,0),1)
close(band(0,0.1,-0.2),0)
close(remap(0,1,mix(1,0.2,0.5)),0.6)
close(remap(1,0,0.2),0.8)
close(remap(0.5,0.5,0.4),0)
close(mix(0.5,1,0.6),0.8)
close(mix(1,0,0.65),0.35)
# Four blocked of nine samples.
close(5/9,0.5555555555555556)

# ---- ABI v2 (bindless/toon.slang) -------------------------------------------
checks = 9

def oct_encode(n):
    l1 = abs(n[0]) + abs(n[1]) + abs(n[2])
    x, y = n[0]/l1, n[1]/l1
    if n[2] < 0:
        x, y = (1-abs(y))*(1 if x >= 0 else -1), (1-abs(x))*(1 if y >= 0 else -1)
    return x, y
def oct_decode(e):
    x, y = e; z = 1-abs(x)-abs(y); t = sat(-z)
    x += -t if x >= 0 else t; y += -t if y >= 0 else t
    l = math.sqrt(x*x+y*y+z*z); return x/l, y/l, z/l

# Octahedral normal: decode(encode(n)) == n over the sphere, both hemispheres.
worst = 0.0
for i in range(0, 181, 9):
    for j in range(0, 360, 9):
        th, ph = math.radians(i), math.radians(j)
        n = (math.sin(th)*math.cos(ph), math.cos(th), math.sin(th)*math.sin(ph))
        d = oct_decode(oct_encode(n))
        worst = max(worst, max(abs(a-b) for a, b in zip(n, d)))
assert worst < 1e-12, worst; checks += 1
# A cube corner's averaged normal is (1,1,1)/sqrt(3) and survives the round trip.
c = 1/math.sqrt(3)
for a, b in zip(oct_decode(oct_encode((c, c, c))), (c, c, c)): close(a, b)
checks += 1

def extrude_pixel(width_px, view_z, fov_deg, height, max_dist=0.0):
    d = abs(view_z)
    if max_dist > 0: d = min(d, max_dist)
    p11 = 1/math.tan(math.radians(fov_deg)/2)
    return width_px*2*d/(p11*height)
def extrude_tutorial(width, view_z, fov_deg):
    return width*sat(abs(view_z))*fov_deg*0.00005
def to_pixels(world, view_z, fov_deg, height):
    return world/(2*abs(view_z)*math.tan(math.radians(fov_deg)/2)/height)

# Pixel mode: the same on-screen width at any distance, fov and resolution.
for z in (0.3, 1.0, 6.6, 40.0):
    for fov in (25.0, 50.0, 90.0):
        for h in (360.0, 720.0, 2160.0):
            close(to_pixels(extrude_pixel(2.0, -z, fov, h), -z, fov, h), 2.0)
checks += 1
# ...and with a max distance it is constant up to there, then shrinks as 1/z.
close(to_pixels(extrude_pixel(2.0, -5.0, 50.0, 720.0, 10.0), -5.0, 50.0, 720.0), 2.0)
close(to_pixels(extrude_pixel(2.0, -20.0, 50.0, 720.0, 10.0), -20.0, 50.0, 720.0), 1.0)
checks += 1
# Tutorial mode, for contrast (upstream quirks, kept on purpose in that mode):
# constant on screen only inside one unit, then 1/z; linear in the fov angle.
close(to_pixels(extrude_tutorial(1, -0.5, 60), -0.5, 60, 1080),
      to_pixels(extrude_tutorial(1, -1.0, 60), -1.0, 60, 1080))
close(to_pixels(extrude_tutorial(1, -2.0, 60), -2.0, 60, 1080)*2,
      to_pixels(extrude_tutorial(1, -1.0, 60), -1.0, 60, 1080))
ratio = (to_pixels(extrude_tutorial(1, -1, 120), -1, 120, 1080) /
         to_pixels(extrude_tutorial(1, -1, 60), -1, 60, 1080))
assert abs(ratio - (120/60)*math.tan(math.radians(30))/math.tan(math.radians(60))) < 1e-9
assert 0.66 < ratio < 0.67   # a 120 degree camera draws the line 1/3 thinner
checks += 1

def rgb_to_hsv(c):
    r, g, b = c
    K = (0.0, -1/3, 2/3, -1.0)
    p = (b, g, K[3], K[2]) if g < b else (g, b, K[0], K[1])
    q = (p[0], p[1], p[3], r) if r < p[0] else (r, p[1], p[2], p[0])
    d = q[0] - min(q[3], q[1])
    return (abs(q[2] + (q[3]-q[1])/(6*d+1e-10)), d/(q[0]+1e-10), q[0])
def hsv_to_rgb(c):
    fr = lambda x: x - math.floor(x)
    p = [abs(fr(c[0]+k)*6-3) for k in (1.0, 2/3, 1/3)]
    return tuple(c[2]*mix(1.0, sat(x-1), c[1]) for x in p)
# HSV round trip (the shadow tint must be an identity for hue 0 / sat 1 / value 1).
for rgb in ((0.86, 0.33, 0.22), (0.93, 0.78, 0.66), (0.40, 0.78, 0.66), (0.5, 0.5, 0.5)):
    for a, b in zip(hsv_to_rgb(rgb_to_hsv(rgb)), rgb):
        assert abs(a-b) < 1e-6, (rgb, a, b)
checks += 1
# A hue shift of one third of a turn rotates the channels: R -> B -> G -> R.
h, s_, v = rgb_to_hsv((1.0, 0.0, 0.0))
for a, b in zip(hsv_to_rgb((h + 1/3, s_, v)), (0.0, 1.0, 0.0)): assert abs(a-b) < 1e-6
checks += 1

def styled_shadow(softness, shadow):
    if softness <= 0: return shadow
    t = sat((shadow-(0.5-softness))/(2*softness)); return t*t*(3-2*t)
# Cast-shadow band: off is the raw PCF ramp; on, 4/9 and 5/9 land either side.
close(styled_shadow(0.0, 4/9), 4/9)
assert styled_shadow(0.18, 2/9) == 0.0 and styled_shadow(0.18, 7/9) == 1.0
assert styled_shadow(0.18, 4/9) < 0.5 < styled_shadow(0.18, 5/9)
checks += 1

def caster_offset(depth_bias, normal_bias, n_dot_l):
    # world-space distance the caster moves: along -L, then along -N
    return depth_bias, normal_bias*(1-sat(n_dot_l))
# Shadow caster bias: no normal bias facing the light, full at grazing angles.
close(caster_offset(0.01, 0.02, 1.0)[1], 0.0)
close(caster_offset(0.01, 0.02, 0.0)[1], 0.02)
close(caster_offset(0.01, 0.02, -0.5)[1], 0.02)
checks += 1

print(f'{checks} CPU equation checks passed. GPU checks: see README.md '
      '("Verified on lavapipe").')
