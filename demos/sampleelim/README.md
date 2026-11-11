# sampleelim — a Poisson disk sample set by eliminating samples

```
(method.c on the CPU: weights, a heap, eliminate the heaviest) ─► Draw ─► Present
```

Cem Yuksel: *Sample Elimination for Generating Poisson Disk Sample Sets*
(Eurographics 2015), in the plane.

Left: the samples; the ones kept are yellow, the ones still to go small
and blue. Right: the power spectrum of the samples shown, for whole
frequencies up to 32 (flat grey is white noise; a dark disc with a bright
ring is blue noise).

* `--scene 0`: from M random samples down to N.
* `--scene 1`: the same with a density map, dense in the middle (the
  weights of the paper's figure 12).
* `--scene 2`: the N samples ordered as in section 5.1; the picture steps
  through the first N, N/2, … N/32 of them.

| option | |
|---|---|
| `--count N` | samples kept (400) |
| `--ratio R` | input samples for each one kept, M / N (5) |
| `--no-limit` | beta 0: without the weight limiting of eq. 4 |
| `--open` | distances do not wrap round the square |
| `--seed S` | of the random input |

## The method

Every sample gets a weight: the sum over its neighbours within `2 r_max`
of `(1 − d / (2 r_max))^8` (eq. 1), where `r_max` is the spacing that a
hexagonal packing of N samples would have (eq. 2). The sample with the
largest weight is removed, its share is taken off its neighbours' weights,
and this repeats until N are left (the paper's figure 2). Distances below
`2 r_min` count as `2 r_min` (eq. 4, 5). `docs/sampleelim.md` explains why.

## What `--check` measures

52 assertions, about 4 s. Means of 8 random sets where it says so.

* Replay of 150 eliminations with weights computed from scratch before
  each: the sample removed always had the largest weight (difference 0),
  with wrapped distances and with edges.
* 1000 samples, radius / r_max (and mean radius / r_max), 8 sets:

  | M / N | 1 | 2 | 3 | 4 | 5 |
  |---|---|---|---|---|---|
  | beta 0 | 0.021 (0.467) | 0.531 (0.698) | 0.607 (0.737) | 0.626 (0.747) | 0.635 (0.751) |
  | beta 0.65 | 0.017 (0.463) | 0.544 (0.705) | 0.632 (0.755) | 0.672 (0.776) | 0.694 (0.790) |

* Dart throwing given the radius that elimination reached (M / N = 4):
  all 8 sets reached 1000 samples; its mean radius is 0.976 of
  elimination's.
* Spectrum, 1000 from 5000: mean power for 2 ≤ |k| ≤ 10 is 0.96 for the
  input and 0.089 for the samples kept; the ring average peaks at 1.87 at
  |k| = 35.
* Ordered set of 1024: the first 512, 256, 128, 64 have radius / r_max
  0.63, 0.66, 0.64, 0.63; without the ordering 0.49, 0.35, 0.25, 0.19.
* Density map 0 | 1: 903 samples in the half with 0 and 97 in the half
  with 1; the nearest-sample distance is 2.85 times larger in the latter.

## What this says

The paper's statements that could be tested hold here: weight limiting
matters from M / N = 3 on and without it the gain beyond 4 is small;
dart throwing with the same radius has a mean radius about 2 % smaller
(2.4 % here); every prefix of the ordered set is a usable set.

## Not done

Three and more dimensions (eq. 3), surfaces and other distance measures,
the paper's kd-tree (a grid here), the other weight function of its
figure 13, its timings. The paper was read in parts (sections 3, 4, 5.1,
5.2), not in full.
