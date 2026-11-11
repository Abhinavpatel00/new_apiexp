# particles — compute simulation, indirect draw

262 144 particles by default (`--count 1000000` fits the 64 MB storage
buffer) in a curl-noise flow. The CPU never reads or writes a particle.

```
Simulate (compute, 64 threads/group)    birth, advection, compaction, vertex count
  -> Draw (indirect, vertex pulling)    6 vertices per live particle, additive, RGBA16F
  -> Present -> UI
```

| Buffer range | Written by | Read by |
|---|---|---|
| particles, 32 B each: position, cycle, velocity, age | Simulate | Simulate, Draw (vertex) |
| compact, 4 B each: index of the n-th live particle | Simulate | Draw (vertex) |
| args, 16 B: `VkDrawIndirectCommand` | CPU: reset to `{0, 1, 0, 0}`; Simulate: `+6` per live particle (atomic) | Draw (`pass_draw_indirect`) |

First demo with a compute pass feeding a graphics pass through the storage
buffer (`buf_writes` → `buf_reads` / `use_indirect`), and with a draw whose
size the CPU does not know. The flow is `mu_curl_noise3`
(`external/mu_lib/mu/shaders/noise.slang`).

```sh
./build/cm/particles
./build/cm/particles --headless out.ppm              # 300 frames, then capture
./build/cm/particles --headless out.ppm --count 1000000
./build/cm/particles --check                         # about a minute on lavapipe
```

## Life cycle

Each particle repeats: alive for 2.5–6 s, absent for 0.2–2 s, with its own
phase. All of it is integer milliseconds hashed from the particle index, so
`main.c` runs the same arithmetic (timing only, no positions) and knows how
many particles must be alive — without reading anything back.

## Acceptance check

`--check` draws in a state view: particle `id` owns pixel
`(id % width, id / width)` and writes `(position, 1)` there through the same
compacted, indirect draw. Two runs of 300 frames at a fixed 16 ms step.

| Check | Limit | Measured (frame 150 / 300) |
|---|---|---|
| particles alive, CPU prediction | 10–95 % of all | 123 260 / 200 131 |
| indirect arguments read back (`readback_request_buffer`): `vertexCount` − 6 × predicted | exactly 0 | 0 / 0 |
| compacted index list read back: repeated or out-of-range entries | 0 | 0 / 0 |
| drawn by the indirect draw − predicted | exactly 0 | 0 / 0 |
| pixels drawn more than once (stale compaction entries) | 0 | 0 / 0 |
| positions not finite or beyond ±40 | 0 | 0 / 0 |
| mean height, emitter at 0.3 (the simulation moves them) | > 0.5 | 0.99 / 1.41 |
| second run: values that differ from the first | 0 | 0 / 0 |

The same check passes with `--count 1000000 --size 1280 800`.

## Different from the roadmap

- No bloom pass after the draw; `Present` tone-maps the HDR target directly.
- `wave.slang` / `dispatch.slang` are not used: the dispatch is a plain
  `ceil(count / 64)`.
