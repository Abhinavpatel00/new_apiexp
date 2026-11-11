# Sample Elimination for Poisson Disk Sample Sets — the paper and `demos/sampleelim`

Source. Cem Yuksel, "Sample Elimination for Generating Poisson Disk Sample
Sets", Computer Graphics Forum 34(2), Eurographics 2015
(`http://www.cemyuksel.com/research/sampleelimination/sampleelimination.pdf`).
Read: sections 3 and 4, 5.1, 5.2 and the pseudo code of figure 2; the
related work, the timing section and the figures themselves were not gone
through. Equation and figure numbers below are the paper's.

## 1. The problem

A Poisson disk sample set is a set of random points of which no two are
closer than `2r`. Such sets have "blue noise" spectra: little energy at
low frequencies, which turns sampling error into fine noise instead of
visible blotches. The classical way to get one is dart throwing: propose
random points and reject those too close to an accepted one. It needs the
radius in advance, it slows down as the set fills, and it does not give a
chosen number of samples.

## 2. The idea

Turn it round. Start from more random samples than needed (M) and remove
samples until N are left, choosing each time the sample that is most
crowded. No radius is given; the number of samples is. The paper notes
that finding the subset with the largest radius exactly is NP-complete and
that this is a greedy approximation in M − N steps.

## 3. The weight (section 3.1, eq. 1, 2)

Sample `i` has the weight `w_i = Σ_j w_ij` over the other samples within
`2 r_max`, with

    w_ij = (1 − d^_ij / (2 r_max))^alpha,    d^_ij = min(d_ij, 2 r_max)    (eq. 1)

`alpha = 8` in the paper, so that one very close neighbour outweighs
several moderately close ones (it says other values gave similar results).
`r_max` is the largest radius N samples could have in the domain, the
hexagonal packing; in the plane for area `A` (eq. 2)

    r_max = sqrt(A / (2 √3 N))

The algorithm (figure 2): compute all weights, put the samples in a heap,
then until N are left take the top of the heap out and subtract `w_ij`
from the weight of every sample `i` around the removed `j`, moving `i` in
the heap. The paper finds neighbours with a kd-tree; `method.c` uses a
grid of cells `2 r_max` wide, which does the same job in the plane.

The paper allows any distance measure, also one that is not symmetric.

## 4. Weight limiting (section 3.1, eq. 4, 5)

The paper found that the result is better when samples with many
moderately close neighbours go before samples with one very close
neighbour. It gets this by counting every distance below `2 r_min` as
`2 r_min` (eq. 4), with (eq. 5)

    r_min = r_max (1 − (N / M)^gamma) beta,    beta = 0.65, gamma = 1.5

so there is no limiting when M = N and the limit approaches
`beta r_max` for a large input. It calls this a conservative estimate
found by experiment; the best `r_min` depends on the input.

## 5. How good a set is (section 4)

Two numbers: the radius ratio `rho = r / r_max`, where `r` is half the
smallest distance between two samples, and the mean radius ratio, the
same with the mean over the samples of half the distance to the nearest
one. And the power spectrum. `se_radius` and `se_spectrum` compute them.

## 6. Progressive sample sets (section 5.1)

Go on eliminating: from N to N/2, then to N/4, and so on, each time with
the weights for the new target, and put every eliminated sample at the end
of the current set. Read from the front, the first N/2ᵏ samples are then
each a sample set of their own; a renderer can take as many as it needs.
`se_order` does this. The paper states that the earlier way, reducing a
dart throwing radius step by step, is prone to bias.

## 7. Density (section 5.2)

For its figure 12 the paper uses `r_max = 3 r_max,2` and
`d^_ij = d_ij (3 − 2 I_i)` with `I_i` the intensity of a density map at
sample `i`, and `beta = 0`. Where `I` is 0 distances count three times as
long, so samples there look uncrowded and survive: the set is dense where
the map is dark. The paper points out the limit of this: elimination adds
no samples, so the input must already be dense enough everywhere.

## 8. The demo

Unit square, by default with distances that wrap round (the parts of the
paper that were read do not say which its tests use; `--open` turns it
off). Scene 0 shows the set at eight moments between M and N; the order
in which `se_eliminate` returns the eliminated samples makes every such
moment a prefix of one array.

## 9. What the check measures

The table and the numbers are in `demos/sampleelim/README.md`.

1. The heap and the incremental weights against a replay with weights from
   scratch (independent code for eq. 1, 4, 5 in `main.c`).
2. Both radius ratios for M / N = 1 … 5, with and without weight limiting.
   For M = N the mean radius ratio must be that of random points, 0.4654.
3. Dart throwing with the same radius and count.
4. The spectrum at low frequencies and where its ring is.
5. The radius ratio of prefixes, ordered and not.
6. The density rule on a map with two halves: spacing 3 : 1 would give
   counts 9 : 1; measured 2.85 : 1 and 903 : 97.

## 10. Agreement with the paper

* "Weight limiting … makes a significant difference when M/N > 2"; here
  the gain in `rho` is 0.013 at 2, 0.025 at 3 and 0.058 at 5.
* Without it, more input than 4 N "makes only a minor improvement": +0.010
  from 4 to 5 here, against +0.022 with it.
* Dart throwing "produces about 2% smaller" mean radius at the same
  radius: 2.4 % here (figure 3's sizes, 1000 from 4000).
* The values of `rho` themselves could not be compared: the paper gives
  them as plots (figure 6, for 128 samples, averages of 1000 tests); the
  demo's are for 1000 samples and 8 sets.

## 11. Not implemented

Three and more dimensions (eq. 3 and `r_max,3`), samples on surfaces,
other weight functions (the paper's figure 13), parallel tiles, timings.
