#!/usr/bin/env python3
"""Small CPU sanity checks for the equations, not GPU or visual validation."""
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
print('9 CPU equation checks passed; no GPU or visual test performed.')
