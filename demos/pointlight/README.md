# pointlight — point lights that stay finite next to a surface

```
Light (fullscreen: one ray per pixel, every light for every pixel) ─► Present
```

Cem Yuksel: *Point Light Attenuation Without Singularity* (SIGGRAPH 2020
Talks).

A point light of intensity I lights a surface at distance d by I / d²,
which has no upper limit as d goes to 0. The paper gives the light a
radius r that is used for nothing but the attenuation (its eq. 7):

    fa(d) = 2 / r² · (1 − d / √(d² + r²))

2 / r² at d = 0, and 1 / d² far away.

A room open at the front with two balls.

* `--scene 0` (default): a spot light under the ceiling shines down. Its
  light is carried on by virtual point lights: 64 rays from the spot and a
  small light where each one ends. These lights sit on the surfaces, so
  points next to them are at d near 0. The picture shows only the light
  they carry on (`--direct` adds the spot's own). Left of the line 1 / d²:
  bright spots where a virtual light is near a corner. Right of the line
  eq. 7.
* `--scene 1`: three coloured lights fly 0.04 from the floor, the back
  wall and a ball.

| option | |
|---|---|
| `--view V` | 0 left 1 / d², right eq. 7; 1 all 1 / d²; 2 all eq. 7; 3 all 1 / (d² + r²/2) (eq. 8) |
| `--radius R` | the lights' radius r (default 0.25; the room is 4 wide) |
| `--lights N` | virtual point lights (1 … 256) |
| `--direct` | scene 0 with the spot's own light too |
| `--still` | nothing moves |

## What `--check` measures

66 assertions, under a second.

**The shader's functions** (read back from a picture whose x axis is d)
agree with the formulas in double precision to 1.4e-7.

**The functions** (r = 1): eq. 7 ≤ eq. 8 ≤ 1 / d² everywhere, eq. 7 never
rises. Ratio to 1 / d²:

| d / r | 0.5 | 1 | 2 | 5 | 10 | 100 |
|---|---|---|---|---|---|---|
| eq. 7 | 0.276 | 0.586 | 0.845 | 0.971 | 0.9926 | 0.99993 |
| eq. 8 | 0.333 | 0.667 | 0.889 | 0.980 | 0.9950 | 0.99995 |

**What eq. 7 stands for.** The solid angle of a disc of radius r seen from
its axis, summed ring by ring, over π r²: equal to eq. 7 to 3e-12.

**A real glowing ball** of radius r and the same power, on a surface that
faces it, summed over the directions that meet the ball:

| d / r | 1 | 1.5 | 2 | 4 | 10 |
|---|---|---|---|---|---|
| the ball, × d² / I | 1 | 1 | 1 | 1 | 1 |
| eq. 6 with the ball's solid angle | 2 | 1.146 | 1.072 | 1.016 | 1.0025 |
| eq. 7 | 0.586 | 0.756 | 0.845 | 0.955 | 0.9926 |

The ball follows 1 / d² exactly down to its surface. So eq. 7 is not
what a ball-shaped light does to a facing surface; it is a bounded
function with the right limits. The paper's conclusion says the same:
"it does not closely approximate spherical lights, particularly for short
distances".

**Single precision.** Eq. 7 as printed subtracts two nearly equal
numbers when d is much larger than r:

| d / r | 10 … 20 | 100 … 200 | 1000 … 2000 | 10000 … 20000 |
|---|---|---|---|---|
| largest relative error as printed | 6.6e-5 | 0.59 % | 47 % | 100 % (it gives 0) |
| as 2 / (d² + r² + d √(d² + r²)) | 1.5e-7 | 1.3e-7 | 1.0e-7 | 9.1e-8 |

The second line is the same function; the shader uses it. A light with
r = 1 cm is up to 47 % wrong 10 to 20 m away in the printed form.

**The picture, virtual lights only, r = 0.25, 320 × 180.**

| | 1 / d² | eq. 7 | eq. 8 |
|---|---|---|---|
| brightest pixel, 32 lights | 1.53 | 0.207 | 0.222 |
| 32 against 256 lights, r.m.s. difference / r.m.s. | 0.55 | 0.20 | 0.22 |
| mean of the picture / that with 1 / d², 256 lights | 1 | 0.951 | 0.960 |

Eq. 7 removes the spots (the brightest pixel is 7 times lower) and with
them most of what changes when the number of lights changes. It pays
with 4.9 % of the light.

**Lights 0.04 from a surface.** The brightest point under the green
light is what eq. 7 predicts (0.989, 0.993, 1.004 of it for r = 0.05,
0.15, 0.5) and stays under 0.8 / π · I · 2 / r². With 1 / d² it is 2.1,
9.4 and 83 times brighter.

## Not done

The paper's figure 1 (virtual lights in a large scene, chosen with
stochastic lightcuts); here every pixel adds up all lights. One bounce
only. Diffuse surfaces only. The paper's eq. 8 is read as
1 / (d² + r²/2); see `docs/pointlight.md`.
