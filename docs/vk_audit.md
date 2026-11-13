# `src/vk.c` / `src/vk.h` — audit against mu_lib doctrine

Scope: the whole of `src/vk.h` and `src/vk.c` (~4,100 lines), judged against
`external/mu_lib/AGENTS.md` (DOD doctrine, §12 C mechanics, §12.5 "no null
check for a bug", §12.6 "no helpers with one call site") and the repository
`AGENTS.md`, which defers to it.

Every finding below is either **fixed** in this branch or **open** with a
reason. Fixes are verified by the project's own strict compile line plus a new
no-device test, `tests/vk_invariants.c`.

```
gcc -std=c11 -O2 -Wall -Wextra -Werror -Wno-error=unused-variable \
    -Wno-error=unused-but-set-variable -fno-strict-aliasing \
    -Isrc -Iexternal/vulkan/include -Iexternal/volk -Iexternal/mu_lib/mu \
    -c src/vk.c -o /tmp/vk.o          # exit 0, no diagnostics
```

---

## 1. Correctness bugs

These are cases where the code was wrong, not merely unidiomatic. They are
listed roughly in order of how much damage they could do.

### 1.1 A planned program replayed every frame used build-time image layouts — fixed

`plan_barriers()` bakes a `VkImageMemoryBarrier` per transition, including
`oldLayout`. `apply_barriers()` then replays that plan on **every** frame. For
a persistent image the layout the plan saw at build time is only correct on
the frame it was built for; on frame N+1 the image is wherever frame N left
it. The planner was seeded from
`TextureSlot.layout` (itself fixed this session — it used to be seeded from
`UNDEFINED` unconditionally), but even with a correct seed the barrier read
back was stale by the second replay.

The visible failure is a transition whose `oldLayout` disagrees with the real
layout: a validation error, and on tilers a discarded or corrupted image.

**Fix.** The plan no longer decides the source layout; it records *whether it
is allowed to discard*. `BarrierImg.discard` is set only where the planner
already had a justification to say `UNDEFINED` — an attachment the pass
overwrites in full (`ATT_WRITE`/`ATT_CLEAR`, `ATT_DEPTH_WRITE`/`ATT_DEPTH_CLEAR`).
Everything else resolves at record time through one function:

```c
static VkImageLayout barrier_src_layout(const TargetSlot *ts, const BarrierImg *b,
                                        VkAccessFlags *src_access,
                                        VkPipelineStageFlags *src_stage);
```

which returns the live layout for a persistent image and the baked-in
`UNDEFINED` for a frame-local one (`TargetSlot.frame_layout`, reset to
`UNDEFINED` in `frame_begin()` for `RES_FRAME` slots only). The stage and
access masks are recomputed from the real layout, so a barrier no longer names
a writer that is not there.

*Test:* `test_cross_frame_layout` in `tests/vk_invariants.c` drives two frames
of one program and asserts frame 2's non-discarding barriers never start from
`UNDEFINED`. Reverting the fix makes it fail.

### 1.2 The barrier table was smaller than the number of images a pass can name — fixed

`BarrierPlan.imgs` was sized `MAX_COLOR_ATTACHMENTS + 1u + MAX_BINDS_PER_PASS`
= **25**. A pass can name `MAX_COLOR_ATTACHMENTS` colour attachments, one depth
attachment, `MAX_BINDS_PER_PASS` sampled reads and `MAX_BINDS_PER_PASS` storage
writes — **41 distinct slots**, and 32 of them are reachable at the binding cap
the build check already enforces. Past the 25th distinct image,
`add_img_barrier()` fell off the end of its `if` and **dropped the barrier
without a word**, leaving a later pass reading a layout the GPU never
transitioned to.

**Fix.** `MAX_IMAGES_PER_PASS` names the real bound; the table uses it, and the
drop path is now an assert instead of a silent return (asserts are compiled out
under `NDEBUG`, so the build-time check is the actual guarantee).

*Test:* `test_barrier_table_capacity` builds a pass at the binding cap naming 32
distinct images and asserts 32 barriers are planned. Against the old table size
it fails with `planned 25 barriers`.

Same class of defect in `BarrierPlan.bufs` (2 per use against a possible 32
uses) — now asserted against the build-time limit.

### 1.3 `add_img_barrier()` merging could produce an illegal discard — fixed

The merge path (two passes in one program touching one image) used last-wins.
With `discard` in play the merge must be the conservative intersection: the
barrier may discard only if **both** uses could. Now `e->discard = e->discard
&& b->discard`.

### 1.4 The swapchain image count was trusted blindly — fixed

`set_surface()` clamped the count returned by `vkGetSwapchainImagesKHR` to
`MAX_FRAMES_IN_FLIGHT` and then filled `swapchain_images[]`/`swapchain_views[]`
— arrays of exactly that size. The clamp happened before the fill, so this was
not an overflow. What was missing: neither query checked its `VkResult`, a zero
count was not rejected, and the second query's count was trusted independently
of the first. Silently truncating also hid the real problem — a swapchain that
does not fit is a configuration the caller should hear about.

**Fix.** Both queries check their `VkResult`, a count of zero or above
`MAX_FRAMES_IN_FLIGHT` is refused with a message instead of silently truncated,
the second query's count is re-checked, and the allocation is null-checked.

### 1.5 `gpu_create()` leaked every object created before a failure — fixed

Five error paths did `vkDestroyInstance(); free(g);`, which drops the
`VkDevice` if one exists and skips `vkDestroyDevice` entirely. Three others
did `free(g)` with objects live.

**Fix.** Every path unwinds through `gpu_destroy()`, which now returns early
for a `Gpu` that never got an instance and for one with no device (destroying
the surface and instance on the way out). Verified under ASan/UBSan: on this
sandbox `volkInitialize()` fails, and the failure path exits clean.

### 1.6 Table slots leaked on a failed create — fixed

`buffer_create()` claimed a buffer slot and returned `BUFFER_NIL` if
`make_buffer()` failed, leaving the slot `live` and permanently unusable.
`sampler_create()` released its bindless index but left the slot filled.
`upload_end()` returned early on a command-buffer or submit failure, stranding
the command buffer, the staging chunk (which every later `upload_begin()` then
waits a second for), and the `Upload` handle.

**Fix.** All three give their resources back on the failure path, with the
generation bumped so a stale handle cannot alias the returned slot.

### 1.7 `upload_texture()` forced every texture into `SHADER_READ_ONLY` — fixed

A texture created with `TEX_STORAGE` rests in `GENERAL` (that is the layout
`write_image()` writes its descriptor with), but `upload_texture()` ended with
a hardcoded transition to `SHADER_READ_ONLY_OPTIMAL`. After any upload the
image was in the wrong layout for its own descriptor.

**Fix.** `idle_layout_for()` decides from the texture's usage flags. The same
function now validates the region against the texture extent and the source
span against the region size — previously a short `DataSpan` had
`vkCmdCopyBufferToImage` read past the end of the staging buffer.

### 1.8 Static depth state was absent from the pipeline key — fixed earlier this session

`PipeKey` had no `depth_cmp`/`depth_write`, so two pipes that differed only in
depth state collapsed in the intern table and the second got the first's
pipeline. Recalled here because it is the same class of bug as 1.1: identity
that does not describe the object it identifies.

### 1.9 Smaller fixes applied

| Finding | Fix |
| --- | --- |
| `intern_key()` ignored `PipeKey.vs_entry`/`fs_entry`, so a custom entry point was never used and two pipes differing only in entry point collided | entry names now live in the key and `build_graphics()` reads them from it |
| `frame_begin()` treated `VK_SUBOPTIMAL_KHR` as failure and used a stale image index | accepted, index bounds-checked |
| `pass_dispatch`/`pass_draw_indirect` coerced a zero count to 1 | return early |
| `PUSH` could not express a full 256-byte payload | `PUSH_PAD_` pads to exactly `PUSH_MAX` |
| `to_vk_stage_bits()` OR'd `ALL_GRAPHICS` into every non-zero mask | map exactly, and `to_vk_stage_extra()` adds `DRAW_INDIRECT_BIT` |
| `ACC_TRANSFER` named a write on the source side | split by direction |
| `plan_barriers()` overwrote instead of OR-ing a read's write state | OR |
| `upload_begin()` did not reset a waited chunk | resets |
| `draw_ready()`/`pipe_bound` were never read | enforced before every draw and dispatch |
| transient arena peak was a session high-water mark reported as a per-frame statistic | reset in `frame_begin()` |
| `buffer_destroy()` silently no-oped for arena slices | distinguishes a created buffer from a slice and retires the slice's declared range |

---

## 2. Doctrinal gaps (not bugs)

### 2.1 `mu_lib` primitives are unused — the headline finding

`src/vk.c` contains **zero** uses of `MU_ASSERT`, `MU_PANIC`, `MU_RESTRICT`,
`mu_arena`, or `mu_pool`, while `mu_common.h` is included by `src/vk.h` and the
whole repo is organised around that library. The backend reimplements an
arena (`ArenaState`), a free-list (`new_*_slot`), and its own error channel
instead.

Two of these are deliberate and should stay:

* **The `bool` + `gpu_last_error()` contract.** The repository `AGENTS.md`
  exempts it explicitly. A library whose failure mode is `MU_ASSERT` cannot
  report a table-full to a demo that wants to keep running.
* **The hand-rolled slot tables.** They need a generation per slot for handle
  identity; `mu_pool` does not provide that, and the generation is load-bearing
  (`HANDLE_MATCHES`).

What should change:

* `_Static_assert` → `MU_STATIC_ASSERT` (**done**, 15 sites), and `MU_ASSERT`
is now on the two barrier-table bounds that used to drop entries silently. One name for one
  construct across the repo, and it matches the mu_lib spelling.
* `MU_ASSERT` belongs on internal invariants that are *true by construction*
  and whose violation is a backend bug rather than a caller error: the record
  path's "a draw has a bound pipe" (`draw_ready()` now enforces it, but an
  assert would catch the case where a future caller skips it), and the barrier
  array bounds in `apply_barriers()`, which are checked at plan time and then
  trusted. **Open**: adding asserts through `apply_barriers()` and the record
  path is mechanical, but that code cannot be executed in this environment (no
  Vulkan driver), and an assert that has never been seen to fire is not worth
  much. It belongs in a change that can be run on a real device.

Note also that `MU_ASSERT` compiles to a `SIGABRT`, which is the right
behaviour for a backend bug and the wrong behaviour for a caller passing a nil
handle — the `bool` + `gpu_last_error()` split in this file is exactly that
distinction, and it should not be collapsed.

### 2.2 `-Werror` is on, and it earned its place

Four real defects this session were caught by the compiler alone: `-Wrestrict`
on `snprintf` into a struct field, `-Wunused-parameter` after a signature
change, `-Wimplicit-function-declaration` from a helper defined after its use,
and a `_Static_assert` that fired on my own first `draw_sort_key` packing. The
correct response in each case was to fix the code, not the flag — which is the
mu_lib rule, and it held.

### 2.3 DOD at the boundary, AoS inside

`vk.h`'s public surface is DOD-shaped: handles are `{index, gen}`, `PipeKey` is
a value type compared by fields, `DrawState` is an 8-byte packed sort key,
`FrameStats` is a plain struct. That is right and matches mu_lib.

Inside `Gpu`, everything is array-of-struct. For the working set that is the
correct choice — a draw walks all of `DrawState`, and the pass plan walks all
of `BarrierImg` — so splitting into SoA would add indirection without helping
a sequential walk. Not a finding; noted so it is not "fixed" later.

The one place the layout does cost: `Gpu` is 555 KB and `FrameProgram` 274 KB,
both `calloc`'d once. `FrameStats` is 13.9 KB and is copied whole per frame
(`g->active_stats`, plus `f->pending`). At 60 Hz that is ~1.7 MB/s of `memcpy`,
which is not a problem but is also not free, and the copy is the only reason
`perf_frame_stats()` needs to be cheap. Left alone.

### 2.4 Config section duplicated a definition — fixed

`MAX_PIPES` was defined twice in §1 (24, then 256). A `#define` with no
`#undef` and a new value is a `-Wmacro-redefined` waiting for a compiler that
enables it, and it made the capacity impossible to reason about. §1 is now one
list with the `_Static_assert`s that constrain it.

### 2.5 `key_equal()` compares entry-point names with `strcmp`

Correct, and it must stay: it compares names because *the names are what the
pipeline is built from*, and two different `char*` with equal contents are the
same entry point. The cost is a `strcmp` per resident pipe per intern attempt.
With `MAX_PIPES 256` that is at most 256 short string compares on a cache
miss — acceptable. Pointer comparison would be wrong (two `pipe_intern()` calls
with identical literal text are the same pipeline). **No change.**

### 2.6 The `third_party/mu_lib` copy is not reproducible

`src/vk.h` includes `../third_party/mu_lib/mu/mu_common.h`, and
`git ls-files -s third_party/mu_lib` shows a **gitlink** at `5d98890`, but
`.gitmodules` has no mapping for that path — `git submodule status` says so
out loud. The directory was populated this session by copying
`external/mu_lib/mu/` into it.

That is a real reproducibility defect: a fresh clone cannot build `vk.h` at
all. **Open**, and deliberately not fixed here: choosing between (a) adding the
submodule mapping, (b) pointing `vk.h` at `external/mu_lib`, or (c) vendoring
the two headers outright is a repository-structure decision, not a backend one.
`CMakeLists.txt` compounds it by using `${...}/external/mu_lib/mu` for
`mu_gfx` and `${...}/third_party/mu_lib/mu` for `flightsim`/`stickman` — the
same header reached by two roots.

### 2.7 One earlier finding was wrong, and is recorded as such

An intermediate note in this audit claimed `mu_id_pool_is_id()` had inverted
polarity ("returns 1 for free ids"). It does not: it returns 1 for a *live* id
and 0 for a free one, which is what `tests/id_pool.c:54` asserts and what
`mu_id_pool_deinit()`'s `used_ids == 0` precondition requires. The confusion
came from probing the predicate before calling `mu_id_pool_create_id()`.
Verified directly against the implementation; **no change made**.

---

## 2.8 Semaphores are per frame slot, not per swapchain image — sound, with a condition

Asked explicitly during review, so recorded with the reasoning.

`FrameSlot` (indexed by frame slot, `MAX_FRAMES_IN_FLIGHT` of them) owns two
binary semaphores:

| Semaphore | Signalled by | Waited by | Indexed by |
| --- | --- | --- | --- |
| `present_done` | the frame's `vkQueueSubmit` | `vkQueuePresentKHR` | frame slot |
| `acquire_done` | `vkAcquireNextImageKHR` | the frame's `vkQueueSubmit` | frame slot |

The textbook advice to index by *image* comes from `acquire_done`, and the
reason is a specific hazard: the acquire is an operation on the image, not on
the frame slot, so VUID-vkAcquireNextImageKHR-semaphore-01286 requires the
semaphore to be unsignalled. If image N is handed back while the slot that
previously acquired image N is still waiting on its semaphore, the second
acquire signals an already-signalled semaphore.

That cannot happen here **only because the image count never exceeds the frame
slot count**: `set_surface()` asks for `frames_in_flight` images (raised to the
surface minimum, lowered to the surface maximum) and §1.4's fix now *refuses* a
swapchain that returns more than `MAX_FRAMES_IN_FLIGHT`, since
`swapchain_images[]` and `swapchain_views[]` are that size. With
`image_count <= frames_in_flight`, any image acquisition is matched by a slot
that has already completed its wait, so `acquire_done` is reusable.

`present_done` is per frame slot for an independent and simpler reason: it is
signalled by the slot and waited by the slot's own present, and a slot does not
begin its next frame until the timeline semaphore shows its previous submission
complete.

Two consequences worth knowing:

* The backend relies on **timeline-based slot retirement** (`retire()` polls
  `vkGetSemaphoreCounterValue` and resolves frames whose submit value has
  passed) rather than on `vkWaitForPresentKHR` or a per-image fence. That is
  what keeps the design free of `VK_KHR_swapchain_maintenance1` and of an
  image→fence table.
* If `image_count` were ever allowed to exceed `frames_in_flight` — say to
  support a `minImageCount` of 4 with 3 frame slots — `acquire_done` **must**
  become image-indexed in the same change. `test_semaphore_indexing` asserts the
  invariant (`image_count <= frames_in_flight`) that makes the current indexing
  valid, so the assumption is stated rather than implied.

---

## 3. Performance notes (nothing changed)

* **`wait_oldest_frame()` retries a 1-second wait 60 times.** In the normal
  case the first wait returns immediately. The retry exists because a software
  rasterizer can exceed one second. Correct, and the comment says why.
* **`MAX_PIPES 256` × ~100 KB PSO.** The table never compacts, so this is a
  session budget. That is now stated at the definition, and
  `MAX_PIPES_IN_FRAME` turns "compile a pipeline mid-frame" from a hitch into a
  reported error.
* **`plan_barriers()` is O(passes × targets)** with a linear scan per lookup.
  For a 10-pass program over ~20 targets that is a few thousand comparisons per
  *build*, not per frame. Fine.

---

## 4. Verification

New: `tests/vk_invariants.c`, wired into `CMakeLists.txt` as `vk_invariants`.
45 checks, no Vulkan device required — it includes `vk.c` to reach the planner,
so it rebuilds with the file it tests.

```
vk_invariants: 45 checks, 0 failed
```

It covers: cross-frame barrier layouts; a same-frame write→read chain;
a frame-local read-before-write rejected at build time; depth state in
`key_equal`; `pipe_intern_depth_only`; `draw_sort_key` field coverage,
pipe-index dominance, and stability; `PUSH` at exactly `PUSH_MAX` and below it;
`compute_pass()` name truncation; handle sentinels
(`TARGET_IS_SWAPCHAIN`/`HANDLE_MATCHES`/`HANDLE_VALID`); `format_bytes`; and the
barrier table's capacity against the most distinct images one pass can name.

Each test was checked in both directions. The cross-frame barrier test fails
when `barrier_src_layout()` is reverted to return `UNDEFINED`. The capacity test
fails when `MAX_IMAGES_PER_PASS` is reverted to the old size (built with
`-DNDEBUG` so the newly-added assert cannot mask it), reporting
`planned 25 barriers` for 32 named images. A test that cannot fail is not a
test.

Compilation status across the tree (`-Wall -Wextra`, `-fsyntax-only`):
**57 of 58** translation units clean. The one failure,
`demos/flightsim/main.c`, is pre-existing and unrelated: `flight.h:40` includes
`mu_shader_noise.h`, which does not exist in the tree (`git stash` confirms the
failure predates this work). `demos/common/platform.c` needed
`external/glfw` initialised, which is a submodule checkout, not a code issue.

Three demos (`jfa`, `pbr`, `sdfworld`) and `demos/fluid` defined their own
`static void compute_pass(...)` with a different signature from `vk.h`'s, which
was an outright compile break before this work. Each was renamed to a local
name (`compute_image_pass`, `compute_pass_bare`).
