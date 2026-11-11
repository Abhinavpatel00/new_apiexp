# Optimization catalog — "Mesa logic of all these optimizations"

One row per optimization. Columns:

- **Saves** — which budget (`A` passes/sync, `B` bandwidth, `C` invocations, `D` fixed-function,
  `E` occupancy), and roughly what.
- **Mesa logic** — what the driver/hardware actually does (file references in `references/07`).
- **Action** — the API/engine change.
- **Verify** — the counter or measurement that proves it worked.
- **Fails when** — the condition under which it does nothing or hurts.

---

## Part 1 — Pipeline / frame architecture

| Optimization | Saves | Mesa logic | Action | Verify | Fails when |
|---|---|---|---|---|---|
| Remove a whole pass | A, B | Each pass = barrier pair + attachment store/load | delete/merge it | pass timestamps | output is genuinely consumed |
| Merge adjacent passes | A, B | Tile store + reload eliminated | one render pass, local-read layout | barrier count, bandwidth | passes have a real RAW dependency on different layouts |
| Move frame-start compute to a compute queue | A | GFX has a separate compute queue; graphics queue serialises | overlap compute with graphics | queue-timeline gaps | data dependency both ways |

## Part 2 — Clears, load/store, overdraw, prepasses

| Optimization | Saves | Mesa logic | Action | Verify | Fails when |
|---|---|---|---|---|---|
| `LOAD_OP_DONT_CARE` instead of `LOAD` | B (decompress + metadata reset) | no tile fetch exists on RADV/ANV/NVK (immediate-mode); the win is skipping the decompress and letting the driver reset aux/DCC state | set loadOp | clear/pass time, INTEL_DEBUG=perf warnings | contents must be preserved |
| `STORE_OP_DONT_CARE` for transient attachments | ~0 on desktop | RADV/ANV never read `storeOp`; only NVK uses it (`STORE_ZCULL`). Keep it for tile-based hardware and future-proofing, not for frame time | set storeOp | nothing measurable | consumer reads it later |
| `LOAD_OP_CLEAR` in attachment layout | B | fast clear = metadata write if `radv_layout_can_fast_clear` passes | attachment layout + clear loadOp | clear cost ≈ 0 | image shared concurrently / no write usage / wrong layout |
| Compute workgroup 64/128/256, multiple of 64 | E (waves) | wave slots are per-SIMD (16 max); ACO caps concurrent workgroups at 16/CU (`aco_live_var_analysis.cpp`) | change `local_size_x` | waves/SIMD in RGP | LDS per workgroup × waves > 64 KB |
| Replace 1024-invocation workgroups with 256 + LDS staging (or justify the 1024) | E (waves) | resources are allocated per workgroup as a unit: a 1024-invocation group needs ≤48 VGPR and ≤32 KiB LDS to fit twice, else occupancy halves (`09` §3A.2) | retile to 256; keep islands in `shared` | waves/SIMD before/after at equal VGPRs | the algorithm genuinely needs a 32×32 tile or a large island (`09` §3A.3) |
| Compute the halo overhead before choosing a tile size | B, E | border pixels are fetched but not written: 8×8 = 56% overhead, 16×16 = 27%, 32×32 = 13% | pick the tile whose overhead you can afford at your VGPR count | loads/invocation, LDS bytes | neighbourhood is not rectangular |
| Move wave-uniform data to SGPRs (UBO, workgroup ID, push constants) | E (VGPRs) | scalar registers are wave-wide, 1/64th the storage; ACO emits scalar loads for provably uniform addresses (`aco_select_nir_intrinsics.cpp:1503`) | UBO instead of SSBO for per-draw data; drop unused `w` components; `mat4x3` | VGPR count in `RADV_DEBUG=shaderstats` | the value varies per lane |
| Use real booleans and packed 16-bit values | E (VGPRs) | bools are `s1` (one bit per lane, SGPR-side, `aco_ir.h:293`); 16-bit math packs two per VGPR and runs at double rate on GFX9+ (`ac_gpu_info.c:337`) | `bool`, `uint16_t`, `float16_t`, `packHalf2x16` | VGPR count | precision genuinely needs 32 bits |
| Reduce VGPRs to reach the next wave count | E | VGPRs/thread = physical_vgprs / waves; 48 at 16 waves/SIMD on GFX12 (`ac_gpu_info.c:283`) | simplify the shader, prefer 16-bit, cut live ranges | `RADV_DEBUG=shaderstats` VGPR count and spills | already limited by LDS, not VGPRs |
| Shrink LDS below an allocation boundary | E | LDS is rounded up to `ac_shader_get_lds_alloc_granularity()` before dividing the occupancy budget | shrink `shared` arrays, alias them | waves/SIMD change | request is far from a boundary |
| AoS → SoA for the hot stream | F (bytes) | sector granularity is 32 B; one field of a 12–40 B struct wastes 2–8× the sectors | restructure buffers | DRAM bytes vs payload | access already coalesced or L2-resident |
| Per-draw constants in a UBO, not an SSBO | F/E | UBO reads are wave-uniform scalar loads; NVK caps the range at 64 KB (`nvk_physical_device.c:894`) | move the struct, keep it std140 | instructions per wave | data is per-lane or too large |
| `readonly buffer` / `NonWritable` for lookup tables | F | lets loads stay on the read-only path and not be re-issued after a store to another pointer | add the qualifier | memory-instruction count, L1 behaviour | the buffer is written in the same shader |
| Kill the dispatch tail (≥1000 workgroups or a persistent kernel) | E | one extra wave at near-zero occupancy costs a full dispatch latency | grid sizing, `vkCmdDispatchIndirect` | dispatch duration vs workgroup count | the dispatch is genuinely tiny |
| Replace `vkCmdClearAttachments` | A, C | real per-region raster work, not metadata | load ops or `vkCmdClearColorImage` | pass time | tiny scissored clear |
| Remove shader-based clears | C | full-screen invocations + store | hardware clear | PS invocations | — |
| Depth prepass (measured overdraw only) | C | depth test rejects before material | position-only pass, same depth attachment | PS invocations drop in base pass | overdraw < ~1.5, alpha-tested, cheap materials |
| Alpha-tested prepass (partial) | C | alpha test runs twice, but hides material cost | near-geometry only, same test | PS invocations per pixel | material cheaper than the test |
| Front-to-back sorting | C | early-Z rejects more | sort opaque by depth | PS invocations | material sort savings exceed it |
| Reduce quad overdraw | C | 2×2 quad shading | larger triangles / LOD / VRS / alpha prepass | PS invocations ÷ covered pixels | geometry is already large on screen |

## Part 3–4 — Base pass, G-buffer, MSAA, shadows

| Optimization | Saves | Mesa logic | Action | Verify | Fails when |
|---|---|---|---|---|---|
| Fewer G-buffer targets | B | each MRT = write + read × sample count | pack/derive channels | bytes/frame | precision insufficient |
| Narrower G-buffer formats | B | half bytes per channel | `R16G16B16A16` → `R10G10B10A2`/`R8G8B8A8_SNORM` | bytes/frame | format loses compression eligibility |
| No MSAA on deferred G-buffer | B, C | MSAA multiplies every target's bytes | single-sample + post-AA/coverage | bytes/frame | — |
| MSAA in forward pipeline | C (quality) | edges only; resolve is a PS (`ac_create_resolve_ps:28`) | 2x/4x + render-pass resolve | resolve pass cost | resolve at wrong resolution |
| Resolve at consumed resolution | B, C | resolve reads samples × bpp per pixel | half-res resolve for half-res composite | resolve pass time | composite needs full res |
| Coverage/alpha-to-coverage instead of post-AA | A, B | no extra full-res pass | enable A2C, MSAA target | pass count, quality | not supported by the material path |
| Shadow cascades by distance | B, C | shadow pass bytes + invocations | smaller far cascades | shadow pass time | cascades overlap |
| Cull casters per cascade | C, D | fewer vertices/primitives | per-cascade culling / indirect | VS invocations | casters genuinely needed |
| Depth-only shadow passes | B | no colour attachment | depth attachment only | bytes | — |

## Part 5–7 — Lighting, sky, colour

| Optimization | Saves | Mesa logic | Action | Verify | Fails when |
|---|---|---|---|---|---|
| Deferred/clustered lighting | C | light volume overdraw removed | light grid/cluster + compute culling | PS invocations in lighting | few lights |
| Stencil-volume lighting instead of full-screen | C | per-pixel light loop → stencil-marked regions | stencil + pass per light | invocations | many overlapping lights (bandwidth) |
| HDR format choice (`R11G11B10_SFLOAT`) | B | 4 vs 8 bytes/px on every read/write | change format | bytes/frame | alpha or range needed |
| Sky last, depth EQUAL, no write | C | shades only uncovered pixels | reorder | PS invocations | sky contributes fog/lighting |
| Half-res bloom/SSS/SSR | B, C | 4× fewer bytes and invocations | half-res target + depth-aware upsample | pass time | high-frequency content |
| LUT instead of analytic tone curve | A, E | one fetch replaces ALU chain | 3D/2D LUT | PS time | LUT fetch latency dominates |
| sRGB format instead of shader encode | B, C | hardware does conversion on store | `_SRGB` attachment formats | PS ALU/time | blending semantics differ |
| Avoid exposure readback stalls | A | CPU/GPU sync | apply exposure with one frame delay | queue idle gaps | — |

## Part 8–10 — Anti-aliasing and temporal

| Optimization | Saves | Mesa logic | Action | Verify | Fails when |
|---|---|---|---|---|---|
| TAA instead of (or with) MSAA | B, C | history buffer instead of sample storage | velocity + history, full-res | bytes, quality | history half-res/8-bit |
| Velocity as an extra MRT | A, B | avoids a second geometry pass | write velocity in base pass | pass count | shader too heavy |
| Depth-reconstructed velocity for static geometry | B | saves velocity storage/bandwidth | reconstruct from depth + prev view | bytes | deformed geometry |
| Clamp history by neighbourhood + depth | — (quality) | rejection avoids ghosting | variance clamp | visual | too tight → aliasing |
| Narrow resolve at fixed budget | B | fewer taps | tune radius/taps | time + stability | wide is needed for stability |
| Fix particle/foliage velocity | — (quality) | particles have no velocity by default | previous-position output | ghosting | — |

## Part 11–12 — Motion blur

| Optimization | Saves | Mesa logic | Action | Verify | Fails when |
|---|---|---|---|---|---|
| Half-res motion blur | B, C | 4× fewer bytes/invocations | half-res + depth-aware upsample | pass time | thin geometry artefacts |
| Tile max-velocity bound | C | samples proportional to tile motion | tile reduce + per-tile taps | PS invocations | uniformly fast motion |
| Single-direction sampling | C | 1 gather instead of 2 | per-tile dominant direction | taps/pixel | complex motion |
| Skip dilation when static | A, C | early-out by tile | clear/predicate tiles | pass time | — |

## Part 13 — Composite, UI, gamma

| Optimization | Saves | Mesa logic | Action | Verify | Fails when |
|---|---|---|---|---|---|
| Composite in the tonemap pass | A, B | removes a full-screen read-modify-write | merge passes | barrier/pass count | ordering requirements |
| UI into the final pass | A, B | removes swapchain load/store | draw UI in the same pass | bytes | UI needs display-space |
| `LOAD_OP_DONT_CARE` on swapchain first touch | B | avoids a fetch of undefined contents | set loadOp | bytes | preserved window content needed |
| R8 glyph atlas | B | 4× less texture bandwidth | single-channel atlas | sampler bytes | colour glyphs |

## Cross-cutting

| Optimization | Saves | Mesa logic | Action | Verify | Fails when |
|---|---|---|---|---|---|
| Keep attachments compressed (layout hygiene) | B | `radv_layout_dcc_compressed` / `_is_htile_compressed` gate compression per layout | attachment layouts, read-only depth layouts, no feedback loops | bandwidth counters | format not eligible |
| Avoid `GENERAL` | B | DCC/HTILE conditional; feedback loops excluded | proper layouts | decompress counts | driver rescues depth case |
| Batch small draws | D | VGT primgroup packing, `fpovs_per_batch = 63` | merge draws / multi-draw indirect | VGT time, draws/frame | draws already large |
| Reduce state changes per bin | A, D | `context_states_per_bin = 1` on GFX10+ | sort by pipeline/set | binning stats | — |
| Narrow barriers | A, B | over-wide masks → L2 writeback + invalidate | minimal stage/access masks | barrier time, cache ops | real dependency |
| Remove per-frame object creation | A (CPU) | allocation + bind validation | cache views/sets/pipelines | CPU frame time | — |
| Wave-size tuning | E | wave32/64 changes VGPR/LDS per wave | per-stage wave size | waves-in-flight | memory-bound |
| Reduce VGPR spills | B, E | spills = local memory traffic | simplify/split shaders | spill count | never register-bound |
| Subgroup reductions instead of LDS | E | fewer LDS ops + barrier | `subgroupAdd`/`subgroupBallot` | time | not supported |
| Derive paired rays from a half-domain phase, don't re-draw | Samples (`N_eff`) | an antipodal/mirrored partner orbit is generated twice if the free parameter spans the full domain | draw the phase over one fundamental domain, derive the partner (`06` §8) | distinct sample directions per pixel, noise-vs-N must fall √2 per doubling | the pair is meant to be independent (then drop the coupling instead) |
| Make temporal phase sets unique modulo pattern symmetry | Samples (`N_eff`) | `k` phase IDs that differ by the set's own symmetry produce identical sample sets | choose `k` coprime with the symmetry order; verify by hashing the set | hash of the k sample sets shows `k` distinct values | pattern has no symmetry |
| State `N_eff` next to every tap/sample budget | Quality per ms | noise ∝ 1/√N_eff, so half the effective samples is 1.41× noise or 2× the rays | measure unique samples once, then budget honestly | debug pass that counts distinct directions | — |
| Persistently map upload memory and write straight into VRAM | A (CPU/upload) | with ReBAR/UMA RADV exposes `DEVICE_LOCAL|HOST_VISIBLE` (`radv_physical_device.c:544`); RADV's own upload path uses `CPU_ACCESS|GTT_WC` (`radv_cmd_buffer.c:1525`) | `vkMapMemory` once, ring per frame-in-flight, no staging copy | CPU frame time, no upload barrier in the profile | no ReBAR: PCIe-mapped writes lose to a staging copy for large data |
| Never read through the mapped pointer | A | host-visible non-`HOST_CACHED` memory is write-combining: uncached loads, ~100× slower (0.27 GB/s vs 28 GB/s measured) | keep allocator state in CPU-local memory, no asserts/memcmp on the mapping, no GPU-written flags read from it | grep the mapping for loads; check for accidental `memcpy` *from* it | the buffer is genuinely `HOST_CACHED` (then reads are fine) |
| Write full lines, in order | A | write-combining merges adjacent stores only; partial lines and strides smaller than 64 B defeat it | build each record fully, linear `memcpy`, assemble scattered fields in cached scratch first | compare write throughput of a strided vs linear variant | stores already linear and full-line |
| Trace SDF volumes at the mip that matches the pixel cone | Steps × fetch bytes | tracing is fetch-bound; a mip-2 texel covers 4× the footprint of mip-0 at 1/64 the texels | `textureLod` by cone width, break on `D < coneWidth*t`, jump 2 mips at a time (`10` §1) | steps/pixel and fetch bytes per pass | field has no mips, or mips were box-filtered (distances overestimated) |
| Analytic last step for sphere tracing | Steps (and precision) | the trace converges geometrically, so the ε-tail is pure cost; `remaining = D/(1 − D/D₋₁)` is the closed form of the remaining series | keep `dPrev`, extrapolate with clamps, verify the point (`10` §2) | steps/pixel drop, normal noise drops | surface curvature within the last cone / grazing rays (guards in `10` §2.3) |
| Make the hit point precise before blaming the normals | Quality | a per-pixel-varying positional error differentiates into normal speckle via `dFdx/dFdy` | analytic hit, scale-relative ε ≥ f32 ulp of t, tetrahedron gradient at ~1 voxel (`10` §3) | visualize derivative normal vs geometric normal separately | the surface really is discontinuous (silhouette quads) |
| Packed/fp16 math where valid | E | halves register footprint | `mediump`/fp16 | occupancy | precision needed |
| VRS on measured over-shade | C | fewer PS invocations | pipeline/per-draw rate or rate image | PS invocations | rate not applied |
| Cap tessellation output varyings | B | HS output goes to VRAM by design | fewer control points/outputs | tess pass bytes | — |

---

## How to use this catalog

1. Filter to rows whose **Saves** letter matches your bottleneck budget.
2. For each candidate, read the **Mesa logic** column in `references/02` (and the source index
   in `07`) before touching code.
3. Apply the **Verify** measurement *before* the change to establish a baseline.
4. Check **Fails when** against the actual engine — if the condition holds, drop the row.
5. Never apply two rows at once.

## Part 14 — Geometry: GPU-driven mesh paths

| Optimization | Saves | Mesa logic | Action | Verify | Fails when |
|---|---|---|---|---|---|
| Per-meshlet frustum + backface-cone culling before the draw | D | the rasterizer is fed fewer primitives; the cull itself is compute on bounds you already have | cull in compute, emit one indirect draw per surviving meshlet | primitives submitted, GPU time | rejection ratio is low (measured: NV 5.5 → 4.3 ms only from frustum, 3.9 with cone culling) |
| Per-primitive cull in the mesh shader (clip codes + exact 2D area test) | D | fixed-function primitive/varying allocation (ISBE) is the bottleneck once geometry is dense | mesh shader, subgroup-wide cull, `SetMeshOutputCounts` + ballot/mask compaction | prims reaching raster, ISBE vs SM utilisation | you cannot reject conservatively — a false reject is a hole in the image |
| Micro-polygon rejection (bounding box inside one pixel) | D | a triangle with no sample coverage still costs a primitive slot | reproduce the 8-sub-pixel window snap, `floor` the bbox, reject if the corners coincide | prims submitted (measured 31 M → 1.2 M on a pathological scene) | triangles are large on screen — the test never fires |
| `vkCmdDrawMeshTasksIndirectCountEXT` instead of a task/amplification shader | D, A | AMD implements task shaders as a series of small indirect dispatches; NVIDIA penalises large amplification payloads (<108 B preferred, <236 B max) | atomic counter bucketed into 32K-entry `(8, 32768, 1)` dispatches, offset `gl_WorkGroupID` by `gl_DrawIndex` | GPU time (measured task-shader tax ~10 % at best, worse as rejection rises) | you ship on one vendor and have measured the task path locally |
