# `vk_v2.h` skills audit — 2026-11-08

**Status: static / design round, NOT a GPU optimization round.**
GPU target: RX 9070 XT (Navi48 / GFX12), RADV Mesa `27.0.0-devel`, 2560×1440.
Toolchain here: `slangc 2026.21`, `spirv-dis 2025.1`, GCC 14 (`-std=c11 -Wall -Wextra -Werror`).
No frame timestamps or ISA dumps were collected in this workspace. Every "ms" below is an
**analytic floor derived from measured bandwidth**, not a measured pass time. No speedup
percentage is claimed anywhere in this document or in the header.

This document is the finding → change → mechanism → measurement ledger promised by
`shaders/mu/README.md` and the top banner of `src/vk_v2.h`. It follows the `vulkan-opt`
output contract (§8) in order: frame budget, bottleneck statement, ranked changes, deltas
with explicit reverts, rejected ideas, next measurement. Section 7 adds the per-shader
contract from `vulkan-shader-opt` §9.

---

## 0. Inputs

| Input | What it contributed |
|---|---|
| `/home/user/skills/vulkan-opt` | budgets A–F, order A→B→C→D→F→E, `vk_scan.py`, report template, hard "do not" list |
| `/home/user/skills/vulkan-shader-opt` | source→slangc→SPIR-V→NIR/ACO→ISA ladder, 20 probes, `shader_scan.py`, §9 output contract |
| `AGENTS.md` §3/§4.8/§12 | layout before algorithms, minimum bits, naming and aligned declarations |
| `.github_copilot-instructions.md` | **no vertex/index buffers**, single descriptor set/layout, bindless IDs + offsets, 16-byte push, minimal local changes |
| `README.md` | negative-height viewport coordinate table (must survive verbatim) |
| `renderer.txt` | pass chain → `PassKind` |
| `dataflow.txt` | ownership map; **a settled scene uploads nothing** |
| `todo.txt` | in-process shader compiler, own enums → forward surface |
| `docs/api-v2-design.md` §1 | unfulfilled promise: expose per-pipeline VGPR count |
| `docs/pipeline-explosion.md` | `PipeKey`, 19→7 pipelines, context rolls are real |
| `src/vk_v2.h` | the API being designed; read in full, no sections reordered |
| `shaders/mu/*` | the shader-side numbers the CPU budget must agree with |

---

## 1. Frame budget (contract item 1)

The table is a **budget**, not a measurement. "Before/After" are intentionally blank until
per-pass GPU timestamps exist; the analytic floor is what the header can compute today from
format, resolution, and measured bandwidth (644.6 GB/s).

| Pass (`renderer.txt`) | Budget | Before (ms) | After (ms) | Analytic floor | Note |
|---|---|---:|---:|---:|---|
| compute: sim / cull / compact / route split | A, C, E | — | — | — | `ComputeShape` caps workgroup size; `N`, `N_eff` required in the report |
| sky, opaque, MSAA, alpha, transparent | C, D | — | — | — | `PassKind` drives barrier scope; `GeometryRef` removes the vertex-input axis |
| SMAA (fullscreen graphics) | B, F | — | — | 0.092 | one RGBA16F read + write at 1440p |
| SSAO / bloom / tonemap (post compute) | B, E, F | — | — | 0.092 each | ≤ 6 full-screen HDR passes = 0.55 ms floor |
| LDR → swapchain blit | A, B | — | — | — | `PASS_BLIT`; no full-res resolve consumed half-res |
| UI (Nuklear, swapchain-direct) | A | — | — | — | `PASS_UI` |
| **frame** | — | — | — | **≥ 0.55 ms bandwidth floor** | CPU budget ≤ 2.0 ms at 1440p |

Derived numbers used by `FrameBudget` / `frame_program_report`:

- 2560×1440 × 8 B/px × (1 read + 1 write) = **58.98 MB** = **91.5 µs** at 644.6 GB/s.
- Six such passes = **0.549 ms** of unavoidable VRAM traffic; this is why
  `MAX_FULLSCREEN_HDR_PASSES = 6` is a *ceiling*, not a target.
- DRAM/L2 granularity is **32-byte sectors** (`SECTOR_BYTES`); a pass is charged
  `distinct_sectors × 32`, never "useful bytes".
- Full occupancy: **96 VGPRs/thread** (98,304 regs/CU ÷ 1,024 threads = 96). The 48-VGPR
  figure in `vulkan-shader-opt/SKILL.md` §1 is the older GCN 10-wave table and is **not**
  used.
- I-cache: 32 KiB/WGP; LDS: 64 KiB/CU; 32 resident waves/CU.
- bytes-in-flight = bandwidth × latency ≈ 644.6 GB/s × 600 ns ≈ **387 KB per GPU**,
  ≈ 6 KB/CU at 64 CUs (`bytes_in_flight`).

---

## 2. Bottleneck statement (contract item 2)

> **Hypothesis, unverified:** the post chain is bandwidth-bound (budget B) at ≥ 0.55 ms/frame
> because six full-screen RGBA16F passes each pay 58.98 MB of read+write traffic; the 96-VGPR
> full-occupancy budget (budget E) has not been measured on this GPU, so no shader-level claim
> can be ranked above a pass-count claim yet.

`frame_bottleneck()` exists to turn `PassStat.ratio` (measured ms / analytic floor) into one
of `BANDWIDTH` (ratio ≈ 1.05), `LATENCY`, `ALU` (ratio ≥ 3) or `SYNC`. Until timestamps are
fed in, it returns `BOTTLENECK_NONE`; the header does not guess.

---

## 3. Ranked changes applied to `src/vk_v2.h` (contract item 3)

Ranked by estimated bytes / invocations removed, per `vulkan-opt` §3 step 5. All changes are
**additive**: sections 1–15 were extended in place and §16–§19 were appended, so no existing
section moved (verified by `scripts/section_index.py`).

| # | Change | Why (mechanism + cite) | Expected effect | Risk | Measured / verified here |
|---|---|---|---|---|---|
| 1 | `FrameBudget` + `frame_stats_within_budget()` + `FrameStats` ledger (§18): four zeros — queue waits, pipeline creations, descriptor writes, spills | `vulkan-opt` §2 budget A, `SKILL.md` §5.9, `05` §5, §14 bindless single set. A compile or wait landing mid-frame serialises CPU/GPU and is invisible until the frame trace | regression visible in CI instead of only in a profiler | a budget that is too tight blocks legitimate offline work | C11 `-Werror` compile; `vk_scan.py src` → 0 findings; `layer_check.py` → clean |
| 2 | `ComputeShape` / `compute_shape_error()` / `pass_dispatch_2d()` / `pass_dispatch_fullscreen_triangle()` (§16) | `shader-opt` 15 §1: launch topology outranks shader body; 15 §2: one fullscreen triangle gives 95% vs 87% L1/L2 hit; `vulkan-opt` 09 §3A.2: 1024-invocation workgroup has zero occupancy slack | rejects bad launches at frame-build time; removes the two-triangle seam | a validation that is too strict rejects a legal non-tiled dispatch | bijection precondition measured in `validate_math.py` (exact-divisor grids MATCH; 6 non-multiple grids clamp in range but duplicate — hence the assertion) |
| 3 | `ShaderStats` / `pipe_shader_stats()` / `shader_budget_ok()` / `pipe_budget_report()` (§17) | keeps `docs/api-v2-design.md` §1; `shader-opt` §1 non-negotiable 1: a shader crossing 96 VGPRs halves occupancy and no C-side number moves | occupancy regression becomes a build failure | `stats_available=false` on drivers that cannot report → must not assert on zeros | declaration compiles; **ISA/VGPR dump not performed — see §8** |
| 4 | `GeometryRef` / `pass_geometry()` / `GeometryPush` (§19) and the explicit reconciliation of `pass_bind_index` / `pass_bind_vertex` | `.github_copilot-instructions.md`: no vertex/index buffers; `pipeline-explosion.md` M1: vertex layout must not be a pipeline axis. Bindless fetch by `SV_VertexID` from set 0 binding 3 | removes per-draw vertex-input state changes and any chance of a pipeline explosion on stride/attributes | requires every VS to fetch by ID; non-indexed draws need `indices.size == 0` | `GEOMETRY_NIL`, 256-byte push, 16-byte alignment, C11 `-Werror` |
| 5 | `PassKind` in `PassDesc` (§3/§9) | `read_stages` was documented as "0 = derived from pass kind" but the kind did not exist; renderer.txt's chain decides the default barrier scope | fewer, narrower barriers (`vulkan-opt` budget A) | a wrong label widens a barrier | `layer_check.py` section/name check |
| 6 | `PassStat.samples/n_eff/steps_per_pixel/l2_hit_rate/wg_*/tile_n` (§12) + `pass_note_samples()` / `pass_note_dispatch()` | `vulkan-opt` 06 §8: put `N` and `N_eff` next to the pass cost; 10 §4: the step budget is a number in the report; `shader-opt` 13 §3: L2 hit rate decides whether tiling is worth it | makes the "is this bandwidth-bound?" question answerable from the frame report | unmeasured fields must stay 0, not be guessed | C11 compile |
| 7 | `ATT_STORAGE_READ` (§3) | `vulkan-opt` 02 §2.2: a compute pass that only *reads* a storage image must not be forced onto the uncompressed GENERAL path | keeps DCC/compression eligibility | backend must actually derive a read-only layout | appended (no enum value shifted); `layer_check.py` bpp sync |
| 8 | `FMT_RGBA8_SNORM` (§3/§15) | `shader-opt` 04 §3: `*_SNORM` decodes `*2-1` *in the fetch*, removing two ALU ops and the "normal × 2 − 1" precision bug | 2 fewer ALU ops per normal fetch | fetch must not already be UNORM-normalised | appended at the end of `Format` so existing values are stable; `format_bytes` case = 4 = `MU_BPP_RGBA8_SNORM` |
| 9 | skill-derived constants in §1 + `GpuInfo` device facts in §5 | every budget must be a number the CPU and the shader agree on; hardcoding wave size 64 or a 48-VGPR budget is a correctness bug on GFX12 wave32 | portable, self-documenting budgets | drift between header and `shaders/mu/*.slang` | `scripts/layer_check.py` checks 12 constants, 13 bpp values and `MU_NDC_TO_UV` against the header |
| 10 | `sector_count` / `pass_bandwidth_us` / `bytes_in_flight` / `halo_overhead` / `halo_border_ratio` / `occupancy_vgprs` (§16) | `vulkan-opt` 09 §4 / §4.4, 03 §6; the skill's halo *prose* (border/fetched) and *table* (border/interior) disagree, so both are shipped with distinct names | `frame_program_report` writes a floor and a bottleneck, not a vibe | formulas must use `double`, not integer division | hand-checked against `validate_math.py`: 91.5 µs, 56.2/26.6/12.9 % at 8/16/32 tiles; `occupancy_vgprs(32,32) = 98,304/1,024 = 96` is enforced by a `_Static_assert` |
| 11 | `Bottleneck` / `frame_bottleneck()` (§18) | `shader-opt` 13 §3 P3 triage: a unit near its own peak (bandwidth) vs no unit near peak (latency) have opposite fixes | prevents the classic "optimize the ALU in a bandwidth-bound pass" mistake | `top_sol_pct` needs real counter data | returns `NONE` without data |
| 12 | `ShaderSource` / `shader_compile_in_process()` (§17), `MAX_SPEC_CONSTANTS` (§1) | `todo.txt` in-process compiler; `shader-opt` 10 §3: a specialization constant leaves a real branch and is folded at pipeline creation — a **pipeline-count** decision, not an instruction-count one | compilation stays off the frame path; variants named by capability through `PipeKey.*_entry` | a compile inside `frame_run` increments `pipeline_creations` and fails the budget | C11 compile; no per-frame spec-constant setter exists |
| 13 | `scripts/section_index.py`, `scripts/layer_check.py` | the header's opening comment claimed both scripts parsed it and neither existed | the "sections are appended, not reordered" and "no slow verbs" rules are now executable | regex-based; a genuinely new C construct may need the parser extended | both pass; negative control catches injected `queue_wait_idle` / `create_graphics_pipeline` |

### The no-slow-verb ledger

The header's public declaration list is checked against the ban list by `layer_check.py`.
`vk_scan.py src` returns **0** findings. The corresponding mechanisms:

| Banned | Mechanism | Enforcement |
|---|---|---|
| device/queue idle waits | serialises CPU and GPU (`vulkan-opt` §5.9) | `FrameStats.queue_waits` must be 0; no verb declared |
| `ALL_COMMANDS` / memory-read-write scopes | forces flush+invalidate wider than the dependency (`vulkan-opt` 02) | `Access2` has no all-commands / read-write value |
| `GENERAL` on a fully readable attachment | disables DCC/compression (`vulkan-opt` 02 §2.2) | `AttUse` derives layout; `ATT_STORAGE_READ` added |
| `LOAD_OP_LOAD` on a fully overwritten attachment | needless decompress (`vulkan-opt` 02 §3) | `ATT_CLEAR`/`ATT_DISCARD`/`ATT_WRITE` derive store/load |
| full-res resolve consumed half-res | 4× the bytes (`vulkan-opt` 03) | `PASS_BLIT` + `pass_bandwidth_us` floor |
| per-frame descriptor writes / pipeline creation | compile can land mid-frame | `FrameStats` zeros + `frame_stats_within_budget` |
| vertex/index buffer binding | pipeline explosion + VGT state churn | `GeometryRef` / `pass_geometry` (§19) |

---

## 4. Deltas (contract item 4)

**Static measurements actually run** (repeatable from the repo root):

| Measurement | Before | After | Δ |
|---|---:|---:|---:|
| `gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only` TU including `vk_v2.h` | clean (prior round) | clean | 0 |
| `gcc -std=c2x -Wall -Wextra -Werror -fsyntax-only` | not run | clean | — |
| `vk_scan.py src` findings | 0 | 0 | 0 |
| `shader_scan.py shaders/mu` findings | 67 (3 medium, 3 low, 61 info) | 54 (all `info`) | −13 |
| section banners | 15 | 19 (1–19 contiguous) | +4 appended |
| cross-file constants verified | 0 (no checker) | 12 constants + 13 bpp + `MU_NDC_TO_UV` | +26 |
| `probe.slang` SPIR-V body ops | 6861 | 7076 | +215 (new modules/helpers, **not** a regression claim) |
| `probe.slang` control-flow ops | 66 | 66 | 0 (SDF marchers only) |
| `probe.slang` compiler warnings | 0 | 0 | 0 |
| `validate_math.py` | passes | passes (6 new checks) | 0 failures |

The +215 body ops are the cost of *adding* `dispatch.slang`, `wave.slang`, `fp16.slang`,
`format.slang` and their probe coverage; they are not an optimisation regression. The
A/B deltas below are measured on isolated entry points, not on the library total.

### Measured A/B deltas, isolated (`shaders/mu/tests/measure.py`, runtime inputs)

| Change | Before | After | Δ | Mechanism |
|---|---:|---:|---:|---|
| `x*y+z` → `mu_mad` | 2 | 1 | −1 | `OpFMul+OpFAdd` → 1 `OpExtInst Fma` (one `v_fma_f32` on RADV either way) |
| `sign` → `mu_sign_nz` | 3 | 2 | −1 | `FSign` + Inf mul/clamp → 1 `v_cndmask_b32` |
| `p/c` → `p*mu_rcp(c)` | 4 | 4 | **0** | 3 lane `v_rcp_f32` → 1 scalar reciprocal; op count hides it |
| `repeat3` → `repeat3_inv` | 14 | 12 | −2 | 3 scalar `FDiv` → 0; caller hoists uniform inverse |
| generic → pow2 tiling | 14 | 13 | −1 | 2 `UDiv` → 1, uniform scalar ALU |
| `WavePrefixSum` → `mu_wave_compact_offset` | 4 | 4 | **0** | ~40 → 2 ISA instructions (`mbcnt_amd`); SPIR-V hides it |
| `mu_atan2_fast` | 19 | 17 | −2 | FMA + scalar-collapse chain |
| `mu_srgb_to_linear_approx` | 6 | 4 | −2 | mad-chain polynomial |

### "No change ⇒ reverted" (mandatory)

| Tried | Measurement | Outcome |
|---|---|---|
| `mu_tonemap_hable` mad rewrite | 13 → **16** net ops (constant white point W=11.2 folds the original but not `Fma`) | **reverted** |
| `mu_env_brdf_approx` / `f_ss` mad rewrite | env 8 → **13** (constant roughness) | **reverted**; scalar collapse in `mu_pbr_direct` kept |
| Bayer bit-shift/or rewrite | 32 → **32** ops, 16/16 values identical | **rejected**, arithmetic Bayer kept |
| `rcp()` as an ISA win | Mesa `ac_nir.c:60` `lower_fdiv=true`, `nir_opt_algebraic.py:1917-1918` fdiv→frcp/fmul, ACO `aco_select_nir_alu.cpp:2367` `emit_rcp` | **not claimed**; `mu_rcp` kept only for hoisting vector→scalar and for intent/portability |
| `RelaxedPrecision` / global `mediump` | no SPIR-V/ISA difference on RADV | **not shipped** |
| fp16 throughput claim | explicit `half2` emits `OpTypeFloat 16` + `OpFConvert`; no `v_pk_*` | **not claimed**; scalar half buys registers, not throughput |
| `mu_load_u32_le` pointer form | adds `PhysicalStorageBufferAddresses`, `Int64`, `OpConvertUToPtr` | **replaced** with `ByteAddressBuffer` overload |
| 48-VGPR "full occupancy" | corrected to 96 VGPRs/thread for GFX12 wave32 | **removed** from every claim |
| `v_perm_b32` packing claim / exact "3 `OpExtInst Pow`" claim | unsupported | **removed** |

---

## 5. Rejected ideas (contract item 5)

| Idea | Why rejected |
|---|---|
| Put specialization constants back as a per-frame knob | `shader-opt` 10 §3: real branch survives the module, folded only at pipeline creation — a pipeline-count decision |
| Default 1024-invocation workgroups | `vulkan-opt` 09 §3A.2: zero occupancy slack; 49 VGPRs or 33 KiB LDS halves waves/CU |
| Bavoil tiling on every full-screen pass | `shader-opt` 13 §2–§3: only for bandwidth-bound wide-kernel gathers with low L2 hit; one-texel-per-pixel and ALU-bound passes lose the arithmetic with no overlap to recover |
| Morton / Z-order remap | `shader-opt` 13 §2: measured gain came from horizontal tiling on a separable filter; Z-order was not the measured lever |
| Claim a frame-time win from the SPIR-V op-count deltas | SPIR-V cannot price registers, wave count, `OpGroupNonUniform*` expansions or final ACO scheduling (`shader-opt` §1.2) |
| Insert new sections into the middle of `vk_v2.h` | breaks the mental map and doc references; `section_index.py` enforces append-only |
| Reorder `Format` / `AttUse` enums to group new values | shifts existing numeric values; new values appended instead |
| Reintroduce vertex/index binding, or add vertex layout to `PipeKey` | `.github_copilot-instructions.md` + `pipeline-explosion.md` M1 |
| "Fix" the Y-flip by editing `vk_cmd_set_viewport_scissor` | root `README.md` says explicitly not to; `NDC_TO_UV_*` and `MU_NDC_TO_UV` agree instead |
| Narrower colour formats / unmeasured depth prepass | `vulkan-opt` hard "do not": needs numerical checks and a measured overdraw ratio |
| Write-combined reads, strided stores, near-CU-LDS `shared` | `vulkan-opt` 09 §3B/§4: sector efficiency and occupancy both regress |

---

## 6. Per-shader contract rows (`vulkan-shader-opt` §9)

Honest limitation: **the RADV ISA was not dumped in this workspace.** "ISA delta" below is the
*front-end* SPIR-V delta from `slangc` + `spirv-dis`; where the actual ISA is known from the
skill's Mesa-grounded references, that is stated separately. Registers are "unmeasured".

```
Shader:      mu/tests/probe.slang [numthreads(MU_WG_64,1,1)] cs, library regression probe
Cost today:  not measured (no GPU timestamps); 7076 SPIR-V body ops, 66 CF ops, 0 warnings
Change:      mu_mad/mu_mads/mu_rcp/mu_sign_nz, scalar-collapse, pow2 tiling, paired-half
             helpers, exact sRGB, 4 new modules, probe coverage for every public helper
ISA delta:   front-end only — see §4 A/B table; RADV ACO listing NOT dumped
Registers:   unmeasured; budget is 96 VGPRs/thread (GFX12 wave32), spills must be 0
Numerics:    unchanged by the FMA/rcp/sign rewrites (validate_math.py: sign substitution
             0 differing / 321602 samples, sRGB round trip 4.441e-16, AgX polynomial
             9.215e-15); changed deliberately where documented (SDF extrapolated last step
             uses MU_SDF_MAX_EXTRAP=8.0 and ONE extra field fetch to verify)
Risk:        front-end op counts can hide register pressure or ACO re-fusing ffma_weak
Verify:      RADV_DEBUG=shaderstats + RGP/RGA disassembly on RX 9070 XT; compare VGPRs,
             waves/SIMD, spills, and pass ms before/after — see §8
```

```
Shader:      SDF marchers (the only 66 control-flow ops in the library)
Cost today:  not measured; step budget recorded per pass via pass_note_samples()
Change:      analytic last step D*Dprev/(Dprev-D), reciprocal repetition
             repeat3_inv, scale-relative epsilon, one-extra-field-fetch verification
ISA delta:   repeat3 14 → 12 SPIR-V body ops, 3 scalar FDiv → 0 (front-end)
Registers:   unmeasured
Numerics:    geometric-tail identity verified q=0.1…0.95; 8× clamp is a deliberate bound
Risk:        an extrapolated hit that is not re-fetched can skip a thin feature
Verify:      A/B the marcher pass with per-pass timestamps AND a golden-image diff
```

```
Shader:      wide-kernel gather (tiling candidate)
Cost today:  not measured; gated on measured L2 hit rate (PassStat.l2_hit_rate)
Change:      mu_tiled_group_id_pow2, N=16 when grid.x divisible by N
ISA delta:   14 → 13 SPIR-V body ops, 2 UDiv → 1 (uniform scalar)
Registers:   unmeasured; 12 integer ops per thread, in a push constant where uniform
Numerics:    exact-divisor bijection verified; 6 non-multiple grids clamp but duplicate
             tiles — compute_shape_error() rejects them
Risk:        applying it to a one-texel-per-pixel or ALU-bound pass adds arithmetic
Verify:      L2 hit rate before/after + pass ms; skill measured -47% and 63→86% L2
```

---

## 7. Numerical / static verification already passing

| Check | Result |
|---|---|
| `python3 shaders/mu/tests/validate_math.py` | all pass; 6 new checks |
| SDF `sign` substitution | 0 differing / 321602 samples |
| Bavoil bijection | exact-divisor grids MATCH; 6 non-multiple grids clamp |
| 1440p RGBA16F read+write | 91.5 µs @ 644.6 GB/s |
| halo (border/interior) 16/32/64 tiles | 56.2 / 26.6 / 12.9 % |
| exact sRGB round trip | 4.441e-16 |
| fullscreen triangle | (0,0),(2,0),(0,2) → clip (−1,1),(3,1),(−1,−3) |
| AgX polynomial identity | worst 9.215e-15 |
| `python3 scripts/section_index.py --check` | 19 sections, 1..19 contiguous |
| `python3 scripts/layer_check.py` | 67 public functions, 57 types, 12 constants, 13 bpp, 19 sections in sync |
| `python3 /home/user/skills/vulkan-opt/scripts/vk_scan.py src` | 0 findings |
| `python3 /home/user/skills/vulkan-shader-opt/scripts/shader_scan.py shaders/mu` | 54 findings, all `info` |

---

## 8. Next measurement (contract item 6)

**Single highest-expected-value experiment:** dump the RADV ISA and register counts for the
three shaders in §6 on the RX 9070 XT, then re-run `measure.py`'s A/B cases against the ACO
listing:

```
RADV_DEBUG=shaderstats ./demo            # VGPR/SGPR, spills, waves/SIMD
MESA_VK_TRACE=ctxroll ./demo             # FrameStats.context_rolls
RGP capture, or rga -s vk-spv <spv>     # v_fma_f32 / v_rcp_f32 / mbcnt_amd / v_pk_*
vkCmdWriteTimestamp2 around each pass    # fills PassStat.gpu_ms, enables frame_bottleneck()
```

This is what would confirm or kill: (a) whether the −1/-2 SPIR-V deltas survive ACO at all,
(b) whether any library shader exceeds 96 VGPRs and loses full occupancy, (c) whether the
post chain is genuinely bandwidth-bound at the 0.55 ms floor, and (d) whether
`mu_wave_compact_offset` delivers the ~40→2 ISA mechanism in a real compaction pass. Until
then, `frame_bottleneck()` returns `BOTTLENECK_NONE`, `ShaderStats.stats_available` may be
false, and **no change in this audit is presented as a measured frame-time win**.
