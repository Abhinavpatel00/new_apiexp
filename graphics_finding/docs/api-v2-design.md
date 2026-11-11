# API v2 — a performance-first design for `mu_gfx`

Target: RX 9070 XT (Navi 48, GFX12), RADV/Mesa `27.0.0-devel`, Linux
Companion to `docs/core-rewrite-plan.md` (rev 2). This supersedes its "API frozen" assumption.

---

## 0. The shift

Rev 2 was a refactor: same API, better structure. What you are asking for now is different — the API
itself should be the thing that makes the engine fast, so that performance is a property of the shapes
available to you rather than of remembering to do the right thing at each call site.

The organising principle:

> **The API has no slow verbs.** Every operation that has a measured, hardware-specific cost on this
> GPU either does not exist in the public surface, or exists in a form where the fast path is the only
> path.

That is testable, and it is how I would judge the design: if any function on the public surface can be
called in a way that is 2× slower than another call to the same function, it is designed wrong.

The rest of this document is the design, the numbers it rests on, and the migration.

---

## 1. The hardware model the API encodes

Every rule below is either quoted from Mesa in the skills or measured by the project itself.

| Fact | Number | Source | Consequence for the API |
|---|---|---|---|
| Register file per SIMD | 192 KB ⇒ **96 VGPRs/thread at full occupancy** (1,024 threads/CU) | `radv_shader.c:3129-3155`, `ac_gpu_info.c:250,292` | shaders are budgeted at 96 VGPRs; the API must expose per-pipeline VGPR count so a regression is visible at build time |
| Instruction cache | **32 KiB per WGP** | `ac_binary.c:157-160` | code size is a first-order shader budget; ACO's loop-prefetch retune is `gfx_level <= GFX11` only, so it does not run here (`aco_assembler.cpp:1750-1752`) |
| Push constants | **256 bytes** | `radv_constants.h:26`, `radv_physical_device.c:1841` | per-draw state is push constants, not UBOs, not descriptor writes |
| Storage buffer range | `UINT32_MAX`, offset alignment **4** | `radv_physical_device.c:1840,1902` | whole-pool buffers + 4-byte offsets; no per-object buffer objects |
| `GENERAL` layout on a compute/transfer queue | **not DCC-compressed** | `vulkan-opt` `02` §2.2 | layouts are derived, never chosen by callers |
| Fast clear | only in an attachment layout | `02` §2.1 | clears are `LOAD_OP_CLEAR`, never `vkCmdClear*Image` |
| Feedback loop (`ATTACHMENT_FEEDBACK_LOOP`) | **never** compressed | `02` §2.2 | read-while-writing the same image is rejected at frame-build time |
| Atomics | bypass L1 | `vulkan-opt` `05` §1 | compute passes that atomics must declare a write range, not a read |
| Context rolls | 8 banks, 7 usable | `vulkan-opt` `03` §8 | state changes are grouped by the API's draw ordering, not by the caller |
| Cull vs draw cost | cull 0.06 ms vs draws 0.47 ms (PC) | Wihlidal, `vulkan-opt` `04` §3 | GPU-driven culling is the default path; CPU-side draw counts are not in the API |
| Command buffer / pass overhead | one submission per frame | project's own `vk_frame_submit` | kept, and made the only way to submit |

**Frame budget reference** (the numbers the design is sized against):

```
1440p RGBA16F  = 2560 × 1440 × 8 B = 29.49 MB      one read+write = 59.0 MB
1080p RGBA16F  = 1920 × 1080 × 8 B = 16.59 MB      one read+write = 33.2 MB
644.6 GB/s     ⇒ a 1440p read+write of one HDR target = 91 µs
                  a 1080p read+write of one HDR target = 51 µs
```

So a full-screen HDR pass costs **~0.09 ms in pure bandwidth before it shades anything** — at 1440p,
eleven such passes are the entire 8.3 ms half of a 120 Hz frame. This is why the API must make pass
count and load/store discipline structural rather than stylistic. §5 turns this into a per-pass budget
the engine prints.

---

## 2. The type system

### 2.1 Handles: generation-checked, no pointers

```c
typedef struct Buffer  { uint32_t index, gen; } Buffer;
typedef struct Texture { uint32_t index, gen; } Texture;
typedef struct Target  { uint32_t index, gen; } Target;   /* render target / transient */
typedef struct Pipe    { uint32_t index, gen; } Pipe;
#define BUFFER_NIL  ((Buffer){0, 0})
```

Eight bytes, passed by value, no pool-pointer instability, and a stale id is *detected* instead of
aliasing a live resource — the exact bug class the current `RenderTargetId` comment warns about
(`vk.h:444`: "a stale id cannot alias a live target"). In debug builds every dereference validates the
generation; in release it is two loads away from a pool slot.

This replaces `TextureID`, `RenderTargetId`, `PipelineID`, `ShaderId`, `SamplerID` — five id
namespaces with five different validity rules become one with one rule.

### 2.2 Resources have a *class*, and the class decides the memory

```c
typedef enum ResClass {
    RES_PERSISTENT,   /* lives until destroyed; GPU may read it next frame */
    RES_FRAME,        /* born and killed inside one frame; the engine may alias it */
    RES_READBACK,     /* GPU writes, CPU reads later; must survive N frames in flight */
} ResClass;
```

`RES_FRAME` is the new thing, and it is where the money is: a transient target's memory is recycled
within the frame's arena, so the depth buffer, the HDR chain, the bloom mips, the SSAO target and the
SMAA intermediates can share address ranges whose lifetimes do not overlap. The engine computes the
aliasing; the caller only says `RES_FRAME`.

**Honest scope of the win:** aliasing saves *footprint*, not bandwidth. The bandwidth wins come from
§2.3 (load/store), §2.2's DCC-by-construction layouts and §4.2 (no queue stalls). Do not budget
aliasing as a frame-time win — budget it as a memory-ceiling win, which matters when you add
higher-resolution targets or a fourth G-buffer.

### 2.3 Attachments: intent, not layout, and load/store is mandatory

```c
typedef enum AttUse {
    ATT_COLOR_WRITE,       /* fully overwritten → LOAD_OP_DONT_CARE, no fetch */
    ATT_COLOR_WRITE_KEEP,  /* partially written → LOAD_OP_LOAD */
    ATT_COLOR_CLEAR,       /* LOAD_OP_CLEAR (fast-clear eligible) */
    ATT_DEPTH_WRITE,
    ATT_DEPTH_CLEAR,
    ATT_DEPTH_READ,        /* sampled as depth, or tested without writing */
    ATT_RESOLVE_SRC,
} AttUse;
```

There is **no `VkImageLayout` in the public API and no way to name `GENERAL`**. The pass builder maps
intent → layout from the table in §6, which is the DCC/fast-clear table from the research. If a pass
genuinely needs storage-image read-write, it asks for `ATT_STORAGE_RW` and the builder knows it costs
compression, records that in the frame's diagnostics, and moves on.

`ATT_COLOR_WRITE` (`DONT_CARE`) is the default because in this engine almost every target is fully
overwritten each frame — and today's code mostly uses `LOAD_CLEAR` or `LOAD_KEEP` where `DONT_CARE`
would skip a full target fetch (29.5 MB at 1440p, 45 µs). That single default is worth more than the
aliasing work.

### 2.4 Pipelines: created in tables, never in a frame

```c
typedef struct PipeTable PipeTable;   /* named, declarative, built at load / on config change */
PipeTable *pipe_table_build(Gpu *g, const PipeDesc *descs, uint32_t count);
Pipe       pipe_get(const PipeTable *t, PipeName n);
```

The API has no `create_graphics_pipeline` callable from frame code. Pipeline creation, reflection and
the pipeline cache live behind a table that is rebuilt when *configuration* changes (MSAA on/off, toon
on/off) — which is what the current `pipeline_rebuild` already does, minus the ability to create a
pipeline mid-frame.

### 2.5 The shader ABI is one sentence

> **One bindless descriptor set, bound once per frame, plus ≤256 bytes of push constants per draw.**

```c
/* set 0, binding 0..3: sampled images[], storage images[], samplers[], storage buffers[] */
/* push constants: the per-draw struct, ≤256 B, mirrored in Slang via `[[vk::push_constant]]` */
```

Nothing else may be bound. This kills per-draw descriptor writes (15 `vkUpdateDescriptorSets` sites
today, most at init but the pattern invites more), keeps the descriptor hot in the cache, and gives a
per-draw state change a fixed cost: one push-constant write, no allocator, no set.

---

## 3. The frame model

### 3.1 A pass declares what it touches; the engine derives everything else

```c
typedef struct PassDesc2 {
    const char          *name;        /* profiler label + perf_report row */
    const AttachDesc    *colors;      uint32_t color_count;
    const AttachDesc    *depth;
    const BufUse        *buf_reads;   uint32_t buf_read_count;
    const BufUse        *buf_writes;  uint32_t buf_write_count;
    Span32              shader_stage_reads;   /* sampled/storage reads */
    Pipe                pipe;         /* 0 = the pass binds it (UI passes) */
} PassDesc2;

typedef struct BufUse { Buffer buf; uint64_t offset, size; Access2 access; } BufUse;
```

This is your existing `PassDesc`/`BufferAccess` shape — it works, and it is the single best thing in
the current API (byte-range declarations, one flush point, `pass_serial` dedup). v2 keeps the *semantics*
and changes three things:

1. **Ranges are mandatory and checked.** `BufUse` has no "whole buffer" shorthand in the fast path; a
   pass that says `size = 0` is a build error, not a 512 MB `ALL_COMMANDS` barrier later.
2. **Attachments carry intent, not layout** (§2.3).
3. **The pass list is declared before it runs** (§3.2), so barriers can be planned across passes
   instead of per pass.

### 3.2 The frame is compiled once per configuration, then replayed

Today `renderer_frame` branches per frame on `two_d`, `msaa`, `smaa_enabled`, `post.flags` and
`bloom` — but the *set of passes* changes only when one of those settings changes. So the pass list is
not dynamic; it is **static per configuration**. That is the leverage:

```c
typedef struct FrameProgram FrameProgram;   /* resolved: pass list + barrier plan + transient map */

/* Built when configuration changes (MSAA toggle, SMAA toggle, post flags, resolution).  */
FrameProgram *frame_program_build(Gpu *g, const PassDesc2 *passes, uint32_t count);

/* Replayed every frame. Records the recorded pass callbacks, inserts the planned barriers,
   resets the transient arena, submits with the frame's timeline value. */
void frame_program_run(Gpu *g, FrameProgram *p, const PassRun *run);
```

What the compile step buys, concretely:

- **Barrier merging across passes.** Two consecutive compute passes writing disjoint ranges, or a pass
  that samples what the previous pass wrote, currently each pay a flush. Planned together, the
  producer→consumer transition happens once and the same-pass duplicate stays free.
- **Layout round-trip elimination.** If A writes in `GENERAL` and B samples it, the transition to
  `SHADER_READ_ONLY_OPTIMAL` is planned once; if C then writes it again next frame, that becomes a
  single transition back, not a per-pass dance.
- **Redundant-load elimination.** A transient that is written before it is read anywhere in the program
  gets `DONT_CARE` automatically instead of by the caller's diligence.
- **Aliasing** (§2.2) becomes computable at all, because lifetimes are known.
- **Static validation**: a pass that reads a target nobody wrote, a feedback loop, an attachment whose
  sample count disagrees with the frame's — all become build errors with a pass name, instead of
  validation-layer messages at runtime.
- **Cost preview**: the program knows every attachment's format, extent, load/store and pass order, so
  it can print the bandwidth floor per pass (§5) before you ever run the frame.

The cost is one new concept. It is not a general render graph — no dynamic graph building per frame, no
automatic pass culling, no topological reordering. It is a **precompiled pass list with a planner**.
That is the 80 % for a fraction of the complexity, and it is available precisely because your frame is
static per configuration.

### 3.3 Submission: one per frame, and the API has no queue

```c
void frame_submit(Gpu *g);            /* acquires, records, submits, presents, retires */
```

No `vkQueueSubmit`, `vkQueueWaitIdle`, `vkDeviceWaitIdle`, no fence argument, no semaphore in the
public API. Frames in flight, the timeline value, the delete queue's retirement and present are the
engine's business. Today's `vk_end_one_time_cmd` (submit + `vkQueueWaitIdle`, `helpers.h:233`) is
exactly the function this design removes the possibility of — which is the biggest measured cost in the
current tree (§4.2).

---

## 4. Memory and transfer

### 4.1 Three arenas, no `vkAllocateMemory` in user code

| Arena | Contents | Notes |
|---|---|---|
| `ARENA_DEVICE` | persistent buffers, textures, pools; **one big storage buffer** for structured data | `maxStorageBufferRange = UINT32_MAX`, `minStorageBufferOffsetAlignment = 4` (`radv_physical_device.c:1840,1902`) — so suballocate freely and hand out `{buffer, offset, size}` slices |
| `ARENA_TRANSIENT` | `RES_FRAME` targets and scratch | bump allocator, reset per frame, aliased by the frame program |
| `ARENA_STAGING` | uploads | ring buffer, retired by timeline value, never waited on |

### 4.2 Uploads cannot block, by construction

```c
Upload *upload_begin(Gpu *g);                 /* opens the transfer batch */
void    upload_buffer(Upload *u, Buffer dst, uint64_t off, ByteSpan src);
void    upload_texture(Upload *u, Texture dst, const UploadRegion *r, ByteSpan src);
void    upload_end(Gpu *g, Upload *u);        /* records, submits once, retires by timeline */
```

No wait, no fence, no per-asset round trip. The staging memory for a batch is retired when the timeline
value passes — the mechanism already exists in `DeleteQueue` + `timeline_last_submitted`. `≤64 KB`
uploads take `vkCmdUpdateBuffer` and never touch staging at all.

Measured motivation: `vk_end_one_time_cmd` waits the queue **per one-shot command buffer**, and
`create_texture` calls the upload path — so today N textures = N queue stalls during load and any
runtime streaming.

### 4.3 Shader-visible data: one buffer, one push constant

Structured data lives in one device-local storage buffer (`gpu_base_addr` already exists in
`VkBackend`); passes address it by 4-byte-aligned offset. Per-draw state rides push constants (≤256 B,
`radv_constants.h:26`). There is no UBO-per-frame-object anymore — a 256-byte push constant is a
register write, and on a 96-VGPR budget the difference between a UBO load and a push constant is an
`lgkmcnt` wait you do not pay.

---

## 5. The budget the API enforces

Because the frame program knows every attachment's extent, format and load/store, it can compute the
bandwidth floor of each pass and of the frame:

```
pass_floor_bytes = Σ attachments ( extent × bytes_per_pixel × (load ? 1 : 0 + store ? 1 : 0) )
frame_floor_ms   = Σ pass_floor_bytes / 644.6 GB/s
```

At 1440p, a full-screen RGBA16F pass that loads and stores is 59.0 MB → **91 µs**. The engine prints
that next to the measured per-pass time in `perf_report.txt`, so every pass has two numbers: *what the
hardware must move* and *what it took*. A pass measured at 3× its floor is ALU- or latency-bound; a
pass at 1.05× is finished, and no amount of shader work will help. That is the single most useful
diagnostic you can put in front of yourself, and it needs the frame program to exist.

Budgets to hold the design to (1440p, 60 Hz = 16.67 ms):

| Item | Budget | Why |
|---|---|---|
| Full-screen HDR passes that load and store | ≤ 6 | 6 × 91 µs = 0.55 ms floor, plus shading |
| Layout transitions per frame | ≤ 12 | each `GENERAL`↔`SHADER_RO` can decompress/recompress |
| Queue waits per frame | **0** | §4.2 |
| Pipeline creations per frame | **0** | §2.4 |
| Descriptor writes per frame | **0** | §2.5 — bindless is written at resource create |
| Draw state changes | one push constant per draw | §4.3 |
| VGPRs per pipeline | ≤ 96 (full occupancy) | §1 |
| Frame-loop CPU time | ≤ 2 ms at 1440p | leaves the rest to the GPU; measured already by `cpu_active_ns` |

---

## 6. The ban list, with the reason attached

Each of these is a public API *absence*, and each has a citation or a measurement behind it.

| Not in v2 | What it forced today | Why the ban is right |
|---|---|---|
| `VkImageLayout` of any kind | callers choosing `GENERAL` in 4 real sites; `GENERAL` on a compute queue is not DCC-compressed | `vulkan-opt` `02` §2.2 |
| `vkCmdClear*Image` | clears outside an attachment layout, which is not fast-clear eligible | `02` §2.1 |
| `vkCmdPipelineBarrier` / `ALL_COMMANDS` | over-wide barriers, timeline signal at `ALL_COMMANDS` (`vk.c:3908,3919`) | `vulkan-opt` `05` §1 — the barrier *is* the mechanism on AMD; it must be narrow |
| `vkQueueSubmit` / `vkQueueWaitIdle` / fences | per-asset upload stalls (`helpers.h:233`) | measured in this tree |
| `vkUpdateDescriptorSets` from frame code | invites per-draw sets | bindless (§2.5) |
| `vkCreateGraphicsPipelines` from frame code | pipeline creation can land mid-frame | §2.4 |
| `vkCmdDraw` with CPU-computed counts | defeats culling; Wihlidal's cull is 0.06 ms vs 0.47 ms of draws | `vulkan-opt` `04` §3 |
| Real-time MSAA as a default | 4× shades the same pixels four times; your own 1× alpha route recovered ~25 % of frame shading | project measurement, `renderer.txt` |
| Read-while-write on one target | `ATTACHMENT_FEEDBACK_LOOP` is never compressed | `02` §2.2 |
| Whole-buffer barriers | byte ranges are already the model; keep it mandatory | `vulkan-opt` `05` §1 |

Anything you genuinely need beyond this stays available as an **escape hatch**, but named and counted:
`gpu_unsafe_native(g)` returns the raw `VkDevice` in debug builds only, and each use is recorded in
`perf_report.txt` so the escape does not quietly become the path.

---

## 7. Migration

The four files stay four files (per rev 2): `vk.c`, `vk.h`, `renderer.c`, `renderer.h`, organized in the
ten sections from the rewrite plan. API v2 lives in the same two, under section banners
`11. API V2 — frame program`, `12. API V2 — memory`, …

| Old | New | Notes |
|---|---|---|
| `begin_pass` / `end_pass` + `PassDesc` | `PassDesc2` in a `FrameProgram` | same declaration spirit, now planned as a program |
| `RenderTargetId` (u32) | `Target` (index+gen) | stale-id bugs become assertions |
| `rt_transition*` / `flush_barriers` | *gone* — derived (§3) | 18 call sites of `flush_barriers` in the tree disappear |
| `create_texture` + implicit one-shot upload | `texture_create` + `upload_*` batch | §4.2 |
| `create_graphics_pipeline` | `pipe_table_build` / `pipe_get` | §2.4 |
| `buffer_pool_alloc` (6 sites) | `arena_alloc(ARENA_DEVICE, size, align)` | same idea, one arena type, 4-byte alignment guaranteed |
| `wait_idle` | debug/shutdown only, named | §3.3 |
| `PassDesc.samples` recomputation (`renderer_scene_sample_count`) | frame program owns sample count | mismatched-attachment class of bug goes away |
| `renderer.h` public surface | **unchanged** | demos and `main.c` keep working; porting happens inside the engine first |

Port order: (1) `vk.c` gains the v2 sections alongside the old API, (2) `renderer.c` ports its 17
pass sites to a `FrameProgram`, (3) `scene.c` (30 pass sites) and the indirect-draw recording, (4)
`two_d`, `nuklear_pass.inl`, `demo/*`. Each step runs the full demo suite; the old API is deleted only
when nothing calls it.

**Honest estimate:** v2 is a rewrite of the frame layer (~2,500 lines of the 7,000 in scope) plus
mechanical porting of ~50 pass sites. Weeks of evenings, not a weekend. The five highest-value items,
if you want to start with effects rather than architecture:

1. `ATT_COLOR_WRITE` → `DONT_CARE` default (removes a full target fetch per pass, 45 µs at 1440p).
2. Upload batching (§4.2) — removes every per-asset queue stall.
3. Derived layouts (§2.3/§6) — removes the `GENERAL` compression loss.
4. Per-pass bandwidth floor printed next to measured time (§5).
5. The three `atan2` sites and two `pow` sites in the shaders (§4 of the rewrite plan).

---

## 8. Where I would push back on my own design

- **The frame program is the only genuinely new concept.** If it proves annoying in practice, the
  fallback costs little: keep `PassDesc2` and the intent-based attachments, and execute passes
  immediately with a per-pass flush as today. You would lose barrier merging, aliasing and the cost
  preview, but not the layout, upload, ban-list or budget wins. Design the types so this fallback stays
  possible — do not put planner assumptions into `PassDesc2`.
- **Aliasing is the least valuable item here** relative to its risk (a wrong lifetime is a
  heisenbug). Ship it last, or not at all.
- **256-byte push constants are a real ceiling** on per-draw state. If a material needs more, the answer
  is a row in the device buffer addressed by push-constant offset — not a UBO per draw.
- **None of this fixes a shading-bound frame.** The measured facts here are about passes, layouts,
  transfers and state — the shader budgets (96 VGPRs, 32 KiB I$, 109-instruction `atan2`) are the other
  half, and they live in the shaders, not the API.

Say which way you want to go and I will start: either **S0 + the five effect-first items** on the
current tree (nothing architectural moves, measurable in days), or **the v2 types in `vk.h` first** so
the shape is real before any call site changes.
