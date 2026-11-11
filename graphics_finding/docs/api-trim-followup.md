# API trimming follow-up and profiling rationale

The initial trimming pass correctly removed hardcoded hardware budgets and fixed
RX 9070 XT estimates, but it also removed useful, portable profiling support.
That was too broad. The graphics skills and the v2 API rationale make a clear
distinction: **the library should collect timings and organize profiling data;
it should not guess a device's bottleneck from uncalibrated assumptions.**

## What profiling the library owns

- GPU timestamps around every pass are automatic. Callers do not create query
  pools or resolve timestamp results.
- Named intra-pass GPU timestamp scopes are optional and bounded per frame.
  They help split a dominant pass into meaningful regions without requiring an
  external profiler just to see where time went. Scopes are sequential and
  non-nested; their stage field is a label, not a synchronization control.
- `frame_program_report()` reports per-pass times and workload annotations;
  `perf_dump()` additionally includes resolved scopes. The smoke test exercises
  these paths.

These facilities follow the skill's first step: measure frame time and sort
passes by measured GPU time before choosing an optimization. Pass timestamps
are observations; they do not label the bottleneck automatically.

## Workload annotations the app supplies

Shader source and intent are not generally introspectable by the C Vulkan
backend. A pass callback can therefore optionally annotate:

- total samples and effective independent samples (`N` and `N_eff`),
- steps per pixel,
- workgroup dimensions and tile size.

The API only records and reports these values; it does not enforce workgroup
shapes, invent missing counts, or claim that an annotation was measured. This
puts workload context next to the measured pass cost while leaving semantic
truth with the code that knows the algorithm. Zero means “not supplied.”

## What remains out of the profiler

The Vulkan API does not portably expose counters such as L2 hit rate, DRAM
bytes, VALU utilization, or wave stalls. The skills call for RGP/RGA or other
vendor tools to classify the limiting unit and explicitly warn against
optimizing from timing alone. Accordingly, the library does not synthesize a
`Bottleneck` from pass-time ratios.

The old bandwidth “floor” used fixed device bandwidth and idealized resource
traffic, then treated a ratio against that estimate as a bottleneck verdict.
That is not a hardware-independent floor: compression, caches, actual shader
accesses, and memory traffic can differ from declared attachments and ranges.
The report now keeps a **logical declared-byte estimate** per pass: declared
buffer-range sizes plus full declared texture/attachment extents using their
format and sample count. Each use is counted independently; texture sampling
patterns, overdraw, cache reuse, and compression are unknown. It is explicitly
an estimate—not physical GPU traffic or a time floor. The old automatic
ratio-based bottleneck classification stays out. Hardware-specific budgets,
fixed tuning constants, shader-stat budget gates, and dispatch-shape policy
checks likewise remain out.

## Verification

The smoke test checks automatic pass timing, workload annotation propagation,
and a named intra-pass scope. Lavapipe validates the API path only; its timing
is not presented as a hardware performance conclusion.
