---
name: vulkan-opt
description: >
  Optimize a Vulkan game engine or renderer using driver-level reasoning. Use this
  skill when the task is to find and fix GPU performance problems (frame time, pass
  cost, bandwidth, overdraw, sync stalls, occupancy, small draws, MSAA/TAA/post
  chains), to audit a render pipeline against a reference architecture, or to explain
  why an API-level choice is slow on AMD hardware. Grounded in Mesa/RADV source, not
  folklore.
license: MIT
version: 1.0
---

# Vulkan optimization skill (Mesa-grounded)

Your job is to make a Vulkan renderer faster **without** silently changing what it draws.
Every recommendation you make must be one of:

1. **Remove work** (a pass, a barrier, a resolve, a copy, an invocation), or
2. **Move work** to a cheaper place (bandwidth → compute, full-res → half-res,
   invocations → culling, per-draw state → baked state), or
3. **Let the driver do something it currently refuses to do** (fast clear, DCC/HTILE
   compression, binning, coalesced stores), because an API-level choice disabled it.

If a change fits none of the three, it is a guess. Say so and measure instead.

---

## 0. Triggers

Use this skill when the user asks to:

- "make this renderer/engine faster", "why is this pass slow", "reduce frame time"
- review a render graph / pass list / G-buffer / MSAA-TAA setup for performance
- port or validate a technique from a reference architecture (prepass, deferred, VRS,
  cluster or visibility buffer, half-res effects, temporal pipeline)
- explain an RGP/RenderDoc trace
- decide between API options: layout, load/store op, format, resolve mode, MSAA count,
  descriptor strategy, queue family, barrier scope

Do **not** use it for correctness debugging (validation errors), shader compilation
errors, or non-Vulkan APIs except as contrast.

---

## 1. Non-negotiables

1. **Measure before, measure after.** No change ships on reasoning alone. If you cannot
   measure, produce a measurement plan and stop — do not "optimize" blind.
2. **One variable per iteration.** Two simultaneous changes produce an unattributable win.
3. **Never trade correctness or asked-for quality silently.** If a change alters image
   quality (MSAA → post-AA, half-res effect, lower precision), state the trade explicitly
   and let the user choose.
4. **Respect the frame budget shape.** A 3 ms win in a pass that costs 0.4 ms is not a win;
   find the pass that owns the time.
5. **Cite the mechanism.** When you claim "the driver will/won't do X", say which Mesa code
   decides it (see `references/02-mesa-runtime-semantics.md`). If you do not know, say
   *unverified* and design a test.

---

## 2. Cost model

Five budgets. Map every problem to exactly one before proposing a fix, because the fix for
one budget is usually neutral or harmful for another.

| Budget | Symptom in a trace | Cheapest wins |
|---|---|---|
| **A. Pass/sync count** | many small passes, a barrier between each, resolve chains | merge passes, keep work in one render pass, `LOAD_OP_DONT_CARE`, local-read layouts |
| **B. Bandwidth (bytes)** | high bytes read/written, L2 traffic, cache-miss counters | compression (DCC/CMASK/HTILE), narrower formats, half-res, removing needless store/resolve, dropping redundant MRTs |
| **C. Invocations** | high VS/PS/CS wave counts, quad overdraw, over-shade | depth prepass, tighter culling, VRS, deferred shading, resolve at consumed size |
| **D. Fixed-function throughput** | low utilization with few waves, primitive-limited frames, tiny draws | batch draws, compaction, fewer small prims, guardband/clip settings, IA packing |
| **E. Occupancy / latency** | low waves-in-flight per CU, stalls on memory or barriers | LDS/VGPR budget, wave size, removing spills, better cache locality, fewer fake dependencies |
| **F. Access efficiency** | high DRAM bytes for few useful bytes; L2 sectors >> payload | AoS→SoA, 32 B-sector alignment, `vec4` loads, padding strided walks, UBO instead of SSBO for per-draw constants |

Order of operations: **A → B → C → D → F → E.** Structural wins dominate micro-optimisations; a
merged pass usually beats any shader tweak. Only enter F and E when A–D are exhausted.

For F and E the arithmetic is in `references/09-architecture-occupancy-memory.md`: resident waves,
bytes-in-flight (`bandwidth × latency`), and sector efficiency. Compute the number before proposing a
change, because both budgets are quantitative — "occupancy is low" and "coalescing is bad" are
sentences that must contain a figure.

---

## 3. Workflow

### Step 1 — Establish the budget

Get frame time, target, and GPU. Ask for or produce per-pass GPU timestamps
(`vkCmdWriteTimestamp2` either side of each pass) plus the top 5 passes by cost, into
`assets/pass-budget.md`.

**Exit criteria:** a pass list with measured ms, sorted, and a statement of which pass must
lose time. Never proceed on "the whole frame is slow".

### Step 2 — Classify every pass

- **MANDATORY** — output is consumed and cannot be derived more cheaply.
- **DERIVABLE** — could be computed inside another pass, at a cheaper resolution, or by
  hardware (compression, fast clear, resolve).
- **REMOVABLE** — output unused, cleared then overwritten, resolved then never sampled at
  that size, or a copy that exists for API convenience.

A pass that is cleared and then fully overwritten in the same frame is always REMOVABLE.

### Step 3 — Pick the bottleneck budget

Use `references/01-measure.md`. Write the hypothesis as a sentence containing a number:
"G-buffer bandwidth is 2.1 GB/frame because 3 MRTs at 4x MSAA resolve at full resolution."

### Step 4 — Run the Mesa check

Before changing the engine, predict what the driver will do. The most common finding in a
Vulkan renderer is not "the shader is slow" but **"the API usage made the driver disable a
hardware path"**: compression off, fast clear ineligible, binning throttled, resolve
serialised, barriers forcing flush+invalidate.

Check, in this order:

1. Is this image **compressible** in the layout we use it in?
2. Is this clear **fast-clear eligible**?
3. Does this resolve need to happen at all, at this size?
4. Is this barrier **wider than the data dependency**?
5. Is this draw **packing into the VGT** at all?

### Step 5 — Propose changes, ranked

Emit a ranked list (`assets/report-template.md`): expected win, confidence, risk,
measurement method. Rank by estimated bytes or invocations removed, not by how interesting
the change is.

### Step 6 — Verify

Re-run the same measurement. Report delta per pass and per budget. If a change did not move
the number, **revert it** — an unmeasured "optimization" left in the codebase is future risk.

### Step 7 — Record the finding

Append to `assets/findings-log.md`: pass, change, budget, before/after, Mesa mechanism.

---

## 4. Decision tables

### 4.1 Attachment load/store (RADV)

| Situation | Correct op | Why (Mesa logic) |
|---|---|---|
| Contents fully written this pass | `LOAD_OP_DONT_CARE` | Nothing to preserve; `CLEAR` costs fast-clear setup, `LOAD` costs a decompress (no tile fetch exists on desktop Mesa) |
| Must start from a known constant, then fully written | `LOAD_OP_CLEAR` | Eligible for fast clear if layout + usage allow (`02` §2) |
| Contents preserved | `LOAD_OP_LOAD` | Only when actually read |
| Never read again this frame | `STORE_OP_DONT_CARE` | Removes tile writeback / decompress-on-store |
| Resolve needed | `STORE_OP_STORE` + resolve attachment | Prefer render-pass resolve; a separate resolve is a second pass over the data |

### 4.2 Image layouts (the compression table)

On RADV, compression and fast clear are layout- and usage-conditional. Summary of what
`radv_layout_dcc_compressed`, `radv_layout_is_htile_compressed` and
`radv_layout_can_fast_clear` implement:

| Layout | DCC (color) | HTILE (depth) | Fast clear |
|---|---|---|---|
| `COLOR_ATTACHMENT_OPTIMAL` / `ATTACHMENT_OPTIMAL` | yes | yes | yes, with write usage |
| `DEPTH_ATTACHMENT_OPTIMAL` / `STENCIL_ATTACHMENT_OPTIMAL` | — | yes | yes |
| `SHADER_READ_ONLY_OPTIMAL` | yes | TC-compat only | no (not a write layout) |
| `GENERAL` | conditional (compute/transfer-queue caveats) | TC-compat only; RADV deliberately allows it for main depth passes | case-by-case |
| `ATTACHMENT_FEEDBACK_LOOP_OPTIMAL` | **never** | **never** | no |
| `TRANSFER_DST_OPTIMAL` | conditional | conditional | no |

Rules that follow:

- Do not use `GENERAL` as a convenience layout for attachments you also sample.
- Do not read+write the same attachment (feedback loop) if you can restructure into
  read-only depth plus a separate pass; feedback loops force decompression.
- Read-only images keep compression always: make textures `SHADER_READ_ONLY_OPTIMAL`.

### 4.3 Prepass decision (depth / alpha-tested)

A prepass pays **vertex cost + depth bandwidth** and saves **pixel shading of hidden
surfaces**. It is a win only when

```
saved_ps_invocations * ps_cost  >  prepass_vertex_cost + depth_traffic
```

Gate it:

```
if overdraw_ratio > 1.6 and ps_cost_per_pixel >= 8 * depth_cost_per_pixel:
    prepass is likely to pay
elif material samples many textures or uses alpha test:
    prepass usually loses (alpha-tested prepass ~ full material cost)
else:
    skip it; fix draw order and culling instead
```

Full treatment: `references/03-passes-and-bandwidth.md` §3.

### 4.4 MSAA and resolve

| Question | Rule |
|---|---|
| Deferred / G-buffer MSAA? | No — MSAA multiplies G-buffer bandwidth; use post-AA plus coverage tricks |
| Forward MSAA? | Yes at 2x/4x on edges |
| Resolve resolution | Resolve at the size you consume; never full-res resolve for half-res compositing |
| Resolve mechanism | Render-pass resolve first; `vkCmdResolveImage` is a real pass on RADV (MSAA resolve is a pixel shader, `02` §5) |
| Transient attachment | Use `LAZILY_ALLOCATED` / local memory for attachments that never leave the tile |

### 4.5 Batching and small draws

The VGT packs draws into primgroups; below that granularity a draw is mostly setup. Signals:
many draws under ~100 primitives, low waves per draw, high state-change rate.

Actions: merge static draws that share pipeline+state, use indirect multi-draw, move
per-object data to a ring buffer plus push constants, stop rebinding pipelines per object.

### 4.6 Occupancy

If waves-in-flight per CU is low and no bandwidth counter saturates, the shader is
resource-bound: VGPR spills, excessive LDS, oversized workgroups.

- Keep the common path spill-free; spills are local-memory traffic that competes with
  bandwidth budget B.
- Tessellation workgroups cap LDS at 32K deliberately — 64K evicts GS/PS from the CU.
- Prefer wave32 for small divergent pixel work and wave64 for wide uniform compute, but
  wave size changes LDS/VGPR per wave; re-measure.

---

## 5. Hard "do not" list

Almost never wins, common in production code. Flag them; require a measurement.

1. `vkCmdClearAttachments` inside a render pass in place of load/store ops.
2. `LOAD_OP_LOAD` on an attachment you fully overwrite (costs a decompress on AMD/Intel; `storeOp`
   folklore aside, `LOAD` is the one that actually costs).
3. `VK_IMAGE_LAYOUT_GENERAL` for anything you do not have to (kills compression paths).
4. Full-resolution resolves of attachments consumed at half or quarter resolution.
5. Per-frame `vkCreateImageView` / descriptor-set reallocation instead of caching.
6. One "safe" `vkCmdPipelineBarrier2` per pass with `ALL_COMMANDS` / `MEMORY_READ|WRITE`.
7. Reading and writing the same attachment in one pass (feedback loop) without cause.
8. MSAA on a deferred G-buffer "because the pipeline already had MSAA".
9. `vkQueueWaitIdle` / `vkDeviceWaitIdle` in the frame loop.
10. Micro-optimising shader ALU in a bandwidth- or primitive-bound pass.
11. Adding a prepass without measuring overdraw first.
12. Switching to narrower formats without checking compression eligibility and precision.
13. "Increasing occupancy" by changing the workgroup size when the limit is VGPRs or LDS
    (`09` §2.2), or claiming a coalescing win without a sector count.
14. Drawing a sample parameter from a domain that a symmetry already covers (derived antipode,
    mirror, `1−u` partner, rotated phase set) — this halves effective sample count silently
    (`06` §8). Always state `N` and `N_eff` for an RT/GI/AO tap budget.
15. Leaving a ray-marched pass to converge on a fixed world-space epsilon at one mip level: the
    ε-tail costs the last steps of every pixel and makes the hit point (and therefore any
    derivative-reconstructed normal) depend on where the loop stopped (`10` §2–§3).
16. Reading through a write-combined (host-visible, non-`HOST_CACHED`) mapping, or writing it with
    strides smaller than a cache line — reads are ~100× slower and strided stores defeat
    write-combining entirely (`05` §6.5).
17. Reaching for 1024-invocation workgroups because the API permits them, or asking for `shared`
    size close to the CU's LDS: both have zero slack and halve occupancy on the first overshoot
    (`09` §3A).

---

## 6. Expected ROI ranking (prioritise, then verify)

| Rank | Change | Typical win | Confidence | Reference |
|---|---|---|---|---|
| 1 | Remove a pass / merge passes / drop redundant copies | large | high | 03 §1 |
| 2 | `DONT_CARE` load/store instead of clear/load/store | medium–large | high | 03 §2 |
| 3 | Restore compression (layout/usage/format fixes) | medium–large | high | 02 §2 |
| 4 | Resolve and half-res where consumed | medium | high | 03 §5, 06 §2 |
| 5 | Draw batching and state reduction | medium | high | 04 §2 |
| 6 | Depth prepass (only when measured) | medium | medium | 03 §3 |
| 7 | Culling improvements (NGG culling, indirect compaction) | medium | medium | 04 §3 |
| 8 | Occupancy / VGPR / LDS tuning | small–medium | medium | 04 §4, 09 §2 |
| 9 | VRS / shading rate on measured over-shade | small–medium | medium | 04 §5 |
| 9.5 | Sample-count efficiency: remove duplicated sample orbits (paired/antithetic rays, phase sets) | medium–large (up to 2×) | high | 06 §8 |
| 9.6 | Ray-march step budget: mip/cone LOD + analytic last step | medium–large in traced passes | high | 10 §1–§2 |
| 9.7 | Persistent mapping + direct writes into VRAM (drop the staging copy, never read the mapping) | small–medium CPU, removes upload stalls | high | 05 §6.5 |
| 9.8 | Workgroup size / LDS-staging restructure (large-group budget, halo economics) | medium–large in compute passes | high | 09 §3A |
| 10 | Shader ALU micro-optimisation | small | low | — |

---

## 7. Reference index

Read only what the current step needs.

| File | Read when |
|---|---|
| `references/01-measure.md` | establishing or interpreting measurements |
| `references/02-mesa-runtime-semantics.md` | predicting driver behaviour; compression, fast clear, resolve, binning |
| `references/03-passes-and-bandwidth.md` | pass structure, clears, overdraw, prepass, G-buffer, MSAA, resolve, half-res |
| `references/04-geometry-and-shading.md` | vertices, IA, culling, mesh shaders, occupancy, waves, LDS/VGPR, VRS |
| `references/05-sync-descriptors-memory.md` | barriers, layouts, queues, descriptor sets, push constants, aliasing, host-visible streaming/upload memory |
| `vulkan-barriers-and-begin-rendering-2026.md` (companion, outside the skill dir) | full source-verified barriers + `vkCmdBeginRendering` reference on RADV/ANV/NVK: every stage/access mask mapped to hardware, layout→compression rules, `loadOp`/`storeOp`/`resolveMode` behaviour |
| `references/06-temporal-and-post.md` | TAA/velocity, motion blur, tonemap/gamma ordering, post-chain cost, sample budgets and paired-sample symmetry |
| `references/07-mesa-source-index.md` | you need the code that implements a mechanism |
| `references/08-known-traps.md` | a symptom looks familiar; check before theorising |
| `references/09-architecture-occupancy-memory.md` | occupancy, waves, VGPR/LDS limits, sector efficiency, bytes-in-flight, workgroup sizing |
| `references/10-ray-marched-and-screen-space.md` | SDF/voxel/screen-space tracing: step-count and mip-cone LOD, analytic last-step convergence, hit-point precision and normal reconstruction |
| `OPTIMIZATION-CATALOG.md` | one-page ranked cheat sheet: what it saves / Mesa logic / action / verify |
| `assets/report-template.md` | producing the deliverable |
| `scripts/vk_scan.py` | first pass over a codebase to find API-level anti-patterns |

---

## 8. Output contract

When you finish an optimization round, produce exactly this, in this order:

1. **Frame budget** — table of passes with before/after ms.
2. **Bottleneck statement** — one sentence, with a number, naming the budget (A–E).
3. **Changes** — ranked; each with what, why (mechanism + cite), expected effect, risk,
   how it was measured.
4. **Deltas** — measured before/after per affected pass, plus explicit "no change ⇒ reverted".
5. **Rejected ideas** — what you considered and why it is not a win.
6. **Next measurement** — the single highest-expected-value next experiment.

Never present a change list without item 4.
