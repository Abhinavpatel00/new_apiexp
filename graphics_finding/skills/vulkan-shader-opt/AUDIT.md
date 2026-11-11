# AUDIT — is everything we read actually in the skill?

A self-audit of the `vulkan-shader-opt` package, answering one question: **for every source we
went through, where does it land, and is that landing verified?** Written because a skill that
*looks* complete is the easiest way to keep believing bad advice.

Method: a grep checklist over the reference files (one row per claim family), a byte-comparison of
every Mesa code quote against the pinned checkout, and the `slangc` probe harness. All three are
re-runnable — the commands are at the end.

---

## 1. Source → landing matrix

| Source | What we took | Lands in | Status |
|---|---|---|---|
| **Persson 2013/2014** (2 talks) | MAD/FMA, constants, quarter-rate, int divide, `nointerpolation`, front-face, sampler SGPRs, Load/Gather, loop splitting, variants, `med3`, gamma/`snorm`, denorm | `06` (scorecard), `02`, `04`, `08`, `09`, `03` | **integrated** — 15 Persson claim families present |
| **MJP blog** (fp16 2019) | two precision paths, relaxed vs explicit, capabilities, packed math | `11` | **integrated** |
| **MJP blog** (early-Z 2025) | derived-Z state, discard/depth/UAV table, forcing early-Z, FSI/ROV | `12` §1, `05` §2 | **integrated** + proof in `radv_shader.c` |
| **MJP blog** (permutations 2021) | static properties can't branch; spec constants | `12` §3–§4, `03` §4, `10` §3 | **integrated** |
| **MJP blog** (shader compilers 2012) | compile time as a PSO cost | `12` §5 | **integrated** |
| **MJP blog** (spherical normals 2009) | decode cost; superseded by octahedral | `12` §6 + probe | **integrated, measured** |
| **MJP blog** (tone mapping 2010) | ALU-only filmic curve; fp16 range | `12` §7, `08` §5 | **integrated (brief)** |
| **MJP blog** (sample points 2015) | `VK_EXT_sample_locations`; per-sample is static | `12` §7, `05` | **triaged** — D3D/NVAPI mechanics dropped |
| **MJP blog** (10 Things, 2011) | nothing directly; technique wishlist | `12` §9 | **read + judged as not-shader-advice** (see fix log) |
| **Bavoil** (L2 swizzling 2020) | thread-group tiling, applicability test, corrected snippet + measured cost | `13` §2, `09` §1–§3 | **integrated, measured** |
| **Bavoil** (P3 2019) | bottleneck classification, thresholds, SOL bound, hit-rate bands | `13` §3, `SKILL.md` §4 step 1, `vulkan-opt` `01` §1a | **integrated + cross-skill** |
| **Bavoil** (other 11 items) | verdicts only | `13` §1, §4 | **triaged** |
| **Gist "GPU Optimization for GameDev"** (index) | **the whole index, all 5 chunks**: 29 author blocks + every organisation block, each with a S/H/T/X verdict | `14` | **read in full, triaged; 9 of ~30 shader-relevant items still queued** |
| **Lagarde** (inverse trig for GCN, 2014) | FR/QR/VGPR cost of acos/asin/atan/atan2, range reduction, minimax vs Taylor, relative-vs-absolute error, final code | `08` §6 | **integrated, cross-verified against Mesa's own lowering** |
| **Anagnostou** (hidden cost of shader instructions, 2025) | expansion table, latency table, no-FP-scalar-ALU trap, waterfall loop, LDS banking | `08` §6 | **integrated** |
| **Anagnostou** (VS exports, 2025) | TRAM/ISBE budget, 1→10 float4 ≈ 3× cost, unused exports ~free on NVIDIA only | `03` §1 | **integrated** |
| **Wronski** (Is this a branch?, 2021) | if ≡ `?:` ≡ `step`; coherence rule | `05` §1 | **integrated + re-measured with our probe** |
| **Drobot** (fullscreen execution patterns, 2014) | raster order, 87 %/95 %/95 % hit rates, 100 %/108 %/108 % | `09` §2, `15` §2 | **integrated** |
| **Sommefeldt** (context rolls, 2018) | 8 banks / 7 usable, roll cost, batching rule, RGP diagnosis | `vulkan-opt` `03` §8 | **integrated (host side)** |
| **AMD GDC 2019** (blend of GCN opt + colour processing) | separable-filter LDS store-caching: ring-buffer math, occupancy check, workgroup shape | `15` §3 | **integrated** |
| **Anagnostou** (low-level thinking 2023) | a 10-year re-test of Persson on RDNA2/DXC: 17 verdicts, the `pow(x,1)` / `1/sqrt` / `sign()` changes, `[branch]` + implicit LOD, register indexing ladder, cbuffer uniform-vs-per-lane loads | `16` §1–§2, `02` §4, `03` §3, `04` §5, `05` §1, `08` §1a, `09` §2 | **integrated + one claim re-measured in our stack** |
| **Anagnostou** (shader tips and tricks, 53 items, 2022) | grouped by decision and routed to the owning reference; nothing copied without a verdict | `16` §3 | **integrated (digested)** |
| **Anagnostou** (shader occupancy, 2020) | stall happens at the *use*, not the request; occupancy and scheduling are substitutes; cache-thrash and serialised-fetch reasons high occupancy loses | `09` §2 | **integrated** |
| **Anagnostou** (to z-prepass or not, 2020) | what early depth *enables* (alpha-equal, SSAO/SSR/screen-space shadows, Hi-Z, binning, cascades, decals, RT origins), prepass simplification, depth decompression | `vulkan-opt` `03` §3 | **integrated (host side)** |
| **Sawicki** (A Better Way to Scalarize a Shader, 2020 + 2026 update) | the peel loop, the helper-lane hang, and the maximal-reconvergence requirement | `02` §6 | **integrated + verified against RADV and with a probe** |
| **Schreiner** (What's up with my branch on GPU?, 2019) | nothing taken | — | **dead link**; archive-only, conceptual, no measurements |
| **RasterGrid** (Understanding GPU caches, 2021) | per-core cache bytes per resident thread, coalescing-buffer role, spatial-vs-temporal reuse, uniform cache rationale, i-cache size, atomics bypassing L1 | `09` §3, `vulkan-opt` `05` §1 | **integrated** |
| **Ciardi** (Intro to GPU Scalarization, parts 1–2, 2018) | per-cell vs per-light strategies and their trade, the `>=` helper-lane fix, the `~0` vs `WaveBallot(true)` bug, tiled-vs-clustered divergence | `02` §6 | **integrated + probe** |
| **Khronos Vulkan samples** (performance set, 2025 docs) | `VK_EXT_image_compression_control` availability, the safe-layout principle, Arm-vs-AMD mapping | `vulkan-opt` `02` §2.2 | **integrated, Arm numbers declined** |
| **Wihlidal** (Optimizing the Graphics Pipeline with Compute, GDC 2016) | the per-triangle instruction budget, primitive-rate walls, the 78 % culled / 0.06 ms result, HTILE decode, sync-vs-async bandwidth | `vulkan-opt` `04` §3, `09` §4.3 | **integrated, renderer-level** |
| **Chajdas** (An introduction to GPU compute 1–3, 2018) | the issue-port model, `GLC` L1 bypass, CU caches invalidated between dispatches | `09` §2, `vulkan-opt` `05` §1 | **integrated, GCN-era constants flagged** |
| **López** (The Art of Packing Data, 2025) | RGBA8 pack pricing on RDNA3, the `+0.5` rounding trick, `pack_u8` → `v_perm_b32` | `02` §5 | **integrated + probe, intrinsic win does not reach SPIR-V** |
| **Chips and Cheese** (RDNA4 out-of-order memory + dynamic register allocation, 2025) | the wait-counter split; the 192 KB/SIMD register file and 96-VGPR full occupancy; mutable VGPR allocation | `vulkan-opt` `09` §4.5, §2.5; `05` §5; `03` §3 | **integrated + Mesa-verified; one of our own numbers corrected** |
| **Themaister** (Modernizing Granite's mesh rendering, 2024) | the full NV and AMD stage-by-stage ladder, meshlet-format portability, the measured task-shader trap | `vulkan-opt` `04` §4, catalog Part 14 | **integrated, renderer-level** |
| **Goldberg** (The road to 16-bit floats, 2022) | the fp16 A/B `shaderstats` pair, the fp32-constant convert tax, vertex-input and tooling traps | `11` §4, §9 | **integrated, one claim marked obsolete** |
| **Aaltonen** (Large thread groups, 2017) | register-file division, the one-group-per-CU cliff, barrier instruction-mix argument | `03` §3 | **integrated, re-derived for GFX12** |
| **Sabourin** (Harnessing Wave Intrinsics, GPC 2024) | prefix-sum vs prefix-count-bits pricing, waterfall loops, `WaveMatch` availability, saturation arithmetic, histogram collisions | `05` §3, `09` §2, `15` §5 | **integrated + probe, one claim corrected** |
| **Our own measurements** (`slangc` 2026.21, SPIRV-Tools 2025.1) | what `-O3` folds, FMA/half/swizzle costs | `08` §1, `10` §2, `11` §4, `13` §2 | **integrated + harness** |
| **Mesa 27.0.0-devel** (`ac271714…`) | every backend claim | `07` index + inline anchors | **integrated + byte-verified** |

### What changed in the gist pass

| Before | After |
|---|---|
| the gist was known only through its Bavoil block (`13` §1) | all 5 chunks read; every author block has a verdict; the queue is explicit (`14` §6) |
| `atan2` was listed as unverified ("we have not read the ISA") | `08` §6: 1 `OpExtInst` vs ~109 RDNA instructions, with Lagarde's per-function costs and the Mesa lowering that produces them |
| "`?:` is a `cndmask`" was asserted from ACO's ISel only | `05` §1: re-measured at SPIR-V, plus the driver threshold that decides it (`limit = 8`) |
| varyings were a cost with only AMD's LDS figure | `03` §1: NVIDIA's documented half of the budget (ISBE → VPC → TRAM), with the non-portable "unused export is free" caveat |
| effects had no reference at all | `15`: topology, LDS store-caching, sampling order, and an explicit list of what the corpus cannot support |

## 2. Verification status by claim class

| Claim class | How it is backed | Count |
|---|---|---|
| Mesa source behaviour | code quoted verbatim and compared to the checkout, byte for byte | **15 blocks / 74 lines, 0 mismatches** |
| Front-end behaviour ("slangc does/does not fold X") | reproducible probes with expected counts, exit non-zero on drift | **30 expectations / 13 probes, all green** |
| Numbers in the text that come from a third party | attributed to the author and host ("his, RTX 2080, 1440p") | Bavoil 47%/63→86%, MJP culling rates, Persson cycle costs |
| **Not verified here** | listed so it cannot be mistaken for measured | see §3 |

## 3. Explicitly unverified (do not quote as ours)

1. **Persson's cycle costs** (int divide ≈40/48 cycles, transcendentals at ¼ rate) — from his
   slides, not re-measured on GFX12; they are used qualitatively (`02`).
2. **Bavoil's P3 numbers** and the RGP mapping — the method transfers, the counter names do not
   map 1:1; `13` §3.1 marks this.
3. **Third-party host measurements** (his 7900 XT culling rates, his 2080 denoiser pass) — quoted
   as *his*, never as universal.
4. **`atan2` ISA cost** — measured only at SPIR-V level (1 `OpExtInst`); the ISA sequence is not in
   the sparse Mesa checkout, and the text says so (`12` §6).
5. **Coarsening crossover numbers** — deliberately marked *illustrative*, not measured (`09` §5).
6. **Posts triaged from titles/abstracts** — every one is marked in the triage tables; they
   contributed no rule.

## 4. Known gaps (deliberate)

- **The rest of the gist.** Only the Bavoil section was extracted. The gist lists ~30 other authors
  plus AMD / NVIDIA / Intel / Arm / Khronos sections (Filament docs, Rys Sommefeldt's context
  rolls, Drobot's GCN talks, Intel's GRF/wave-width notes, Arm's tile-based guidance). Those are
  *sources for the next pass*, not integrated content; two of them (context rolls, Arm tiling)
  belong to `vulkan-opt`.
- **Paywalled talks** (GDC Vault): known by title only.
- **DXIL/D3D12-only mechanics**: out of scope by instruction, not by omission.
- **`vulkan-opt` reverse links**: the shader skill now points outward in three places
  (`vulkan-opt` `09` §2.2, §3A, §4), and `vulkan-opt` points back in two (`vulkan-opt` `01` §1a, `vulkan-opt` `04` §7). Any
  new frame-level rule found while working here goes to `vulkan-opt`, not into this skill.

## 5. Fix log from this audit

Four real problems were found by the checklist and fixed:

| Problem | Fix |
|---|---|
| `12` §9 claimed "nine of the ten *10 Things* are fixed by Vulkan, and the survivor is per-draw redundancy" — a content claim about a post that had been judged by title, and wrong (the list is Phong/SSAO/DXT5/tone mapping/GI/DoF/flares/fog — technique choices) | §9 rewritten with the real ten items, a residue table, and honest verdicts |
| `13` §3 attributes thresholds to P3 without the source's own wording, and omits the two things that make P3 usable (top-5 clustering, the 90/50 → 1.8× win bound) | §3 now quotes the article's Case 1/2/3 structure, the `>95%`/`<90%` SM-Active% rule, the hit-rate bands, and the win-bound arithmetic |
| Mul+add → FMA/MAC formation was verified this session (`nir_opt_algebraic.py:420-430`, `aco_register_allocation.cpp:3456-3457`, `aco_select_nir_alu.cpp:1989`) but never written down | added to `08` §1 as a table |
| Persson's "sampler state is SGPR data" point was not in the package, and `vulkan-opt` `04` §7 said "no discard to get early-Z", which is stronger than the hardware actually requires | `04` gained the uniform-vs-per-lane descriptor paragraph; `vulkan-opt` `04` §7 corrected and linked to `12` §1 |

## 5q. Fix log from the ACM-survey checklist pass, and the prefetch finding

The survey was queued as "read for gaps, not for integration". Doing that properly meant turning its
28-class taxonomy into a checklist against our own index, and the checklist earned its keep.

1. **One genuine gap: software prefetching** (survey §6.1.9). Checking it against Mesa answers it
   decisively — `grep -rn prefetch src/amd/compiler/` has 17 hits and **none of them is a data
   prefetch**: the only emission is a per-loop instruction-prefetch mode change in the assembler. New
   subsection in `09` §2 prices the instruction form and refuses the data form by name, so a reader
   arriving with CUDA's `prefetch.global.L2` learns immediately that nothing lowers it.
2. **The useful hardware fact it produced:** ACO's loop prefetch retuning is gated
   `gfx_level >= GFX10_3 && gfx_level <= GFX11` (`aco_assembler.cpp:1750-1752`), so **the heuristic
   does not fire on GFX12 at all** — a class of "unrolling made no difference where a GCN-era guide
   predicted a win" now has an explanation. Also recorded: the fetcher's distance is 3 cache lines for
   graphics (`ac_gpu_info.c:1689`), it *faults* rather than clamping at page boundaries so the driver
   pads binaries by `distance × 64` bytes, and GFX12's size cap is 128 = half of the 32 KiB WGP
   instruction cache.
3. **Three written declines with reasons instead of silence**: sparse-matrix formats and CPU/GPU
   splitting (no graphics analogue), inter-block synchronisation (no grid barrier in Vulkan; a
   dispatch boundary costs a context roll), plus warp *specialisation* (no hardware support, nothing in
   RADV assigns work per warp) and auto-tuning (a skill cannot sweep at runtime; the sweep protocol
   transfers, noted in `09` §5).
4. **One class needed a hardware correction rather than a pointer.** Survey 6.3.1 "vectorization" does
   not mean a per-thread SIMD unit on GCN/RDNA — the `v_*` ALU is one scalar op per lane, so
   vectorization is always *packing* (`02` §5). Left explicit in the matrix, because the CUDA reading
   of that word is the wrong one here.
5. Eight new quoted anchors added to `verify_citations.py` so this subsection's claims fail loudly if
   ACO or `ac_binary.c` moves. Suite: **86 citations, 28/28 anchors, 0 problems**;
   `slang_probe.sh` 55/55.

## 5p. Convergence pass: the queue says what it means

No new sources this pass. The work was making the *record* honest, because a triage file that says
"queued" next to eight finished items stops being a work list.

1. **Eight author rows were stale** in `14` §3 (#7 Chajdas, #10 Aaltonen, #16 RasterGrid, #17 Sawicki,
   #19 Ciardi, #25 Arntzen, #26 Wihlidal, #28 Chips and Cheese all carried "queued" despite §6 marking
   them done). Each now names its landing section, so §3 and §6 agree.
2. **Two rows were closed as negatives in place**, rather than left dangling: Schreiner #23 is a dead
   site with archive-only conceptual content (no measurements, no claim worth taking), and Lottes #14
   stays *deliberately partial* — his claims are in `09` via secondary sources and the decks are
   video-only, so recording that is more honest than a fake "done".
3. **The wrong arithmetic from the `09` landing turned up a second time**, in `14` §6 as
   "98,304/2048 = **48**". That is the same GCN-thread-count mistake as `03` §3, and it survived because
   it was written as a *summary of* the corrected section. Now states 98,304 / **1,024** = 96 wave32
   (48 wave64) and says which version was wrong. Lesson worth keeping: fix the derivation *and* every
   restatement of it, because the restatements are what people read.
4. Re-ran the whole suite after the edits: `slang_probe.sh` **55/55** across 20 probes,
   `shader_scan.py` and `vk_scan.py` both still fire on planted anti-patterns, `verify_citations.py`
   **78 / 20 / 0**.

## 5o. Fix log from the RDNA4-registers pass (and a self-correction)

1. **We had a 2× error in a VGPR budget, and a source caught it.** `03` §3 said GFX12 "allows 48
   VGPRs/thread at 16 waves/SIMD" — true only in wave64 terms. The wave32 answer is **96**, and the
   mistake is a unit conversion: dividing the wave64 register file by the wave32 slot count. Mesa does
   it correctly in `radv_get_max_waves` (`physical_vgprs = num_physical_wave64_vgprs_per_simd *
   (64 / wave_size)`), and C&C state the silicon figure independently (192 KB per SIMD, 16 thread
   slots, 96 VGPRs at full occupancy). Both now appear in the text with the derivation, along with the
   rule "always say which wave size a VGPR budget is quoted in".
2. **The landing from the previous pass was wrong in the same direction and is fixed.** My Aaltonen
   comparison claimed "same 2,048 threads, budget 48 → 1.5× larger". Corrected: a GCN CU held 2,048
   threads at 32 VGPRs; a GFX12 CU holds **1,024 threads at 96** — half the threads, three times the
   budget. The method survived, the conclusion did not, and it is written down rather than quietly
   amended.
3. **Dynamic VGPR allocation was integrated as a negative**, which is the useful form: `s_alloc_vgpr`,
   `SQ_DYN_VGPR`, blocks of 16/32, max eight blocks (>128 VGPRs forces coarse granularity), SCC-checked
   requests with busy-wait, occupancy set by register rather than inferred — and then the fact that
   **Mesa contains no `s_alloc_vgpr` at all**, so any report crediting this feature was not running on
   RADV. C&C's Quake 2 RTX capture (9 of 16 slots) gives the failure mode a concrete image.
4. Two general lessons were promoted out of the raytracing framing so they apply to any shader: the
   register budget is driver policy rather than a constant, and the VGPR count that governs occupancy is
   the maximum over the whole shader including cold paths.

## 5n. Fix log from the Khronos-samples pass

1. **A whole class of advice was ruled out for our hardware before it could be written.** The Khronos
   *Image compression control* sample teaches you to request fixed-rate compression; RADV gates both
   that extension and its swapchain companion on `gfx_level < GFX12` with the comment "Not useful on
   GFX12" (`radv_physical_device.c:241`). So the sample's actionable content on a Navi 48 is *nothing* —
   which is worth recording, because a plausible-sounding recommendation that silently does not apply is
   worse than a missing one.
2. **The Arm numbers were refused rather than adapted.** AFRC's 65 % footprint / 38 % bandwidth /
   49.8 dB PSNR and transaction elimination's ~10 % write-bandwidth recovery are Mali mechanisms; the
   section keeps them labelled and maps only the *rule* onto RADV's DCC/HTILE logic, which already
   exists in `02` §2.2 and §2.3.
3. **The transferable part is a table, not a paragraph**: four Khronos/Arm rules against their AMD
   analogues, including the two that are genuinely the same advice (don't assert `UNDEFINED`; prefer
   don't-care load/store over a layout round trip).
4. This closes the last "source of numbers we could re-measure" lead in `14` §10 with the honest
   outcome: it is a mobile-first sample set, and on a desktop RADV target only the principles survive.

## 5m. Fix log from the Frostbite-culling pass (Wihlidal, GDC 2016)

1. **The culling section now starts from a budget instead of a technique list.** Deriving 384
   instructions per triangle (Xbox One: 12 CU × 64 ALU × 2 FLOPs ÷ 2 engines ÷ 2 ALU/cycle) turns
   "is per-primitive culling worth it?" into arithmetic, and it explains why the cheap tests are the
   ones that exist: winding by 2D cross-product sign, bbox-vs-Hi-Z, screen-extent small-prim. PS4 lands
   at 508 and a Fury X at 1,024, so the same shader is over budget on one machine and comfortable on
   another.
2. **A rasterizer fact that contradicts a common assumption is now stated**: the practical primitive
   rate is ~0.9/cycle against a theoretical 2, a VS result must return within 4,096 cycles, and
   triangles whose bounding rectangle exceeds 32×32 take multiple coarse-raster cycles. Big triangles
   are cheap to shade and expensive to *set up* — which is a separate axis from overdraw.
3. **The async-compute trade got a number, and it points the other way from fashion.** The same
   instancing path measured **1.5 bytes/triangle** (sync, near-100 % L2 hit, ALU-bound, 24 VGPRs,
   occupancy 8) versus **20 bytes/triangle** (async, low L2 residency, VMEM-bound). A 13× traffic
   difference from scheduling alone. That is now in `04` §3 and cited from `09` §4.3, next to the L2
   locality argument it belongs to.
4. **Two implementation details were kept because they are not reproduced anywhere else in the corpus**:
   shader-side `HTILE` decode (one test rejects 64 pixels; the 14+6-bit stencil format and the 2×14-bit
   min/max format) and software-Z priming of the Hi-Z pyramid so occlusion culling need not wait for a
   depth prepass.

## 5l. Fix log from the coverage pass (closing the index)

1. **The two "unread chunks" queue items were phantom work.** The GPUOpen context-rolls article is
   complete in chunk 0 and its chunks 1–2 are boilerplate; the gist's HTML chunks 5–8 are comment
   threads, while the raw markdown is 5 chunks. Both are now recorded as closed in `14` §10 with the
   recipe (read raw, read to the end once) so a future pass does not pay for them again.
2. **The tail of the index produced five leads and four declines, not content.** Most usefully it
   confirmed what this corpus *is*: an index of talks and architecture decks, with almost no
   effect-level material. The declines are written down with reasons (Naughty Dog = abstracts only,
   SER = D3D12/NV-only, tools = "run this tool" is not advice), because a list that only ever grows is
   not a triage.
3. One genuinely new `[third-party]`-free lead is Khronos's own **Vulkan performance samples** — the
   only remaining source in the index whose numbers we could re-measure on our own part, which makes it
   the highest-value item left in the queue.

## 5k. Fix log from the compute-series pass (Chajdas, 2018)

1. **The cost model gained its missing half.** Every section before this one explained *latency*; the
   issue-port model explains *throughput* — six ports, one issue per cycle, VALU occupying the SIMD for
   several cycles — and that is why instruction **mix** is a lever and not a style preference. It also
   gives the mechanism behind advice this skill already had in storage form ("keep uniform data in
   SGPRs"): scalar and vector work use different ports and can overlap.
2. **The GCN constants are quarantined.** "Six ports, five issueable per cycle, VALU 4 cycles" is dated
   in the text, with the instruction to read issue/stall counters on current hardware and keep only the
   structure. This is the fourth landing in a row where the *method* transferred and the number did not
   (Aaltonen's 32 VGPRs, Ciardi's 10 waves, Schröder's page costs).
3. **Two of the article's cache facts closed open loops in `vulkan-opt`.** The `GLC` bit ("force bypass
   of L1") explains *why* the pinned AMD compiler has no `Coherent`-decoration handling — coherence is
   a per-access choice at the instruction level, not a cache mode — and "CU-local caches are
   invalidated between dispatches" is the reason there is no cross-dispatch L1 reuse to optimise, which
   is a rule this skill asks for elsewhere without having justified.

## 5j. Fix log from the packing pass (López, 2025)

1. **The headline number is real but not ours.** `pack_u8` cutting the RGBA8 pack from 14 to 11 ISA
   instructions is a DXC + LLPC + RGA result on gfx1103; the skill keeps it, labelled with the tool
   and the ASIC, and states López's own caveat that older RGA produced *worse than baseline*.
2. **Measured here, the intrinsic does nothing**: manual chain and `pack_u8` emit identical SPIR-V
   (6 shifts, 6 ORs, no bitfield op), because Slang expands it at the front end. That is now a probe
   (`pack-probe.slang`, 4 assertions) rather than a paragraph of theory, and it is the third
   independent case in this skill of *the IR cannot see the optimisation you are paying for*.
3. **The reason the RADV path exists at all is documented**, so the advice is not "give up": RADV sets
   `has_pack_32_4x8` (`ac_nir.c:102`) and ACO lowers `pack_32_4x8` to a *copy*
   (`aco_select_nir_alu.cpp:3282`) — the win is available if the components are 8-bit throughout, and
   unavailable if they were always 32-bit `uint`s, because nothing pattern-matches a shift/or chain
   back into `v_perm_b32`.
4. **The `+0.5` rounding idiom was separated from the intrinsic question**, which is how it should
   have been all along: `uint(x * 255.0 + 0.5)` is free on AMD (an inline-literal FMA addend), and in
   SPIR-V it is a separate `OpFAdd` — so that half of the advice is a backend fact, not a source rule.

## 5i. Fix log from the RDNA4-memory pass (Chips and Cheese, 2025)

1. **The skill's oldest piece of advice — "a load, then a drain" — was stated too strongly, and the
   source is hardware.** RDNA4 splits `s_waitcnt` into per-category counters, so mixing LDS and scalar
   loads no longer forces `lgkmcnt(0)`. `05` §5 and `vulkan-opt` `09` §4.5 now say which mixes are
   free and which are not, instead of repeating a rule that was true on GCN through RDNA3.
2. **The claim was verified in our own compiler before being written down**, which is what makes it
   safe to act on: `wait_imm::max()` (`aco_ir.cpp:1476`, counters at `:1483-1485`) creates the sample,
   BVH and `km` counters **only** at GFX12, and the opcode list at `:1610-1628` matches the article's
   description. Two ACO details the article omits were added because they change codegen: fused
   `s_wait_loadcnt_dscnt` / `s_wait_storecnt_dscnt` pairs, and the fact that scalar loads are the
   out-of-order category, so one dependent `s_load` drains *all* pending scalar loads.
3. **The framework's perspective was kept, not clipped.** C&C note NVIDIA has done this since Turing
   and Intel since Gen9 — so the section ends by telling the reader *not* to restructure around it,
   which is the difference between a fact and a bandwagon.
4. The *dynamic register allocation* post was left in the queue rather than half-integrated: it is
   raytracing-shaped and its published numbers show the feature being forced and *losing*, which needs
   its own read before it can be stated.

## 5h. Fix log from the GPU-driven-mesh pass (Themaister, 2024)

1. **The headline claim of the genre — "mesh shaders are faster" — is not what the measurements say,
   so the section states what they do say.** Without per-primitive culling a mesh shader (4.0–4.1 ms)
   ties with plain MDI (3.9 ms) on NVIDIA; the win comes from culling primitives (3.3), rejecting
   micro-polygons (1.9) and not paying for statistics (1.65). Both tables are reproduced, because the
   AMD column *contradicts* the NVIDIA one on MDI (4.0 → 5.3 ms, a loss).
2. **A vendor claim that we are likely to inherit was corrected in advance:** the 2N/N meshlet shape
   recommended by GPUOpen could not be reproduced (AMD lost ~30 %); 32-wide sublets inside 256-wide
   meshlets is what both vendors accepted. The section keeps both facts so a future reader does not
   have to pick a side blind.
3. **Task shaders are recorded as a measured trap, not a design option**: ~10× slowdown on NVIDIA at
   the hierarchical shape, a payload cliff (<108 B), and on AMD they degenerate into small indirect
   dispatches. The portable answer is `vkCmdDrawMeshTasksIndirectCountEXT` plus counter bucketing, and
   the bucketing trick is spelled out because the 64K workgroup limit is otherwise a silent wall.
4. **Instrumentation cost is now a documented number** (1.9 → 1.65 ms from removing one atomic per
   workgroup), because every capture in this skill's workflow adds exactly that kind of counter.
5. Four rows were added to `OPTIMIZATION-CATALOG.md` as **Part 14**, since the geometry budget had no
   entry there at all — the catalog previously stopped at shading and post.

## 5g. Fix log from the fp16 and thread-group passes

1. **Goldberg's headline finding is now obsolete and is labelled as such, not deleted.** "RADV
   generated zero `V_PK_ADD_F16`" was true in Jan 2022; ACO now selects `v_pk_mul_f16` /
   `v_pk_add_f16` / `v_pk_fma_f16` for 16-bit defs (`aco_select_nir_alu.cpp:1884`, `:1912`, `:1930`,
   `:1986`). What survives is the *reason* his fp16 build lost on every axis, and it is hardware:
   there is no scalar float16 convert, so fp32 UBO constants cost twelve `v_cvt_f16_f32_e32` per loop
   iteration and push data that was free in SGPRs into VGPRs. That single mechanism explains 40 vs 32
   VGPRs and 24 vs 32 waves/SIMD, and it is what makes "make the storage 16-bit, not just the math"
   the actionable rule.
2. **A VGPR reduction is not a result** is now stated with a counterexample: the proprietary Windows
   driver *did* emit packed math (VGPR down, SGPR up) and measured no performance change, with
   `AMD_shader_info` showing +6 % cycles and +96 % stalls.
3. **Aaltonen's arithmetic was re-derived for GFX12 rather than copied.** 65,536/2048 = 32 VGPRs in
   2017 becomes 98,304/2048 = **48** today, and the two derivations are cross-checked against each
   other (16 waves/SIMD × 64 lanes × 2 SIMDs = 2048 threads, 768/16 = 48). The method is presented as
   the transferable thing; the constant is explicitly dated.
4. `11` §9 gained two operational traps that are not arithmetic at all — 16-bit vertex attributes do
   not autoconvert, and fp16 debugging was (and partly still is) the weak link. Both were found by
   shipping, not by reasoning.

## 5f. Fix log from the wave-intrinsics pass (Sabourin, GPC 2024)

1. **One published claim did not survive our stack and is recorded as such.** Sabourin warns that
   dynamic `WaveReadLaneAt` — and even SPIR-V shuffle-up — compiles to a waterfall loop. That is true
   on AMD's offline compiler/RGA, which is what he used. On RADV it is not: `.lower_shuffle` is off,
   and ACO's shuffle case (`aco_select_nir_intrinsics.cpp:4170`) lowers a fully divergent index to
   `ds_bpermute_b32` in one instruction. The section states the mechanism on both sides rather than
   repeating the warning, because the warning would cause a real, unnecessary refactor.
2. **`WaveMatch` does not exist in Slang-on-SPIR-V**, measured rather than assumed: `error[E36107]`
   in both fragment and compute stages. So the histogram fast path is a hand-written ballot loop, and
   the section says how many ballots that costs (`bitcount(bucketCount)`).
3. **SPIR-V op counts are now explicitly ruled out as a pricing method** for subgroup code, with the
   measured example (prefix-sum and prefix-count-bits are 1–2 IR ops; 40 vs 2 in ISA).
4. The pricing fact that *did* reproduce got a Mesa anchor: bit-count → `nir_intrinsic_mbcnt_amd` →
   `v_mbcnt_lo_u32_b32`, i.e. the "2 instructions" is a hardware popcount-prefix.
5. **The same author's blog supplied the renderer-level half and was routed away from this skill.**
   *A Tale Of Too Many Blurs* 3 is a halo-free sliding-window blur with a thread-count identity
   (`total = texture_height × group_width`) and a measured **sign flip** between a 256×2 and a 16×16
   group — the strongest published evidence we have that dispatch shape and shader cannot be tuned
   separately. It belongs to `vulkan-opt` `09` §3A.3b, next to the halo table it beats, and part 1's
   Morton thread-index result went to the same file's §4.2. This is the routing rule of §1 of this
   document in practice: the wave-intrinsic insight stays here, the pass/dispatch consequence leaves.
6. `09` §2 gained the saturation multiplication (26 WGP × 4 × 20 × 32 = 66,560; our part 65,536), and
   `15` §5 gained the auto-exposure histogram row — the first effect-level entry traced back to a
   subgroup measurement rather than a pass-topology argument.

## 5e. Fix log from the scalarisation-tutorial pass

1. `02` §6 now carries **both** published strategies with the trade between them (per-cell
   duplicates work; per-light pays for a cross-lane minimum; the fast path skips it when a wave is
   coherent), instead of only the generic peel loop.
2. **Two published bugs are recorded as bugs.** The `==` deadlock on helper lanes has a
   one-character fix (`>=`) and also explains the `WaveActiveMin → -1` edge case; and the
   `laneMask == ~0` fast-path test was corrected by a reader in 2020 and acknowledged by the author —
   it must compare against `WaveBallot(true)`. A skill that copies these snippets without the
   corrections would ship GPU hangs.
3. The two independent helper-lane failures (Sawicki 2020 crash, Ciardi part 2 note 2) are now
   presented as one rule with two fixes, because they compose:
   `WaveActiveAnyTrue` guard **and** `>=`.
4. `scalarize-min-probe.slang` added so the per-light spelling is verified in our stack rather than
   transcribed from HLSL.

## 5d. Fix log from the cache-hierarchy pass

1. `09` §3 previously asserted lane/sector locality without saying where the numbers come from. It
   now carries the bytes-of-L1-per-thread table (GCN 6.4, RDNA 12.8), which is the argument that
   makes "the L1 is a coalescing buffer, not a cache" a conclusion rather than a slogan, and it
   explains the measured ~75 % hit rate of ordinary bilinear texture mapping.
2. The **uniform cache being a physically separate cache** is the missing half of `04` §5's
   uniform-vs-per-lane rule. The skill previously justified it entirely by instruction selection;
   the reason a uniform load is cheap is that it goes to a different cache that streaming data
   cannot evict.
3. `vulkan-opt` `05` §1 gained the atomics/coherence paragraph, including the verified negative that
   the pinned AMD compiler has no `Coherent`-decoration handling, so the barrier is the mechanism.
   That kills a common wrong shortcut ("I used an atomic, so it's synchronised") with a mechanism
   rather than a rule.

## 5c. Fix log from the occupancy / prepass / scalarisation pass

1. **A correctness bug in a widely-copied idiom is now in the skill.** The peel/waterfall loop cannot
   be relied on in a Vulkan pixel shader without `MaximallyReconvergesKHR` or explicit subgroup
   barriers, and the `break` form hangs on helper lanes. Our probe proves the module carries neither
   the barrier nor the execution mode, and RADV's support for `VK_KHR_shader_maximal_reconvergence`
   is cited from `radv_physical_device.c:787` / `:1513-1514` and `shader_info.h:329`. This is the
   first item in the skill that is a *hang*, not a slowdown.
2. `vulkan-opt` `03` §3 had a cost model for a prepass but no value model. The 2020 post supplied
   one: seven things early depth enables, which is why "we're deferred" is only half an answer.
3. `09` §2 now states the stall-at-use mechanism explicitly, and notes that occupancy and
   load-to-use distance are *substitutes* — which is the missing premise behind ref `16`'s
   "fewer VGPRs was slower" counter-example.
4. One queue item closed as **dead link** rather than silently dropped, with the archive URL and the
   reason no claim was taken (conceptual, no measurements).

## 5b. Fix log from the 2023 re-test pass

1. **The operand-grouping claim was re-measured, and the number changed.** DXIL reports 10 → 6
   instructions; Slang→SPIR-V measures 5 → 4 ops for the same two spellings. Both are right: an
   `OpVectorTimesScalar` is one op and three ISA instructions. The rule written into `02` §4 is
   therefore the *mechanism* ("collapse the scalars before the first vector multiply") with our own
   counts attached, not the DXIL number.
2. Three Persson-era claims were found **obsolete** rather than merely aged: `1/sqrt(x)` is now one
   instruction (2014 said "reciprocal plus sqrt"), `pow(x,1)` still round-trips through exp/log, and
   `[branch]` around an implicit-LOD `Sample()` cannot be a branch at all because derivatives need
   the quad. All three are now in the skill with the correction, not the original advice.
3. Added **four** pieces of `16` to existing references instead of leaving them in a new file:
   operand grouping (`02` §4), the register-indexing ladder (`03` §3), scalar-vs-vector buffer loads
   plus `GetDimensions` (`04` §5), load-to-use reordering and the "fewer VGPRs was slower"
   counter-example (`09` §2).
4. `06` now states explicitly that `16` supersedes it for anything measured after 2023 — a scorecard
   that claims to be final is the failure mode this whole audit exists to avoid.

## 5a. Fix log from the gist pass

1. `08` §6 is new and replaced the "inverse trig: unverified" entry in §3 — the gap is closed with
   a measurement and with Mesa's own implementation, not with a citation to the article that
   motivated it.
2. The `atan2-call` scanner rule pointed at `12` §6 (normal packing) for an inverse-trig problem;
   retargeted to `08` §6 and given the ISA number. Two rules added: `acos-asin-call` (Mesa already
   handles these — do not hand-roll) and `step-mask` (`step()` is not cheaper than an `if`).
3. `scripts/verify_citations.py` is new: it bounds-checks **every** `file.c:NNN` citation in the
   skill against the pinned checkout and byte-matches a curated table of 20 anchors. Previously the
   Mesa quotes were verified by hand once, which is not a repeatable claim.
4. `09` §2's "raster order" point was initially inserted as a third numbered item after a bullet
   list; promoted to its own heading.

## 6. Re-running this audit

```bash
cd skills/vulkan-shader-opt

# 1. claim-family checklist (greps; add a row when you add a claim family)
python3 - <<'EOF'
import glob, os
texts={os.path.basename(f)[:2]: open(f).read() for f in glob.glob('references/*.md')}
checks=[("MAD/no-FMA",["mad("],["08"]),("fp16 packed",["v_pk_add_f16"],["11"]),
        ("early-Z",["writes_memory"],["12"]),("swizzle",["swizzl"],["13"])]
for name,needles,files in checks:
    scope=' '.join(texts.get(k,'') for k in files)
    print(('ok  ' if any(n in scope for n in needles) else 'GAP ')+name)
EOF

# 2. every Mesa code quote, byte for byte
#    (harness: fenced block + "*(path:line)*" caption, compared with the pinned checkout)

# 3. every measured front-end claim  (55 assertions, 20 probes)
SLANGC=/path/to/slangc SPIRV_DIS=/path/to/spirv-dis bash scripts/slang_probe.sh

# 4. every Mesa citation resolves, and 20 quoted anchors still match
python3 scripts/verify_citations.py

# 5. the corpus index still matches the references
python3 - <<'EOF'
import glob, os
refs = {os.path.basename(f)[:2] for f in glob.glob('references/*.md')}
need = {'01','02','03','04','05','06','07','08','09','10','11','12','13','14','15'}
print('missing references:', sorted(need - refs) or 'none')
EOF
```
