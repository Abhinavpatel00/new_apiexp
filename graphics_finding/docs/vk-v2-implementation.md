# `vk_v2` backend — implementation record

**Date:** 2026-11-08
**Status:** implemented, compiled, linked, and executed headlessly on Lavapipe.
**Scope:** all 67 public functions declared in `src/vk_v2.h`, plus 6 private
integration hooks in `src/vk_v2_internal.h`.

This document is the companion to `docs/vk-v2-skills-audit.md` (which covers the
header design). It records what the C backend actually does, how to build and run
it, the exact results of the runtime test, which static-scanner findings are
accepted and why, and — importantly — what is **not** proven.

---

## 1. Build and run

The Vulkan headers (`VK_HEADER_VERSION 362`) are vendored in
`external/vulkan/include` (both `vulkan/` and `vk_video/`), so no SDK install is
needed. Volk is vendored at `external/volk/`; it loads the Vulkan runtime dynamically, so binaries link `-ldl` but do not link `libvulkan`.

```sh
cd /home/user/mu_gfx_new
./build.sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./build/smoke
```

`build.sh` compiles with `-std=c11 -Wall -Wextra -Werror -O2
-fno-strict-aliasing`, writes `build/vk_v2.o`, emits `scripts/vk_v2_exports.txt`
from `nm -g --defined-only`, and links `build/smoke`. The export manifest is
consumed by `scripts/layer_check.py` so the checker now proves that every
declared public function is actually defined, not merely declared.

The smoke test must run from the repository root: it loads
`tests/smoke_cs.spv` and `tests/smoke_triangle.spv` by relative path and writes
its reports into `tests/`.

### Naming

There is no `vk2_` / `VK2_` prefix. The module name lives in the **file names**
(`vk_v2.h`, `vk_v2.c`, `vk_v2_internal.h`); identifiers are unprefixed because
the public header already owns a small, closed vocabulary and the 88 backend
helpers are file-local `static`s. Public examples: `gpu_create`,
`frame_program_build`, `pass_dispatch_2d`, `VGPR_FULL_OCC`, `PUSH_MAX`,
`HANDLE_VALID`, `NDC_TO_UV_X`.

Three deliberate exceptions, all for collision/clarity reasons:

| Old | New | Why |
|-----|-----|-----|
| `vk2_cmd`, `vk2_entry` | `pass_cmd`, `pipe_entry` | `cmd` (64 uses) and `entry` (16 uses) are local variables throughout `vk_v2.c`; bare names would shadow them. |
| 22 `vk2_*` enum converters (`vk2_format`, `vk2_samples`, …) | `to_vk_format`, `to_vk_samples`, … | They translate a public enum into a Vulkan value. Bare `format` / `filter` / `samples` collide with struct fields and read ambiguously next to `d->format`. `to_vk_` says what the function does; it is not a namespace prefix. |
| `VK2_VK`, `MU_VK2_ESCAPE` | `VK_CALL`, `MU_ESCAPE` | Bare `VK` is too generic; the escape build flag is `MU_`-namespaced but still carried the version tag. |

`[vk2]` remains as a **diagnostic log/report tag** (a string literal, not an
identifier); it labels stderr messages and the generated `# … report` headers
so they are greppable. The 12 C budget macros are still checked against their
`MU_*` shader-side copies by `scripts/layer_check.py` — only the C-side names
changed, the sync contract did not.

**Note on `build/`:** it is a generated directory. Re-run `./build.sh` after a
fresh checkout; `scripts/vk_v2_exports.txt` is committed so the checker still
works without it.

---

## 2. What is implemented

`src/vk_v2.c` (4021 lines) has 12 internal sections:

| § | Contents |
|---|----------|
| 0 | Fixed capacities, format/bpp/state tables, and the **only** place Vulkan spellings appear (§2 in the file). No runtime allocation of tables. |
| 1 | `struct Gpu` and all slot tables: buffers, textures, targets, samplers, shaders, pipes, frame slots, arenas, staging chunks, delete queue. Generational handles. |
| 2 | Instance / device creation. Vulkan 1.3 core, dynamic rendering, timeline semaphores, descriptor indexing (`partiallyBound`, `runtimeArray`, `variableCount`, non-uniform indexing), `shaderDrawParameters` for Slang's `BaseVertex`, EDS3 entry points loaded through Volk with a static-state fallback, optional validation layer (absent here → a note, not a failure). One bindless descriptor set (bindings 0 sampled images / 1 storage images / 2 samplers / 3 storage buffer) written **once at init**. |
| 3 | Resource creation/destruction. `initial_data` uploads through the real handle. Storage textures get `VK_IMAGE_LAYOUT_GENERAL` and a `GENERAL` descriptor because `shaderStorageImageWriteWithoutFormat` is enabled. Destruction is deferred to the timeline. |
| 4 | Three fixed arenas (64 MiB device, 16 MiB transient, 8 × 4 MiB host staging) sub-allocated from pre-created buffers. |
| 5 | Shader registry (SPV load / in-process source compile) and pipeline interning keyed by `PipeKey`. Graphics pipeline built with dynamic rendering and EDS3 dynamic state; no vertex/index input state. |
| 6 | `frame_program_build`: validates every `BufUse` (non-zero, in range), derives layouts/load/store from `AttUse`, plans byte-range buffer barriers and image barriers, and computes the analytic bandwidth floor. |
| 7 | `frame_begin` / `frame_run` / `frame_end`. One queue submit per frame signalling the timeline + a binary present semaphore; `acquire_done` waited at `COLOR_ATTACHMENT_OUTPUT`. Timeline retirement resolves timestamp queries and applies deferred deletes. **No `vkQueueWaitIdle`, no `vkDeviceWaitIdle`** — shutdown waits on the timeline semaphore instead. |
| 8 | Pass verbs: `pass_push`, `pass_bind_pipe`, `pass_state` (shadow-compare + EDS3), fixed/indirect draws, `pass_dispatch` / indirect dispatch, `pass_dispatch_2d`, `pass_dispatch_fullscreen_triangle`, `pass_geometry` (16-byte `GeometryRef` push constant, **no `vkCmdBindVertexBuffers` / `vkCmdBindIndexBuffer`**), note helpers, timestamp scopes, `GeometryPush` padded to the 256-byte push range. |
| 9 | Batched uploads: `vkCmdUpdateBuffer` for ≤64 KiB, otherwise staged copies in one no-wait submit. |
| 10 | `readback_request` with a real `vkCmdCopyImageToBuffer`, a narrow producer→transfer barrier, a timeline value, and a restore barrier back to the layout the descriptor expects. |
| 11 | `perf_frame_stats`, `perf_dump`, `gpu_validate`, `gpu_last_error`, `frame_budget` / `frame_stats_within_budget` / `frame_bottleneck`, shader-stats import + budget gate, `pipe_budget_report`, conditional escapes (`MU_ESCAPE`), and the private `set_surface` WSI hook. |

`src/vk_v2_internal.h` holds the six integration hooks the public API has no
verb for: `set_compiler`, `shader_register_spv`,
`pipe_intern_compute`, `set_surface`, `set_hw_budget`,
`set_shader_stats` / `import_shader_stats`, and
`target_texture` (the readback bridge, see gap G1). No public Vulkan handle
leaks into the header.

---

## 3. Verification (exact, 2026-11-08)

### Compile matrix — all `rc=0`, `-Wall -Wextra -Werror`

| Flags | Result |
|-------|--------|
| `-std=c11` | clean |
| `-std=c11 -DMU_ESCAPE` | clean |
| `-std=c2x` | clean |
| `-std=c11 -O2 -fno-strict-aliasing` | clean |

Both the default and the `-DMU_ESCAPE` variants also **link and run** the
full smoke test.

### Runtime — Lavapipe, `llvmpipe (LLVM 19.1.7, 256 bits)`, API 1.4.305

`./build/smoke` → `ALL CHECKS PASSED (0 failures)`.

The test is not a compile-only check. It:

1. Creates a device, a device buffer, a 64×1 storage target, a 256×128 colour
   target, an uploaded 4×4 texture, and a sampler.
2. Registers two Slang-produced SPIR-V modules (compute `"main"`, and a module
   with `vs_main` / `fs_main`), interns **2** pipelines, and verifies
   `pipe_find` is idempotent.
3. Builds a 3-pass program (compute write → graphics fullscreen triangle →
   compute read) and verifies a deliberately malformed zero-size `BufUse` is
   rejected at build time.
4. Runs **3 real frames** with one submit/present each.
5. Reads the storage image back and checks the actual GPU-produced bytes:

   ```
   readback pixel[0]  = 0 128 64 255
   readback pixel[63] = 255 128 64 255
   ```

   This is the ramp the compute shader writes (`r = x/63`, `g = 0.5`,
   `b = 0.25`, `a = 1`). A backend that never submitted would return zeros.

6. Verifies resolved timestamp queries (`gpu_ms > 0`, e.g. `0.432 ms` on one
   run), `pass_count == 3`, and the four zeros: `queue_waits`,
   `pipeline_creations`, `descriptor_writes`, `spill_bytes` all `0`.
7. Exercises `frame_stats_within_budget` (WITHIN), `frame_bottleneck`,
   `perf_dump`, shader-stats import (2 rows), `shader_budget_ok` (accept 40
   VGPRs, reject 120), `pipe_budget_report`, all invalid `ComputeShape` cases,
   the §16 inline arithmetic, `gpu_validate`, and teardown.

Representative output (`tests/smoke_perf.txt`):

```
# cpu 0.006 ms  gpu 0.473 ms  passes 3  draws 0  dispatches 2  pushes 3
# barriers 3  state_changes 1  state_runs 1  layout_transitions 2
# zeros: queue_waits 0  pipeline_creations 0  descriptor_writes 0  spills 0
# uploads 0 B  floor 0.000 ms  bytes_moved 131840 B  escape_uses 0
```

`tests/smoke_report.txt` is the pre-run cost preview;
`tests/smoke_report_after.txt` is the same report after the timeline resolved
and populated the measured `gpu_us` column next to the analytic floor.

### Scripts and scanners

| Check | Result |
|-------|--------|
| `python3 scripts/section_index.py --check` | OK, 19 sections 1..19 contiguous |
| `python3 scripts/layer_check.py` | OK: 67 public functions (73 exports), 57 types, 12 constants, 13 bpp values, 19 sections in sync |
| `python3 shaders/mu/tests/validate_math.py` | all numerical checks passed |
| `python3 shaders/mu/tests/measure.py` | `probe.slang: 7076 SPIR-V ops, 66 control-flow ops, 0 warnings`; leaf-only `6756 / 0` |
| `vk_scan.py src` | 27 findings: **0 critical, 0 high**, 7 medium, 8 low, 12 info |
| `shader_scan.py shaders/mu` | 54 findings, all `info` |
| ASan/UBSan-style build (`-fsanitize=address -O0 -g`) | clean, `ALL CHECKS PASSED` |

`measure.py` now falls back to `slangc -target spirv-asm` when `spirv-dis` is not
installed (it is not in this sandbox). The fallback normalises Slang's
`<lineno>:<op>` output to the shape `spirv-dis` produces and reproduces the
exact previously-delivered counts (7076 / 66, leaf 6756 / 0).

---

## 4. Static-scanner triage

`vk_scan.py src` was clean (0 findings) when `src/` contained only the header.
The implementation necessarily contains the Vulkan spellings the boundary is
made of, so it now reports 27 triage items. **All critical/high items were
fixed, not waived**:

- `wait-idle` (critical) at `gpu_destroy` → replaced `vkDeviceWaitIdle` with a
  host `vkWaitSemaphores` on the backend's own timeline.
- `rw-memory-barrier` (high) at `readback_request` → narrowed
  `VK_ACCESS_MEMORY_WRITE_BIT` / `BOTTOM_OF_PIPE` to
  `SHADER_WRITE | COLOR_ATTACHMENT_WRITE` → `TRANSFER_READ`.

The remaining 27 are accepted with reasons:

| Rule | Count | Why it is intentional |
|------|-------|-----------------------|
| `general-layout` | 6 (medium) | Vulkan core has no narrower layout for read/write storage images. `ATT_STORAGE_RW` explicitly prices the GENERAL path; `ATT_STORAGE_READ` uses `SHADER_READ_ONLY_OPTIMAL`. Do not "fix" by claiming compression. |
| `load-op-load` | 1 (medium) | Returned only for `ATT_WRITE_KEEP` / `ATT_STORAGE_READ` / `ATT_DEPTH_READ`, i.e. a partially written attachment. Fully overwritten attachments get `ATT_CLEAR`/`DONT_CARE` → `LOAD_OP_CLEAR`/`DONT_CARE`. |
| `four-float-target` / `four-msaa` | 2 (low) | Enum/table entries for `FMT_RGBA32F` and `SC_4`; the backend does not create either by default. |
| `per-frame-view`, `per-frame-descset` | 5 (low) | All are in `gpu_create` / init (`vkCreateImageView`, `vkCreateDescriptorPool`, `vkAllocateDescriptorSets`, `vkUpdateDescriptorSets`). The steady-state frame writes zero descriptors, confirmed by `FrameStats.descriptor_writes == 0`. The scanner cannot distinguish init from the frame loop. |
| `pipeline-create-in-frame` | 2 (low) | `build_graphics` / compute creation inside `intern_key`, which is called only from `pipe_intern` / shader registration. `FrameStats.pipeline_creations == 0` in every measured frame; a compile inside `frame_run` is now counted and fails the budget. |
| `host-visible-sampled`, `map-memory` | 10 (info) | Staging, readback and arena allocation. Persistent maps; no per-frame remap. Host-visible memory is never used as a shader-sampled resource. |
| `image-copy-blit` | 1 (info) | The readback path itself — `vkCmdCopyImageToBuffer` is the whole point of `readback_request`. |

---

## 5. Known gaps and honest limitations

These are real; they are listed rather than papered over.

- **G1 — reading a render target.** `readback_request` takes a `Texture`, but a
  render attachment is a `Target`. The smoke test uses the private
  `target_texture()` bridge. The public surface should grow a
  `readback_target()` verb (or `readback_request` should accept both handle
  kinds). No public API was silently changed to hide this.
- **G2 — WSI is unverified.** `set_surface()` is implemented (surface caps,
  format/mode selection, swapchain creation, per-image views, swapchain target
  registration), but the sandbox has no window system.
  `VK_EXT_headless_surface` exists on Lavapipe yet reports
  `surfaceSupport = 0`, `0` formats and `0` present modes, so no swapchain can
  be created here. The backend therefore defaults to a headless internal image
  and `frame_end` presents only when a real swapchain exists.
- **G3 — no validation layer.** `VK_LAYER_KHRONOS_validation` is not installed;
  `gpu_create(validation=true)` records a note and continues. Correctness was
  checked with the runtime readback, ASan, and `gpu_validate`, **not** with
  validation-layer messages.
- **G4 — Lavapipe is software.** All timings above (`gpu_ms ≈ 0.4–0.7 ms`,
  bottleneck `ALU`, ratios in the hundreds of thousands) come from a CPU
  rasteriser. They are **not** RX 9070 XT / RDNA4 measurements and must not be
  used as such. The hardware budget constants default to the `STATUS.md` values
  (644.6 GB/s, 600 ns, 64 CUs) but no hardware counter was queried.
- **G5 — shader stats are imported, not measured.** `import_shader_stats`
  parses a text file. No `RADV_DEBUG=shaderstats`, RGP/RGA, ACO listing, VGPR
  count, spill count, wave occupancy, or frame-time A/B exists.
  `frame_bottleneck` returns `NONE` without real data; the smoke test's `ALU`
  verdict is driven by the tiny analytic floor versus a software-renderer
  timestamp.
- **G6 — in-process Slang compile is a hook, not a linked compiler.**
  `shader_compile_in_process` loads SPIR-V via the registered compiler callback;
  no `slangc` library is linked into the backend. The smoke test registers two
  pre-compiled `.spv` modules.
- **G7 — `set_surface` is one-shot.** It does not destroy/re-create an
  existing swapchain on resize; the app must tear down and rebuild. Static
  capacities (`SLOT_*`, arena sizes, frames in flight) are fixed tables, not
  growable.
- **G8 — fixed function coverage.** Indirect draws are wired through
  core `vkCmdDrawIndirectCount`; no mesh shaders, ray tracing, video, or
  multi-queue transfer are exposed, because the public header does not declare
  them.

---

## 6. What is *not* claimed

- No speedup percentage, frame-time win, ISA delta, register count, or wave
  occupancy is claimed anywhere in this backend or its reports.
- The §18 `frame_bottleneck` output is a classification from a number and a
  budget, as the skill requires; on Lavapipe it says `ALU` because the software
  rasteriser is slow relative to a 0.2 µs analytic byte floor. That is a
  statement about the test device, not about RDNA4.
- `RADV_DEBUG=shaderstats` or RGP/RGA on the RX 9070 XT remains the next
  measurement before any ISA or frame-time claim.

---

## 7. Files

| Path | Purpose |
|------|---------|
| `src/vk_v2.h` | Public 19-section / 67-function API (unchanged by this implementation pass). |
| `src/vk_v2.c` | The backend. |
| `src/vk_v2_internal.h` | Private integration hooks. |
| `external/vulkan/include/` | Vendored Vulkan 1.4 headers (`VK_HEADER_VERSION 362`). |
| `build.sh` | Build + export manifest. |
| `tests/smoke.c` | Headless end-to-end test. |
| `tests/smoke_cs.slang`, `.spv` | 64-thread compute shader; writes a ramp to binding 1 + a counter to binding 3. |
| `tests/smoke_triangle.slang`, `.spv` | `vs_main` / `fs_main`, no vertex/index buffers. |
| `tests/smoke_*.txt` | Generated reports from the last run (evidence, not source). |
| `scripts/vk_v2_exports.txt` | `nm` export manifest consumed by `layer_check.py`. |
| `scripts/layer_check.py` | Now also verifies implementation completeness. |
| `shaders/mu/tests/measure.py` | `spirv-dis` → `slangc -target spirv-asm` fallback. |
| `docs/vk-v2-skills-audit.md` | Header-design audit (date corrected to 2026-11-08; halo tile labels corrected to 8/16/32). |
