# vulkan-shader-opt

A shader-level optimisation skill for Vulkan renderers: what to change in GLSL/HLSL/SPIR-V so
that the code the GPU runs is cheaper — and how to prove it in the ISA.

Companion to **`vulkan-opt`** (frame/pass/bandwidth/occupancy). Use `vulkan-opt` first: this
skill assumes the pass structure is already justified and a specific shader is the cost.

Grounded in **Emil Persson's GDC talks** — *Low-level Thinking in High-level Shading Languages*
(2013) and *Low-level Shader Optimization for Next-Gen and DX11* (2014, 52 slides) — and
re-verified for 2026 hardware, with every claim that concerns compiler or hardware behaviour
anchored in **Mesa NIR/ACO** source (27.0.0-devel, commit
`ac271714ff58086430a3817554a4b9ac0e5929d9`) or in **measured `slangc` 2026.21 output**
(`-target spirv -O3`, disassembled with SPIRV-Tools v2025.1) — or explicitly marked *unverified*.

The verification toolchain is part of the skill: `references/10` is a repeatable recipe for
compiling a Slang shader to SPIR-V and reading back exactly which optimizations the front end
performed and which it left to the driver.

## Contents

| File | What it is |
|---|---|
| `SKILL.md` | triggers, cost model, workflow, decision tables, do-not list, ROI ranking, output contract |
| `AUDIT.md` | self-audit: every source → where it lands, what is verified vs unverified, known gaps, and how to re-run the checks |
| `references/01-cost-model-and-measurement.md` | why instruction count is not time; how to dump ISA per vendor; measurement pitfalls |
| `references/02-alu-precision-and-packing.md` | MAD/FMA, transcendentals, `saturate`/`sign`, precision, fp16 packing, integer math |
| `references/03-interpolants-registers-variants.md` | varyings and LDS, vertex input, register pressure, specialization constants |
| `references/04-texturing-derivatives-formats.md` | fetch vs gather vs sample, cube maps, derivatives, format choices |
| `references/05-control-flow-and-wave-ops.md` | uniform vs divergent branches, discard/demote/quad control, subgroups, loops |
| `references/06-persson-scorecard.md` | every claim from the two talks: holds / changed / obsolete, with the 2026 form |
| `references/07-mesa-shader-code-index.md` | where NIR and ACO live, the verified anchors, debug switches |
| `references/08-mathematical-optimization.md` | algebraic identities, strength reduction, error budgets — plus the measured table of what slangc and RADV fold and what they do not |
| `references/09-locality-and-scheduling.md` | register locality, dependency chains, lane/sector/bank locality, thread coarsening, temporal locality |
| `references/10-slang-toolchain-workflow.md` | the slangc command lines, `-O3` behaviour measured, `-dump-ir`, Slang features and their SPIR-V consequence, A/B recipe |
| `references/11-fp16-and-16-bit-precision.md` | fp16 for Vulkan: relaxed vs explicit precision, capabilities, packed-math reality check, scorecard of the 2019 MJP article |
| `references/12-therealmjp-audit.md` | audit of the therealmjp blog: early-Z as driver-derived state, static-property permutations, compile time, normal packing (measured), and a triage of every post |
| `references/13-bavoil-l2-locality-and-p3.md` | Bavoil: thread-group ID swizzling for L2 locality (Slang version + measured cost), and the P3 bottleneck-classification method mapped onto RGP |
| `references/14-gist-corpus-triage.md` | the *GPU Optimization for GameDev* gist read in full: every author block with a verdict, what landed where, what is queued, what is out of scope, and what it does not cover |
| `references/15-effects-pass-topology.md` | effect passes: fullscreen topology measured (2-tri 87% vs 1-tri/compute 95% L1/L2 hit), LDS store-caching for separable filters, and sampling/execution order |
| `references/16-lowlevel-2023-retest.md` | the 2023 re-test of Persson on RDNA2: which 2013/2014 claims still hold, what `pow(x,1)`, `1/sqrt` and `sign()` really cost now, `[branch]` around an implicit-LOD sample, and 53 tips digested |
| `assets/shader-review-checklist.md` | one-page review checklist |
| `assets/slang-probes/` | thirteen small Slang shaders, one question each, behind the measured tables in `08`, `10` and `11` |
| `scripts/slang_probe.sh` | re-runs the probes against your `slangc`/`spirv-dis` and fails if a documented claim no longer holds |
| `scripts/shader_scan.py` | static scan (35 rules) of shader sources for the anti-patterns in `02`–`05` and `08`–`09` |
| `scripts/verify_citations.py` | bounds-checks every `file.c:NNN` citation against the pinned Mesa checkout and byte-matches 20 quoted anchors |
| `/home/user/vulkan-shader-opt-bundle.md` | all of the above as one flat file |

## Use

1. Run `python3 scripts/shader_scan.py <shader dir>` to get a triage list.
1b. Compile the shader and read the module:
   `slangc x.slang -target spirv -entry main -stage fragment -O3 -o x.spv && spirv-dis x.spv`
   (`references/10` §1–§2 for the opcode-histogram one-liner).
2. Open `SKILL.md` §4 and follow the workflow: prove the cost, dump the ISA, change one thing.
3. Use `references/06` whenever someone quotes a pre-Vulkan shader trick.
4. Report with the template in `SKILL.md` §9.
5. Re-check the toolchain claims any time: `scripts/slang_probe.sh` (55 assertions; set `SLANGC` and `SPIRV_DIS`), and the Mesa citations with `scripts/verify_citations.py`.

## Scope boundaries

- Not frame-level: use `vulkan-opt` for pass structure, barriers, bandwidth, dispatch geometry.
- No vendor-compiler internals: only what is observable in the ISA, in the SPIR-V module, or
  documented by the vendor.
- Vulkan + Slang/`slangc` only: no DXIL/D3D12/CUDA/Metal paths.
- Precision changes are always reported as numerics changes, never as free wins.

## Relationship to the other documents

- `/home/user/vulkan-opt-bundle.md` — the frame-level skill (10 references).
- `/home/user/gtc-s3466-2012-kepler-optimization-2026-audit.md` — the architecture talk that
  explains *why* the occupancy and memory rules behind `vulkan-opt` `09` exist.
- `/home/user/vulkan-barriers-and-begin-rendering-2026.md` — barrier/render-pass semantics.
