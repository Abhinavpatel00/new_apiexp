---
name: vulkan-shader-opt
description: >
  Optimize the shaders themselves in a Vulkan renderer: instruction selection and ALU
  cost, precision and packing, interpolants and register pressure, texture/sampler access
  patterns, derivatives, control flow and subgroup operations. Use when the frame is
  already structurally sound but a shader is the cost, when porting DX11-era shader tricks
  to SPIR-V, or when deciding which 2013-era shader advice still holds on 2026 hardware.
  Covers algebraic/mathematical simplification and locality (registers, schedule, lanes,
  cache), with a Slang/slangc workflow for compiling shaders to SPIR-V and reading back
  exactly what the front end did and did not optimize. Grounded in measured slangc output,
  Mesa NIR/ACO source, and vendor ISA documentation, not folklore.
license: MIT
version: 1.1
---

# Vulkan shader optimization (Mesa-grounded)

This skill is the **shader-level** companion to `vulkan-opt` (pass structure, bandwidth,
barriers, occupancy at the dispatch level). Use `vulkan-opt` first: a shader rewrite cannot
fix a pass that should not exist. Once the pass structure is right and a specific shader is
provably the cost, work here.

Your job: make a shader cheaper **without changing what it computes**, and be able to prove
the change in the ISA.

Every recommendation must be one of:

1. **Fewer instructions** (delete work, fold constants, hoist to scalar/uniform, replace a
   sequence with one hardware instruction), or
2. **Cheaper instructions** (move work from a quarter-rate pipe to a full-rate one, from
   vector to scalar, from 32-bit to 16-bit packed), or
3. **Cheaper resources** (fewer VGPRs → more waves, fewer interpolants → less LDS, fewer
   descriptor fetches), or
4. **Fewer invocations doing the work** (divergence, early-out, quad control, waveform ops).

If a proposal is not one of those four, it is style, and style is not a performance claim.

---

## 1. Non-negotiables

1. **Dump the ISA before and after.** A shader change that is not visible in the ISA is not
   a shader optimization. On RADV: `RADV_DEBUG=shaderstats` (counts) and RGP/RGA for the
   disassembly; on Intel: `INTEL_DEBUG=asm`; on NVIDIA: `nvdisasm`/Nsight.
2. **Instruction count is not time.** Persson said it in 2013 ("Instruction count not
   indicative of performance anymore") and it is more true now: a 3-instruction sequence on
   the full-rate pipe can lose to a 1-instruction sequence that serialises a dependency or
   blows the wave count. Attribute every claim to a pipe, a latency, or a register.
3. **Never trade correctness for instruction count silently.** Precision reductions
   (`mediump`, `RelaxedPrecision`, fp16) change the numerics; state the change and the
   tolerance in the report.
4. **Check the register count.** On AMD, VGPRs decide waves/SIMD (`vulkan-opt` `09` §2.2):
   48 VGPRs at 16 waves/SIMD on GFX12. A rewrite that saves two ALU instructions but adds
   two VGPRs can lose.
5. **Cite the mechanism.** "The compiler will fold that" is a claim about NIR/ACO; say
   which pass (`references/07-mesa-shader-code-index.md`), or mark it *unverified* and test.
6. **Check the front end before blaming the driver.** Compile the shader yourself and read the
   SPIR-V: `slangc x.slang -target spirv -entry main -stage fragment -O3 -o x.spv` then
   `spirv-dis x.spv`. Measured with slangc 2026.21: `pow(x,2.0)`, `exp(log(x))`, `a*b+c`
   (no FMA) and `i/3` all survive `-O3` unchanged (`references/10-slang-toolchain-workflow.md` §2).

---

## 2. The optimization taxonomy (where each class lives)

Nine classes of shader optimization. If a proposal does not fit one of them, it is not an
optimization. Each row says what the lever is and which reference has the method.

| # | Class | The lever | Reference |
|---|---|---|---|
| 1 | **Delete work** | constant folding, dead-code removal, hoisting, precompute to a UBO/spec constant | `02` §2, `03` §4 |
| 2 | **Algebraic / mathematical** | rewrite the expression: powers→multiplies, `exp2`, squared-length compares, factoring, reciprocal forms, strength reduction of integer divide | `08` §1–§3 |
| 3 | **Instruction selection** | express the op the hardware has: `mad`, `dot`, `saturate`, `med3`, `rsqrt`, `FMix`, bitfield ops, packed 16-bit | `02` §2, §4–§5 |
| 4 | **Precision and range** | fp16/packed math, 16-bit storage and indices, relaxed precision — with a stated tolerance | `11`, `02` §5, `08` §4 |
| 5 | **Locality** | register live ranges; dependency chains and load-to-use distance; lane/sector/bank locality; thread coarsening; dispatch order across thread groups (L2); cache reuse across passes | `09`, `13` §2 |
| 6 | **Control flow** | make branches wave-uniform, demote vs discard, quad control, loop restructuring, and the depth-test/early-Z consequences of `discard`/depth export/UAV writes | `05`, `12` §1 |
| 7 | **Memory access shape** | fetch vs gather vs sample, explicit LOD, narrower formats, SoA, coalescing | `04`, `vulkan-opt` `09` §4 |
| 8 | **Variants / specialization** | über-shader splits, spec constants, pipeline permutation cost, and which properties *cannot* be branched | `03` §4, `10` §3, `12` §3 |
| 9 | **Invocation count** | early-out, coverage, CullInstanceCount-style amplification, workgroup shape | `05` §2, `vulkan-opt` `09` §3A |

**Verification ladder** for any of the nine: source → `slangc` → SPIR-V (`spirv-dis`) → NIR/ACO
pass name → ISA → frame time. Stop at the first rung where the claim dies, and report which rung
the claim was checked on (`10` §4).

---

## 3. Cost model

Shaders are **instruction-mix bound**, not instruction-count bound. Four classes:

| Class | Typical members | Note |
|---|---|---|
| **Full rate** | float add/sub/mul/fma, int add/sub/mul24, logic, type conversion, floor/ceil/round, min/max/median | one per lane per cycle class |
| **Half rate** | packed 16-bit math (two lanes' worth per instruction), fp64 add, some conversions | gives back VGPRs *and* throughput |
| **Quarter rate** | transcendentals (`sin`, `cos`, `exp2`, `log2`, `rcp`, `sqrt`, `rsqrt`) | one hardware instruction, four times the cost — still far better than a software sequence |
| **Super expensive** | integer divide/modulo (emulated, tens of instructions), inverse trigonometry, 64-bit divide, denormal-handling paths | avoid in inner loops; see `02` |

Plus three non-ALU budgets that usually dominate:

- **Interpolants** — every VS→FS varying is an interpolation instruction per component *and*
  (on AMD) LDS space per wave. See `03` and `vulkan-opt` `09` §2.3.
- **Memory instructions** — fetches are priced in sectors, not instructions (`vulkan-opt`
  `09` §4). A shader can be instruction-light and still lose to a bad access pattern.
- **Occupancy** — waves/SIMD; VGPRs, LDS, and (Intel) GRF mode.

---

## 4. Workflow

### Step 1 — Prove the shader is the cost

You need one of: a per-pass GPU timestamp showing the pass is over budget, a shader-timing
capture (RGP per-shader, Nsight per-kernel), or a measured A/B of the shader with geometry
held constant. "The shader looks expensive" is not evidence.

**Classify the bottleneck before you classify the shader** (`13` §3, the P3 method): find the unit
with the highest percentage of its own peak throughput. If no unit is near 80%, the workload is
latency/occupancy-bound and instruction work is not the answer; if a unit is above 80%, the fix is
to remove work from that specific unit. Only when the SM is the top unit does "make the shader
cheaper" become the right first move.

**Exit criterion:** the shader's cost as a fraction of the frame, and the pipe it is bound on
(VALU / memory / interpolation / occupancy).

### Step 2 — Read the ISA

- **AMD**: RGP (wave timeline, stall reasons) or `RADV_DEBUG=shaderstats` plus the ACO
  disassembly; RGA for offline iteration on the same shader without the app.
- **Intel**: `INTEL_DEBUG=asm` / `perf`; the oneAPI GPU Optimization Guide for the ISA.
- **NVIDIA**: Nsight Compute's source/ISA view (`nvdisasm` for the cubin).
- **Front-end layer (portable, always available)**: compile with `slangc` yourself and read the
  SPIR-V with `spirv-dis`; this is the only view that tells you whether *your source* produced
  the instructions you think it did, before the driver rewrites anything
  (`references/10-slang-toolchain-workflow.md`).
- **Portable sanity layer**: `spirv-dis` on the module — check that your source actually
  produced the operations you expect (no surprise promotions, no `RelaxedPrecision` where
  you needed precision, no `OpFRem` where you meant `mod`).

**Exit criterion:** the top 3 instruction groups by cost, named.

### Step 3 — Change one thing, re-dump

Keep a table: change / instructions / VGPRs / measured time. A rewrite that does not move a
number in that table gets reverted. Use the catalogue in `06-persson-scorecard.md` for the
list of known wins and known traps.

### Step 4 — Record the constraint

If the win depends on a fact the compiler cannot see (range of an integer, uniformity of a
branch, precision tolerance), it must be expressed **in the shader or pipeline**, not in a
comment: spec constants, `nonuniformEXT`, `subgroupUniformControlFlow`, `RelaxedPrecision`,
`mediump`, `flat`. Unexpressed assumptions get optimized away by the next compiler version.

---

## 5. Decision tables

### 5.1 By binding resource

| Bound on | First three moves | Reference |
|---|---|---|
| VALU (instruction issue) | fold to MAD/FMA form; use inline constants; replace transcendentals with cheaper identities; check for `sign`/`pow`/`normalize` in loops | `02` |
| Registers (waves/SIMD low) | shorten live ranges; move wave-uniform work to uniform/scalar path; pack to 16-bit; recompute instead of keep | `03` |
| Interpolation / LDS | cut varyings; `flat` on constants; compute cheap values in FS | `03` |
| Texture / TMU | `texelFetch` where filtering is not needed; `textureGather` for 4-tap; move fetches out of loops; reduce format width | `04` |
| Branching / divergence | make the branch wave-uniform; use `subgroupUniformControlFlow`; demote instead of branch | `05` |
| Overdraw / depth-test efficiency | remove `gl_FragDepth` writes; drop SSBO writes from early-Z passes; conservative depth output; front-to-back or depth prepass | `12` §1 |
| L2 hit rate low in a full-screen compute pass | classify the top unit first; if it is VRAM/L2 and the kernel gathers a neighbourhood, tile the thread-group IDs | `13` §2–§3 |
| Memory (bytes) | this is `vulkan-opt`'s problem, not a shader trick — fix the access pattern | `vulkan-opt` `09` §4 |
| Algebra (what the compiler left undone) | fold powers/roots to multiplies, `exp2` for `exp`, squared-length compares, factor common terms | `08` §1–§2 |
| Dependency chains (ILP) | split accumulators, put independent work between a fetch and its use, reorder for load-to-use distance | `09` §2 |
| Lane locality (sectors, banks) | consecutive lanes → consecutive addresses; de-stride `shared`; coarsen threads; wave-uniform branches | `09` §3 |

### 5.2 By symptom in the ISA

| Symptom | Likely cause | Fix |
|---|---|---|
| `v_mul` + `v_add` instead of one `v_fma`/`v_mac` | expression written so the intermediate must be kept, or precision barrier | rewrite as `a*b+c` in one expression (`02` §2) |
| Long `v_rcp`/`v_sqrt` chains at quarter rate | `normalize()` on an unnormalised vector, `pow(x,2)` | use `inversesqrt`, `x*x` (`02` §3) |
| `v_med3_f32` pairs around a value | `saturate()`/`clamp` — this is fine, it is one instruction | leave it (`02` §4) |
| `v_mul_legacy_f32` + `v_med3_f32` for `sign()` | `sign()` lowering | replace with comparison/bit trick (`02` §4) |
| `v_cndmask_b32` × N in a row | vectorised `?:` | that is correct and cheap; do not "fix" it (`05` §2) |
| `s_cbranch` vs `v_cbranch` | uniform vs divergent branch | make it uniform if it can be (`05` §1) |
| many `ds_*` (LDS) instructions | interpolation staging or real `shared` use | cut varyings; see `03` |
| spilled VGPRs | register pressure | shrink live ranges, split the shader (`03` §3) |
| `OpExtInst Pow` in the module | `pow()` with a constant exponent survived the front end | `x*x`, `sqrt(x)`, `exp2` (`08` §1) |
| `OpSDiv` / `OpSRem` in the module | integer divide/modulo by a constant survived the front end | shift/mask or magic multiply (`08` §3) |
| `OpFMul` + `OpFAdd` where an FMA was meant | source has no `mad()`/`fma()`, and no compiler in the chain forms one | write `mad()` (`08` §1) |

---

## 6. Hard "do not" list

1. Do not micro-optimize a shader in a pass that is bandwidth-, ROP-, or primitive-bound;
   the instructions are not the cost (`vulkan-opt` `01` §1).
2. Do not rewrite math into a form that changes denorm/NaN behaviour, then claim it is a win
   without testing the debug or HDR path.
3. Do not use `pow(x, 2.0)`, `pow(x, 0.5)`, `pow(x, 3.0)` — `x*x`, `sqrt(x)`, `x*x*x`
   (`02` §3).
4. Do not call `normalize()` on a vector you already know is unit length; do not call it per
   iteration in a loop.
5. Do not use `sign()` for a sign test; do not emulate booleans with `float`/`int`.
6. Do not branch on a per-lane condition to skip a few instructions: the branch costs more
   than the instructions unless the skipped block is large (`05` §1).
7. Do not rely on `discard` for early-out in a shader whose depth/early-Z you care about;
   prefer `demote` semantics and check the compression consequences.
8. Do not ship an über-shader "because the branch is uniform anyway" without specialization
   constants or pipeline variants (`03` §4).
9. Do not add `mediump`/`RelaxedPrecision` globally to "make it faster" — it changes results
   and can *pessimize* (extra conversions) when the value is used at full precision later. On
   RADV it does nothing at all: the AMD backend has no relaxed-precision handling (`11` §2).
10. Do not claim an instruction-count win without the VGPR count of both versions.
11. Do not assume any particular fold happens. Compile it and look: every "the compiler will
    handle that" claim in this skill is backed by either a measured module or a named Mesa pass.
12. Do not optimize math in a shader whose bottleneck is memory, interpolation, or occupancy —
    check the cost classification first (`01` §1, `09`).
13. Do not call fp16 a throughput win without `v_pk_*` in the ISA: a single scalar `half` op still
    occupies a whole packed instruction, so it buys registers, not rate (`11` §4).
14. Do not write `gl_FragDepth` or a storage buffer in a fragment shader you want early-Z'd: depth
    export and `writes_memory` both force late-Z, and a `discard` anywhere in the module sets a
    shader-wide kill bit even if it never executes (`12` §1).

---

## 7. Expected ROI ranking

| Rank | Change | Typical win | Confidence | Reference |
|---|---|---|---|---|
| 1 | Delete the computation (algebraic simplification, precompute to constant, hoist out of loop) | large | high | `02` §2, `08` §2 |
| 1a | Fix a transcendental that the compiler did not fold (`pow(x,k)`, `exp(log)`, `length` compares) | medium–large | high | `08` §1–§2 |
| 1b | Strength-reduce integer divide/modulo in an inner loop | medium–large | high | `08` §3 |
| 2 | Cut interpolants / compute in FS instead of interpolating | medium–large | high | `03` §1 |
| 3 | Replace a software sequence with a hardware instruction (transcendental, `saturate`, `med3`, dot-product) | medium | high | `02` §3–§4 |
| 4 | Precision/packing: fp16 pair for color/UV work, 16-bit storage, indices | medium | medium | `11` |
| 5 | Texture access: `texelFetch`/`gather`, fewer fetches, narrower formats | medium | medium | `04` |
| 6 | Uniform/scalar-ize per-lane math that does not vary per lane | medium | medium | `02` §6 |
| 7 | Branch restructuring / quad control | small–medium | medium | `05` |
| 7a | Keeping early-Z alive (no depth export, no SSBO writes, no dead `discard`) | medium (overdraw-dependent) | high | `12` §1 |
| 7b | Thread-group ID tiling for a VRAM-limited full-screen compute pass | large when it applies (47% in the published case) | medium — three preconditions | `13` §2 |
| 8 | Integer math (mul24, no divide, bitfields) | small–medium | high | `02` §7 |
| 9 | Subgroup ops instead of LDS + barrier | small–medium | medium | `05` §3 |
| 10 | Instruction scheduling polish (MAD form, inline constants, instruction order) | small | low | `02` §2 |
| 11 | Dependency-chain / ILP restructuring | small–medium | medium | `09` §2 |
| 12 | Lane locality: de-striding, coarsening, wave-uniform branches | medium (data-dependent) | medium | `09` §3 |

---

## 8. Reference index

| File | Read when |
|---|---|
| `references/01-cost-model-and-measurement.md` | deciding what to measure, or interpreting an ISA dump |
| `references/02-alu-precision-and-packing.md` | the shader is VALU-bound; precision/packing questions |
| `references/03-interpolants-registers-variants.md` | varyings, register pressure, über-shaders, vertex input |
| `references/04-texturing-derivatives-formats.md` | texture fetches, gather, LOD, derivatives, format choices |
| `references/05-control-flow-and-wave-ops.md` | branching, divergence, discard/demote, subgroup operations |
| `references/06-persson-scorecard.md` | someone cites a DX11-era shader trick and you need to know if it still holds |
| `references/07-mesa-shader-code-index.md` | you need the NIR/ACO file that implements a decision |
| `references/08-mathematical-optimization.md` | the algebra itself: identities, strength reduction, error budgets, and the measured table of what the toolchain folds |
| `references/09-locality-and-scheduling.md` | registers, dependency chains, lane/sector/bank locality, thread coarsening |
| `references/10-slang-toolchain-workflow.md` | compiling with slangc and reading SPIR-V: exact commands, measured `-O3` behaviour, Slang features and their SPIR-V consequences |
| `references/11-fp16-and-16-bit-precision.md` | fp16 in Vulkan: relaxed vs explicit precision, the capabilities each needs, why scalar `half` saves registers but not throughput, and a 2026 scorecard of the 2019 fp16 advice |
| `references/12-therealmjp-audit.md` | early-Z/late-Z as a *derived* state (with the RADV line that proves it), what the driver already does for you (`allow_flat_shading` → 2×2 VRS), which properties must be permutations, and an index triage of the whole therealmjp blog |
| `references/13-bavoil-l2-locality-and-p3.md` | thread-group ID swizzling for L2 locality (with a Slang version that is safe for any dispatch dim, and its measured cost), plus the P3 bottleneck-classification method and how to run it on RADV/RGP |
| `references/14-gist-corpus-triage.md` | you want the index of the *GPU Optimization for GameDev* gist: what is in it, what has landed where, what is queued, and what it does not contain |
| `references/15-effects-pass-topology.md` | an effect pass is slow: fullscreen topology (2-tri vs 1-tri vs compute, with hit rates), LDS store-caching for separable filters, and sampling/execution order |
| `references/16-lowlevel-2023-retest.md` | someone quotes Persson at you, or you want to know which of the 2013/2014 claims survived a 2023 re-test on RDNA2 — plus 53 practical shader tips grouped by the decision they serve |

## 8a. Toolchain used to verify this skill

| Tool | Version | Used for |
|---|---|---|
| `slangc` | 2026.21 | compiling every example to SPIR-V (`-target spirv -O3`), `-dump-ir` for pass attribution |
| `spirv-dis` | SPIRV-Tools v2025.1 | reading the module back: opcode histograms, surviving `OpExtInst`/`OpSDiv` |
| `spirv-opt` | SPIRV-Tools v2025.1 | `--relax-float-ops` experiments (measured: 33 `RelaxedPrecision` decorations on a small shader, `11` §2) |
| Mesa | 27.0.0-devel, `ac271714ff58086430a3817554a4b9ac0e5929d9` | NIR/ACO source anchors for every backend claim |

## 9. Output contract

For every proposed shader change, report:

```
Shader:      <stage, entry point, pipeline/draw it belongs to>
Cost today:  <ms or % of frame, and the pipe it is bound on>
Change:      <one sentence, with the algebra or the intrinsic named>
ISA delta:   <instructions before → after, per the actual dump>
Registers:   <VGPR/SGPR/GRF before → after, and waves/SIMD if AMD>
Numerics:    <unchanged | changed, with tolerance and why it is acceptable>
Risk:        <what breaks if the assumption fails>
Verify:      <the measurement that proves it>
```
