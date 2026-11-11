# Core rewrite plan — `vk.c` / `vk.h` / `renderer.c` / `renderer.h`

Target: `github.com/Abhinavpatel00/mu_gfx_new` @ `HEAD`
Hardware: RX 9070 XT (Navi 48, GFX12), RADV, Mesa `27.0.0-devel`

**Rev 2 — no file split.** Rev 1 proposed cutting `vk.c` into twelve translation units. Rejected, and
the objection is right: `vk.c` has **58 `static` helpers** (measured), so splitting the file means
either moving them out or publishing them in a `vk_impl.h` of internal prototypes. Both add
indirection to code whose whole virtue is that a reader can walk it in one pass. The target is now the
same structural fixes, achieved **inside** the two files: section discipline, decomposed god
functions, two small checker scripts, and the performance work — which never depended on layout.

Everything marked **measured** was read out of the checkout or produced by running a tool; anything
marked **recommended** is a judgement call you can overrule.

---

## 0. What I actually looked at

Cloned the repo, then:

| Step | Result |
|---|---|
| `wc -l` on the four files | `vk.c` 4,205 · `vk.h` 1,080 · `renderer.c` 2,932 · `renderer.h` 118 |
| Read `renderer.txt`, `dataflow.txt`, `AGENTS.md`, `README.md`, `todo.txt`, `docs/gpu-driven-dod-redesign-v2.md` | the frame chain, ownership model and conventions are documented, and they are *good* |
| Read `begin_pass`/`end_pass`, `vk_backend_create`, `create_texture`, `create_graphics_pipeline`, `renderer_frame`, `renderer_resources_create`, `renderer_destroy`, `struct VkBackend`, `struct Renderer` | see §1 |
| **Structure census**: functions, statics, section banners, comment density | 133 functions, **58 `static`**, **6 separator banners in 4,205 lines** (one per 700 lines), 18 top-level block comments — §1.1 |
| `vk_scan.py src` (Vulkan-level triage scanner from the vulkan-opt skill) | **41 findings — 21 false positives**, 20 worth a look, ~5 real decisions (§1.3) |
| `shader_scan.py shaders` | **20 findings**, 13 medium, all real (§1.4) |
| grep for `begin_pass`/`end_pass`, `vkUpdateDescriptorSets`, `wait_idle`, one-shot submit helpers | counts in §1.3 |

This is not a codebase that needs rescuing. The pass model, the epoch-based barrier tracking, the
bindless single-set design, the GPU-driven cull→compact→route-split scene and the written-down
conventions are better than most engines this size. The problems the rewrite solves are
**discoverability inside the files**, **three concrete costs** (§1.3), and **two god functions**.

---

## 1. Current state, measured

### 1.1 The four files

| File | Lines | What is in it | The actual problem |
|---|---|---|---|
| `vk.c` | 4,205 | instance, physical device scoring, device+caps, VMA, swapchain, render-target pool, buffers and buffer pools, textures, samplers, shader modules + hot reload, graphics/compute pipeline creation, bindless descriptor set, `begin_pass`/`end_pass` + barrier batch, command buffers, frame submit, GPU profiler, memory tracking, one-shot upload helpers | eleven concerns, **and only 6 banners to navigate them by.** Nothing tells a reader where the texture code starts |
| `vk.h` | 1,080 | every type, enum, struct, prototype | 73 prototypes, **un-grouped**; the declaration order does not match the definition order in `vk.c`, so a jump from header to implementation always lands somewhere else |
| `renderer.c` | 2,932 | frame loop, post chain, settings JSON, video capture + screenshot, LUT loading, MSAA apply, resource creation, GLFW callbacks + input, Nuklear glue, profiler UI, HUD, environment, hot-reload watcher | eleven concerns again; `struct Renderer` is 64 fields and `renderer_frame` is 240 lines of sequencing interleaved with UI polling and capture bookkeeping |
| `renderer.h` | 118 | ~20 entry points (`renderer_create/frame/destroy`, `GameHooks`, `RenderEnvironment`, …) | **nothing wrong with it.** Preserve it |

### 1.2 God functions (measured, by span in `vk.c`)

| Function | Lines |
|---|---|
| `vk_backend_create` | 426 |
| `create_texture` | 337 |
| `create_graphics_pipeline` | 183 |
| `vk_instance_create` | 139 |
| `begin_pass` | 138 |
| `rt_create_at` | 122 |
| `vk_create_swapchain` | 102 |

`struct VkBackend` has 38 fields; `struct Renderer` has 64. Both are serviceable as *records* and bad
as *boundaries* — all 31 `begin_pass` call sites across `src/` reach into the `VkBackend` by value.

### 1.3 What the Vulkan scanner found — with the false positives removed

`vk_scan.py src` → 41 findings. I checked each against the code, because a scanner that reports
`vkDeviceWaitIdle` in a shutdown path as "critical" is doing you a disservice. **21 are false
positives on this codebase:**

- 8 are *definitions* in `vk.h` (`STAGE_ALL`, `ACCESS_MEM_R/W`, `LAYOUT_GENERAL`, `LOAD_KEEP`,
  `FMT_RGBA32F`, `SAMPLES_4`) — the scanner sees the Vulkan enum and assumes a barrier.
- 4 are the VMA function-pointer table in `vk.c` (`vkMapMemory`, `vkUnmapMemory`, `vkFlush/Invalidate…`).
- 4 are `vkCreateImageView` at *resource creation*, not per frame (`vk.c:870,1330,1886,1904`).
- 3 are the bindless descriptor set being created once, at init (`vk.c:3680,3697,3706`).
- 2 are `vkCreateGraphicsPipelines` at pipeline build / MSAA rebuild (`vk.c:2416,2419`).

**The 20 that survive, and what they actually cost:**

| Item | Sites | Verdict |
|---|---|---|
| `vkDeviceWaitIdle` / `vkQueueWaitIdle` | 6 | **all off the per-frame path** — teardown (`renderer.c:613`), video-stop (`:633`), MSAA toggle (`:1141`, documented hot-swap), swapchain recreate (`vk.c:942`), the `wait_idle()` API. Not a bug. **But** the helper underneath, `vk_end_one_time_cmd` (`helpers.h:233-246`), does `vkQueueSubmit` **+ `vkQueueWaitIdle`** for *every* one-shot command buffer — that is the whole texture/asset upload path blocking the queue every time. **Highest-value real fix in the file set** (§4.2) |
| Storage-image `GENERAL` layouts | 4 real (`vk.c:898,1396,1965,3106`) | **a real decision** (§4.1). Storage images written by compute and later sampled keep `GENERAL` all frame, and `GENERAL` on a compute/transfer queue is where RADV stops keeping DCC (`vulkan-opt` `02` §2.2: "`GENERAL` on a texture you only sample is pure loss"); §2.1 adds that a clear is fast-clear eligible only in an attachment layout, so "`GENERAL` plus a manual clear" pays full price |
| `ALL_COMMANDS` pipeline barriers | 2 real (`two_d/picture.c:131,226`) + 2 semaphore stage masks (`vk.c:3908,3919`) | the barrier ones run inside a one-shot submit-then-wait, so they cost nothing measurable — **cosmetic, fix while touching.** The timeline semaphore signalling at `ALL_COMMANDS` is worth narrowing on principle |
| Screenshot readback + swapchain blit | `renderer.c:876,2418` | correct as written; the blit is the documented negated-Y workaround (`renderer.txt`: "Do not 'fix' the negated-Y naively") |

### 1.4 What the shader scanner found (all 13 mediums are real)

| Site | Finding | Note |
|---|---|---|
| `slime_sim.slang:63,152,317` | `atan2(dx, dz)` ×3 | `atan2` is one SPIR-V op and ~109 RDNA instructions; AI-facing yaw is the exact case where the odd-polynomial form or an octahedral encoding is ~10× cheaper (`vulkan-shader-opt` `08` §6) |
| `glade_sky.slang:111,137` | `pow(sun_dot, 3.0)` ×2 | measured in the skill: `pow(x,3)` still reaches SPIR-V as `OpExtInst Pow` at `-O3` (slangc 2026.21). Write `x*x*x` |
| `scene.slang:71` | `1/sqrt(x)` | `rsqrt` is one quarter-rate op instead of two |
| `scene.slang:200,271`, `compact.slang` | `discard` | **deliberate and already better than the advice** — the route split separates discarding groups so the opaque route compiles `SCENE_NO_DISCARD` and keeps early-Z. Keep |
| `markdown_reader.slang:103` | `discard` | UI path, irrelevant to frame time |

### 1.5 Small things worth deleting on day one

- `src/vk.c.bak` — 152 KB of the *previous* `vk.c`, in the tree, which will silently rot into
  misleading "reference" code.
- `--help` as a *file name* at repo root (640 bytes) — looks like a shell-redirect accident.
- `begin_pass` 31 call sites, `end_pass` 30; one of each is the definition, so they balance — but the
  pairing is unchecked. §3.4 turns that into a debug-build invariant.

---

## 2. What must survive the rewrite, verbatim

These are the assets. A rewrite that loses one is a regression no matter how clean the file reads.

1. **`PassDesc` + `begin_pass`/`end_pass` with per-resource declarations.** Buffer reads/writes are
   byte ranges (`BufferAccess`), images are mip-granular attachments, and one flush point emits the
   batch immediately before the pass body. The comment in `begin_pass` explains why the flush cannot
   move — that reasoning *is* the design.
2. **The pass epoch (`pass_serial`).** A target listed twice in one pass emits one barrier and widens
   the recorded source mask. Keep the mechanism and the comment.
3. **The single bindless descriptor set** (one set, `UPDATE_AFTER_BIND`, 65,536 textures, 256
   samplers), bound once per frame for graphics and compute.
4. **The GPU-driven scene path** (`src/three_d/`) — cull → sort → histogram → scan → emit → scatter
   with route-split command lists and `vkCmdDrawIndexedIndirectCount`. **Out of scope. Don't touch it
   while the foundation moves.**
5. **The negated-Y viewport convention** and its table in `README.md`. Documented, load-bearing,
   repeatedly misdiagnosed. The rewrite must not "clean it up".
6. **The env-knob A/B culture** (`MU_NO_SORT`, `MU_NO_SPLIT`, `MU_PREZ`, `MU_MSAA`, `MU_PERF`,
   `MU_SHOT`, `MU_RDOC_FRAME`). This is how the project knows what it knows.
7. **`renderer.h`'s public surface**, unchanged.

---

## 3. Target architecture: in-file, not cross-file

The constraint is one `vk.c` and one `vk.h`. The goal that splitting was meant to serve was:
*a reader can find the code for a concern, and can tell when code in one concern reaches into
another.* Both are achievable inside one file, with the compiler replaced by two small scripts.

### 3.1 `vk.c` becomes ten labelled sections, in dependency order

Section banners already exist as an idea — there are 6 of them. Target: **10 sections, one per ~420
lines**, ordered so that everything is defined before it is used:

| # | Section | Contents today (approx. lines) |
|---|---|---|
| 1 | `INIT: arenas, extensions, physical device` | 40-360 |
| 2 | `INIT: instance, features, queue families` | 360-560 |
| 3 | `INIT: pipeline cache` | 560-640 |
| 4 | `DEVICE: create/destroy, VMA, samplers, timeline` | 640-760 |
| 5 | `SWAPCHAIN` | 756-1000 |
| 6 | `MEMORY: pools, buffers, tracker` | 1000-1110, 1450-1614 |
| 7 | `TEXTURES` | 1109-1450 |
| 8 | `RENDER TARGETS` (pool, transitions) | 1761-2000, 2900-3120 |
| 9 | `SHADERS + PIPELINES` | 2246-2900 |
| 10 | `PASSES, BARRIERS, COMMAND BUFFERS, SUBMIT, PROFILER` | 3063-4205 |

The banner format matters, because tooling parses it:

```c
/* ============================================================================
 * 7. TEXTURES
 *    create_texture, destroy_texture, texture_system_*, the upload path.
 *    May call: MEMORY, RENDER TARGETS (bindless write only).
 *    Must not call: SWAPCHAIN, PASSES.
 * ==========================================================================*/
```

That comment block is the whole mechanism: it gives a reader the map, and it gives the checker
(`§3.4`) the rules.

### 3.2 `vk.h` gets the same ten sections in the same order

73 prototypes are currently un-grouped, and the declaration order does not match the definition order
in `vk.c`. Reordering `vk.h` to mirror §3.1 means a jump from header to implementation lands in the
right place. Pure move, zero semantics — and it is the cheapest change in this plan.

### 3.3 God functions become orchestrators over statics

The rule: **a public function reads like a summary; the work happens in `static` helpers below it,
in call order.**

`create_texture` (337 lines) splits into, roughly:

```c
TextureID create_texture(VkBackend *r, const TextureCreateDesc *desc) {
    TexturePlan plan = texture_plan(desc);              /* format, mips, layers, usage */
    VmaAllocation alloc = texture_alloc(r, &plan);      /* image + memory + tag */
    Texture *tex = texture_init(r, alloc, &plan);       /* view, layout, bindless slot */
    texture_upload(r, tex, desc);                       /* staging or direct, see §4.2 */
    return tex->id;
}
```

`vk_backend_create` (426 lines) becomes ~120 lines calling ten named `static` setters
(`backend_init_arenas`, `backend_init_instance`, `backend_init_device`, `backend_init_swapchain`,
`backend_init_pools`, `backend_init_bindless`, `backend_init_pipelines`, …) — one per section above.
Nobody has to read 426 lines to know what backend creation does; anybody who needs the details drops
one level.

Naming: statics get `s_` or a section prefix (`texture_`, `rt_`, `vk_swapchain_`) so `grep` and
`ctrl-t` land in the right section. Public entry points keep their names (zero API churn), with the
namespacing argument deferred (§7.6).

### 3.4 Two scripts replace the compiler's layering and keep the index honest

1. **`scripts/section_index.py`** — parses the banner blocks, regenerates a table of contents between
   `/* BEGIN INDEX */ … /* END INDEX */` markers at the top of `vk.c` and `vk.h`, and with `--check`
   exits non-zero if the index is stale or a section has no banner. Run from `make check`. This is
   what stops the map from rotting, which is the failure mode of every hand-maintained index.
2. **`scripts/layer_check.py`** — reads the "May call / Must not call" lines from the banners and
   verifies the call graph inside the file (regex-level, ~60 lines of Python). It cannot prove
   correctness; it can catch `TEXTURES` calling into `SWAPCHAIN`, which is the mistake layering was
   supposed to prevent. Rules start empty-ish and only encode what you actually believe.

Together these give ~80 % of the benefit of separate translation units with none of the indirection:
**the map and the layering rule become assertions instead of conventions.**

### 3.5 `renderer.c` gets the same treatment — with one new struct

Assumed the same preference applies here (say the word if not). Sections:

`1 SETTINGS (PostSettings + JSON)` · `2 CAPTURE (video/screenshot/readback)` · `3 LUT` ·
`4 TARGETS` · `5 MSAA` · `6 RESOURCE CREATE` · `7 POST PASSES (bloom/SSAO/SMAA/tonemap/blit)` ·
`8 FRAME LOOP` · `9 PLATFORM (GLFW callbacks, input, watcher)` · `10 UI (Nuklear, profiler, HUD)`

And the one new data structure, which is not about file layout and is worth doing regardless:

```c
/* Every per-swapchain-image target, one record, one create/destroy. Today these
   are 11 parallel arrays in struct Renderer plus a 30-line teardown loop; adding
   a target means editing three places and remembering which one you forgot. */
typedef struct RendererTargets {
    uint32_t image_count, width, height, scene_samples;
    struct { RenderTargetId depth, hdr, depth_msaa, hdr_msaa, ldr,
                    smaa_final, smaa_edges, smaa_weights, ssao;
             RenderTargetId bloom[BLOOM_MIPS]; } rt[MAX_SWAPCHAIN_IMAGES];
} RendererTargets;
```

It collapses the resize path, the MSAA re-create path and the 30-line `renderer_destroy` loop into
one call each, and makes attachment/sample-count matching (`renderer_scene_sample_count`)
impossible to get wrong per target.

### 3.6 What single-file costs, and how each cost is paid

| Cost | Honest size | Mitigation |
|---|---|---|
| **Recompile-everything on every edit** | 4,205 lines of C + 1,080 of header. At this size it is seconds, not minutes — check it with `time make` before caring | keep `vk.h` free of heavy dependencies it does not need (it currently pulls `vulkan_core.h` and two `mu` headers; that is already minimal). If it ever hurts, a `vk_types.h` include is a *later* option that does not disturb anything else |
| **`vk.h` is the whole dependency surface** | real: `renderer.c` cannot see a render target without also compiling the profiler's types | cheap version: `#ifndef VK_NO_PROFILER`-style guards are *not* recommended (preprocessor modes rot). Accept it, and note that the section order at least means the reader knows where to look |
| **No compiler-enforced layering** | real, and the one thing files give you for free | `scripts/layer_check.py` (§3.4) — weaker but explicit |
| **Merge conflicts** | if you ever work the same file on two branches | the section banners make conflicts *coarser* (whole-section moves instead of interleaved hunks). Not solvable in one file; acceptable for a solo project |
| **Index rot** | the classic failure | `section_index.py --check` in `make check` — the index cannot go stale without a failing build |

---

## 4. The performance work (small, specific, measured)

Independent of layout. Do it after the structural stages so a regression has one obvious cause.

### 4.1 Layout policy for compute-written targets

Four sites keep storage images in `GENERAL` for their whole life. Recommended policy, stated **once**
in the `RENDER TARGETS` section and enforced by the transition helpers:

- **Written by compute, then sampled** → write in `GENERAL`, transition to `SHADER_READ_ONLY_OPTIMAL`
  before the sampling pass. `GENERAL` on a texture you only sample is pure loss for compression
  (`vulkan-opt` `02` §2.2); the read-only transition keeps it compressed.
- **Genuinely read-write in one pass** → `GENERAL` is correct.
- **Engine-cleared** → attachment layout with `LOAD_OP_CLEAR`, not `GENERAL` +
  `vkCmdClearColorImage`: fast clear is only eligible in an attachment layout (`02` §2.1).

### 4.2 Stop waiting on the queue for uploads

`vk_end_one_time_cmd` (`helpers.h:233`) = submit + `vkQueueWaitIdle`, used by every one-shot upload.
Recommended:

1. A `transfer_begin()` / `transfer_end()` pair that records all pending uploads into one command
   buffer, submits once with the timeline semaphore you already have, and retires staging memory
   through the existing `delete_queue` when that value passes.
2. Keep the synchronous version for `renderer_create`-time load — one wait, not one per asset.
3. ≤64 KB uploads: `vkCmdUpdateBuffer` straight into the target buffer, no staging.

Effect: asset-load stalls go from per-texture to per-batch. Highest value in the list, and it does not
touch the frame path.

### 4.3 Barrier hygiene, now that it lives in one section

- narrow the timeline semaphore's signal stage mask off `ALL_COMMANDS` (`vk.c:3919`);
- add validation + `syncval` to `make check` so the per-byte-range declarations are *tested*, not
  trusted;
- add a debug-only assert that every `begin_pass` has a matching `end_pass` (§1.5).

### 4.4 Shader pass (do it while builds run)

- `slime_sim.slang:63,152,317` — `atan2(dx,dz)`. AI-facing yaw has a known quadrant, so the
  `(x-y)/(x+y)` odd-polynomial form (~12 instructions) applies; you are paying ~109 × 3 per slime per
  frame.
- `glade_sky.slang:111,137` — `pow(x,3.0)` → `x*x*x`.
- `scene.slang:71` — `1/sqrt(x)` → `rsqrt(x)`.
- **Instruction cache is a real budget here:** 32 KiB per WGP for the whole instruction cache, and
  ACO's loop-prefetch retuning is gated `gfx_level <= GFX11`, so it does not run on Navi 48 at all
  (`aco_assembler.cpp:1750-1752`). Code size in the scene FS is first-order; unrolling that does not
  reduce wave count is a loss.

---

## 5. Staged plan — every stage builds, runs, and reverts

| Stage | Work | Definition of done |
|---|---|---|
| **S0** | Baseline harness: `make check` = build + `vk_scan` + `shader_scan` + 300-frame `MU_PERF` with vsync off; save `perf_report.txt` to `docs/baseline/`; delete `vk.c.bak` and `--help` | baseline committed, numbers reproducible |
| **S1** | `vk.h` reordered into the ten sections (§3.2). **No semantics change.** | identical output; `clang -H` unchanged or smaller |
| **S2** | `vk.c` banners + section reorder (§3.1) + `section_index.py` + `layer_check.py` in `make check` | index generated, checker green, frames bit-identical in RenderDoc |
| **S3** | Decompose `create_texture` (337→~5×70) and `vk_backend_create` (426→~120 + 10 setters) (§3.3) | public functions read as orchestrators; identical behaviour and memory report |
| **S4** | `RendererTargets` + collapse resize/MSAA-create/teardown loops (§3.5) | resize 200× with no leak (`vk_mem_report`); MSAA toggle identical |
| **S5** | `renderer.c` sections (§3.5); `renderer_frame` becomes a readable pass list under ~80 lines | parity across all demos, `MU_SHOT` screenshots unchanged |
| **S6** | Perf: §4.2 upload batching, §4.1 layout policy, §4.3 masks/asserts | measured deltas vs S0 |
| **S7** | Shaders (§4.4) + docs: update `renderer.txt`/`dataflow.txt` anchors, `AGENTS.md` §3 gets the section table | a newcomer can name the section a change belongs in |

Effort at your pace: S0–S2 one evening (S2 is mostly cutting banner comments and moving blocks);
S3 two evenings; S4–S5 two evenings; S6 one evening per item once measured; S7 a few hours.

---

## 6. Acceptance gates (run after every stage)

1. `make` clean at `-Wall -Wextra`, no new warnings.
2. Every demo in `demo/` starts, renders and exits under scripted `MU_PERF=300` / `MU_SHOT`.
3. `perf_report.txt` per-pass ms within noise of `docs/baseline/` — **any** >2 % frame-time change in
   S1–S5 is a bug, not a win.
4. `vk_scan.py src --json` count does not increase; the 21 known false positives recorded in
   `docs/scan-baseline.md` so future reports are diffable.
5. Validation + syncval clean on one asset-heavy scene.
6. `vk_mem_report` at shutdown matches baseline after resize/MSAA churn.

---

## 7. Risks, and the residual decisions

| Risk | Containment |
|---|---|
| Section reorder in `vk.c` breaks something through a static forward-declaration | S2 moves code but changes no expressions; the 58 statics already have forward declarations where needed — the compiler catches every break immediately |
| Header reorder breaks the enums-as-`static const` fold the short names rely on (clang-only trick, see `vk.h` and `README.md`) | S1 is compile-only; if the fold breaks it fails at the first enum. Keep the umbrella include list unchanged |
| Index rot | `section_index.py --check` in `make check` |
| `RendererTargets` migration leaves a stale id in an old array | switchover in one commit, old arrays deleted, not kept "just in case" |
| GPU-driven scene path drifts | untouched; `dataflow.txt` is the check — if a slice's producer/consumer changes, the stage was wrong |

**Decisions still open** (defaults in brackets, so the plan can run without an answer):

1. **`renderer.c` under the same rule?** [yes — same reasoning applies]
2. **Where the perf work sits.** [S6, after the structure stages]
3. **Namespacing** (`create_texture` → `vk_texture_create` etc.; these are the only public functions
   in the tree with no prefix and they collide with names every demo invents). [deferred to S7;
   costs a sweep through `demo/` and `src/gfx/app.c`]

Say "go" and I start at S0: baseline harness, dead files deleted, `make check` in place — so every
later stage is measured rather than asserted.
