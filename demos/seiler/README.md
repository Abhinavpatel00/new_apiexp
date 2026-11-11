# seiler — Seiler's interpolation

```
(method.c on the CPU) ─► Draw ─► Present
```

A cubic Bézier curve from three linear interpolations, after Cem Yuksel,
*Seiler's Interpolation for Evaluating Polynomial Curves* (SIGGRAPH Talks
2024). `--scene 0` draws the construction of the talk's figure 1 for a
curve of `--degree` 2 to 5; `--scene 1` a uniform Catmull-Rom spline
stored as one knot (blue) and one Seiler point (red) a sample. `--at`
fixes the parameter the construction is shown for.

The talk is explained in [`docs/seiler.md`](../../docs/seiler.md).

## The method

```
s1 = 3 b1 − b0 − b3            s2 = 3 b2 − b3 − b0        (once, per curve)

b03 = lerp(b0, b3, t)          blue dot
s12 = lerp(s1, s2, t)          red dot
C   = lerp(b03, s12, (1 − t) t)                           green dot
```

de Casteljau needs six lerps. The first two here use the same weight on
separate pairs, which is what a linear texture filter does when the b and
the s are stored in two textures.

## Acceptance (`--check`, under a second)

| check | measured | rule |
|---|---|---|
| degrees 2 to 5, 500 random curves × 21 parameters: difference terms, Seiler points, offsets, de Casteljau, each − Bernstein, worst | 5.6e-16 | < 1e-13 |
| cubic: Seiler points against the formulas above | 0 | < 1e-14 |
| uniform Catmull-Rom: first Seiler point of a piece − second of the piece before | 2.2e-16 | < 1e-14 |
| spline from one knot and one Seiler point a sample − its Bézier pieces | 1.3e-15 | < 1e-14 |
| a cubic split in two: Seiler points either side of the split differ by | 0.45 | > 0.01 |
| time of one cubic on this CPU: Bernstein, de Casteljau, polynomial, Seiler points, Seiler offsets | about 10, 5, 3, 3.5, 3.7 ns | reported |

## What is not done

* Nothing here runs in a texture filter. The curves are evaluated in C;
  the hardware changes of the HPG 2021 paper (Lin, Seiler, Yuksel) that
  the talk comes from, and its 2-D and 3-D texture filtering, are not
  implemented, and that paper was not read.
* Degrees above 5: the talk gives no difference terms for them.
