# Pipeline explosion — the measured problem, and the fix

One problem, one document. No migration notes: this is the ground-up model, and nothing is
inherited from the current API.

---

## 1. What it costs you today

`src/three_d/scene.c` declares

```c
PipelineID scene_pipe[SCENE_CLASS_COUNT][SCENE_VARIANT_COUNT];   /* scene.c:280 */
```

which is `[4][5]` — 4 shading classes × 5 pass variants — and it fills 19 distinct pipelines:

| | EARLY | LATE | ALPHA_1X | PREZ | EQ |
|---|---|---|---|---|---|
| PBR | ✓ | ✓ | ✓ | ✓ | ✓ |
| TOON | ✓ | ✓ | ✓ | ✓ | ✓ |
| FOLIAGE | ✓ | ✓ | ✓ | ✓ | ✓ |
| FACE | *(= LATE)* | ✓ | ✓ | ✓ | ✓ |
| **used by default?** | yes | yes | yes | **no** (`MU_PREZ` off) | **no** (`MU_PREZ` off) |

Three facts follow, all measurable in the tree:

1. **8 of the 19 are dead by default.** The pre-Z scheme was measured a wash at 678p and is off
   (`renderer.txt`), but its 8 pipelines are still compiled at startup and still occupy the pipeline
   cache.
2. **MSAA multiplies and then rebuilds.** `msaa_scaled` is set on the sky and the EARLY/LATE variants
   (13 sites), so `renderer_set_msaa` runs `pipeline_rebuild_msaa_samples` (`renderer.c:1177`) behind
   `vkDeviceWaitIdle` (`renderer.c:1141`) — a full GPU flush and a batch of pipeline compiles, at the
   moment the user ticks a checkbox.
3. It is not the scene's fault. The same shape repeats at engine level (~11 renderer pipelines + ~7
   in the 2D stage), all hand-listed, all created one call at a time.

**Root cause in one sentence: pass state was encoded in the pipeline key.** EARLY / LATE / EQ differ
only in depth compare and depth write. ALPHA_1X differs only in sample count. If those are pipeline
state, then every pass variant × every shading class is a pipeline. That is the explosion.

---

## 2. What a pipeline still has to encode on this driver

RADV/ACO accepts essentially the whole state vector as **dynamic**. From
`radv_pipeline_graphics.c:217-290` (the `VK_DYNAMIC_STATE_*` → `RADV_DYNAMIC_*` mapping) and the
feature block at `radv_physical_device.c:1230, 1273, 1399-1418`:

| State that used to make a new pipeline | Dynamic in RADV? | Evidence |
|---|---|---|
| depth compare op, depth test, depth write | **yes** | `radv_pipeline_graphics.c:227-232` |
| rasterization samples (MSAA) | **yes** | `:279`, feature `:1412` = `true` |
| colour blend enable / equation / write mask | **yes** | `:277-278`, `:1029-1043` (packed into bits) |
| cull mode, front face, topology | **yes** | `:217-221` |
| vertex input (attributes *and* stride) | **yes** | `:235,251` |
| alpha-to-coverage, sample mask, logic op, colour write enable | **yes** | `:281-290`, feature `:1401-1417` |
| every blend/DS state as a group | **yes** | `RADV_DYNAMIC_CB_STATES` / `_DS_STATES` macros, `:305-316` |
| **shader modules (VS/FS)** | no | — |
| **attachment formats + count** (dynamic rendering) | no | — |
| view / view-mask | no | — |

Note the gate on the blend states: `extendedDynamicState3ColorBlendEnable = !pdev->use_llvm`
(`radv_physical_device.c:1405`) — i.e. available on the ACO path, which is what this machine runs. The
engine must *query* it rather than assume, but on GFX12 + ACO it is there.

So the irreducible key is:

```
PipeKey = { vs, fs, color formats + count, depth format, view mask }
```

Seven of those cover your entire scene. Everything else is an 8-byte `DrawState`.

---

## 3. The fix, in four moves

### M1 — the key is irreducible, and it is interned

```c
typedef struct PipeKey {
    ShaderId vs, fs;
    const char *vs_entry, *fs_entry;
    Format    color[MAX_COLOR_ATTACHMENTS];
    uint8_t   color_count;
    Format    depth;
    uint16_t  view_mask;
    bool      blend_static_off;   /* opt out of dynamic blend, see §5 */
} PipeKey;

Pipe pipe_intern(Gpu *g, const PipeKey *key);   /* same key → same Pipe */
```

The rule that keeps it small: **if RADV can vary it per draw, it is not in the key.** A pipeline
descriptor with twenty fields is twenty axes of a future explosion — that is precisely how the
current table became 19 entries.

### M2 — everything else is an 8-byte per-draw state, shadowed

```c
typedef struct DrawState {
    uint8_t samples, blend, depth_cmp, depth_write, cull, topo;
    uint16_t write_mask;
} DrawState;                      /* _Static_assert(sizeof == 8) */
```

EARLY / LATE / ALPHA_1X / EQ are no longer pipelines; they are three fields:

```c
DrawState early   = draw_state_opaque(SC_4, CULL_BACK);          /* depth GREATER, write on  */
DrawState eq      = draw_state_opaque(SC_1, CULL_BACK); eq.depth_cmp = CMP_EQUAL;
DrawState alpha1x = draw_state_alpha (SC_1, CULL_NONE);          /* blend on, write on       */
```

`pass_state()` is **shadowed**: if the state is already current, it emits nothing. A pass that draws
4,000 instances of one state issues one `vkCmdSet*` burst, not 4,000.

### M3 — sort on (pipe, state), so dynamic state does not become context rolls

Dynamic state is not free on AMD: changing render state mid-stream opens a new context, and the
hardware has **8 context banks, 7 usable** (`vulkan-opt` `03` §8). The defence is the same one the
engine already applies to culling:

```c
static inline uint64_t draw_sort_key(uint32_t pipe_index, DrawState s);  /* pipe << 40 | state */
```

The existing GPU-driven scatter already buckets by group; the group key gains these state bits, so
draws sharing a state become *consecutive*, and one context switch amortises over the whole run. This
is the difference between "dynamic state is cheap" and "dynamic state is a stutter per draw".

### M4 — the set is derived from the frame program, precompiled, and budgeted

```c
FrameProgram *frame_program_build(Gpu *g, const PassDesc *passes, uint32_t count);
```

The program already knows every pass's attachments and its `Pipe`; it collects the distinct keys and
precompiles them **off the frame path**, in parallel, with the pipeline cache persisted. Then:

```c
void pipe_stats(const Gpu *g, uint32_t *pipelines, uint32_t *compiled_this_frame);
#define PIPE_BUDGET 24
```

`compiled_this_frame` should be **0** in every steady-state frame; a non-zero value is reported in
`perf_dump` the way the escape-hatch counter is. A build that pushes `pipelines` past
`PIPE_BUDGET` fails on purpose — a silent jump from 7 to 30 is how the current engine got here.

---

## 4. Result

| Current pipeline | New form |
|---|---|
| EARLY × 4 classes | **4 pipes** (discard-free FS) + `DrawState{samples, GREATER, write}` |
| LATE × 4 classes | **3 pipes** (discard FS; FACE is its own) + same state, later in the frame |
| ALPHA_1X × 4 classes | **reuses LATE's pipes** — only `samples` and `blend` change |
| EQ × 4 classes | **reuses LATE's pipes** — only `depth_cmp = CMP_EQUAL` changes |
| PREZ × 4 classes | **deleted**. It was measured a wash; if revived it is 1 pipe with `color_count = 0`, not 4 |
| MSAA chain rebuilds | **gone** — sample count is dynamic; only the targets are recreated |
| **19 scene pipelines** | **7 scene pipelines** |

And, for the whole engine: pipelines stop being something you maintain by hand. You declare shader
variants; the program tells you which keys exist; the count is assertable.

**What this does not fix.** The remaining 7 are *shader* variants — 4 shading classes × {discard
compiled out, discard present}. That axis is real: a `discard` in the FS costs early-Z, so the pair
must exist (it is what recovered ~25 % of frame shading time in this engine). Keep it at 7 by keeping
the classes honest, and name the variants by capability (`FS_DISCARD_FREE`, `FS_DISCARD`) instead of
by pass, so nobody adds a variant because a pass needed one.

---

## 5. Does dynamic state cost CPU? Yes — and the cost is three orders smaller than the roll

Measured in the driver, not assumed. Three tiers, and only the last one matters:

**Tier 1 — the API call.** RADV's setters are two stores and a dirty-bit OR:

```c
ALWAYS_INLINE static void
radv_cmd_set_depth_compare_op(struct radv_cmd_buffer *cmd_buffer, VkCompareOp op)
{
   state->dynamic.vk.ds.depth.compare_op = op;          /* store  */
   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_COMPARE_OP; /* OR     */
}
```
*(`radv_cmd_buffer.c:587-595`; the blend and samples setters are the same shape at `:466-475` and
`:360-369`.)* Call it 400 times in a frame and you have written ~1,200 words of memory. With
`pass_state()` shadowing, a pass that keeps one state calls the sequence **once**, so the steady-state
count is the number of runs, not the number of draws.

**Tier 2 — emission at the draw.** RADV applies state before the next draw
(`radv_emit_all_graphics_states`, `radv_cmd_buffer.c:14199-14247`) and — this is the part usually
missed — **dedupes by value**:

```c
#define ac_cmdbuf_opt_set_ctx_reg(tracked_regs, reg, reg_enum, value)
   if (!BITSET_TEST(tracked_regs->reg_saved_mask, (reg_enum)) ||
       tracked_regs->reg_value[(reg_enum)] != __value) { ...emit... }
```
*(`ac_cmdbuf.h:515-526`.)* Setting depth state to the value it already has writes **no packet**. So
re-declaring identical state is free in both tiers.

**Tier 3 — the context roll, and this is the real cost.** AMD has 8 context banks, 7 usable
(`vulkan-opt` `03` §8). A draw whose render state differs from the current context starts a new one.
RADV is explicit that this is the coupling to worry about:

> "Index, vertex and streamout buffers don't change context regs. **We assume that any other dirty
> flag causes context rolls.**"
> — `radv_cmd_buffer.c:12478-12480`, in `radv_need_late_scissor_emission`

So the design's cost unit is **the run** — a maximal stretch of draws sharing one `(Pipe, DrawState)` —
not the draw, not the call. Which is exactly what M2 (shadowing) and M3 (sorting) produce.

### The comparison that settles it

A `vkCmdBindPipeline` that changes state **is also a context roll**, and it carries the pipeline object
plus, at build time, a shader compile. So for state that changes at run granularity:

| | pipelines (today) | dynamic state (proposed) |
|---|---|---|
| context rolls per run | 1 | 1 |
| objects to create | 19 | 7 |
| rebuild on MSAA toggle | yes, behind `vkDeviceWaitIdle` | none |
| startup compile | 19 pipelines | 7 pipelines |
| CPU per state change | one bind + dirty bits | ~2 stores + OR |
| fast paths lost | none | blend-off shortcut (§5.1), if any |

Dynamic state does not add a roll that pipelines avoided — it removes objects that pipelines needed.
The one place static state genuinely wins is when the driver can see something you made dynamic and
would otherwise exploit, and there is exactly one such case in RADV (§5.1).

### How to measure it on this machine instead of arguing about it

RADV counts context rolls for you:

```sh
MESA_VK_TRACE=ctxroll ./your_game        # writes /tmp/<proc>_<date>.ctxroll
```
*(option table `radv_instance.c:187-191`, file writer `radv_device.c:868-882`.)*

That is the number to hold the design to: **rolls per frame should be ~the number of (pipe,state) runs
in the frame program**, which for this engine is roughly 8–16. If it grows with the draw count, a state
axis is varying inside a run — the fix is to put that axis in the sort key (split the run) or move it
into push constants and branch in the shader, never to flip it per draw.

`FrameStats` carries both counters (`state_changes`, `state_runs`) so the ratio `draws / state_runs` is
visible every frame without a trace session.

## 5.1 The two honest costs

1. **Dynamic blend can cost the "blend disabled" fast path.** `radv_pipeline_is_blend_enabled()` must
   assume blending is ON when blend state is dynamic (`radv_pipeline_graphics.c:318-330`), because the
   driver cannot see the pipeline's answer. For a class that is *always* opaque, bake it back in:
   `PipeKey.blend_static_off = true` costs one extra key and restores the shortcut. Measure both ways;
   the default is dynamic.
2. **Per-draw state emission.** RADV compacts dynamic blend/write-mask into bitfields
   (`dynamic->color_blend_enable`, `color_write_mask`, `radv_pipeline_graphics.c:1029-1043`) precisely
   so this is cheap — but it is still command-stream work. Mitigations are already in the design:
   shadowing (M2) and sorting (M3). Watch `FrameStats.pushes`, `barriers` and the dirty-state count,
   not just frame time.

One GFX12-specific caveat found while checking: changing depth state marks
`RADV_CMD_DIRTY_GFX12_HIZ_WA_STATE` (`radv_cmd_buffer.c:13909`), so on this chip a pass should keep
its depth state **constant** across all its draws and use separate *passes* for opaque / alpha /
depth-only rather than flipping depth write per draw. That is a pass-design rule, and it is why M2
groups state per pass by default.

---

## 6. Why not shader objects

RADV supports `VK_EXT_shader_object` on ACO (`radv_physical_device.c:936`, `shaderObject = true` at
`:1508`), so it is *available*. It is not the answer here: ESO hands the combination problem to the
driver, which must then compile or look up a pipeline for whatever state you happen to set — you have
moved the explosion, not removed it, and you lose driver-side shortcuts like the blend check above.
Use ESO only if a measurement says it wins. The design above removes the dimension instead.

---

## 7. What this changes in the API

Small and surgical, because the frame program was already the right shape:

| Before | Now |
|---|---|
| `PipeDesc` with 12 enumerated states + `pipe_table_build` | `PipeKey` (4 axes) + `pipe_intern` |
| pipeline created per pass variant (`create_graphics_pipeline` ×19) | 7 keys, interned, precompiled by the program |
| `renderer_set_msaa` → rebuild + `vkDeviceWaitIdle` | `DrawState.samples`, no rebuild |
| variants named after passes (EARLY/LATE/EQ) | states; shader variants named by capability |
| `MAX_PIPELINES 256` and a hand-kept table | `PIPE_BUDGET 24`, asserted, plus `pipe_stats` |

In `src/vk_v2.h` this is §8: `PipeKey`, `pipe_intern`, `DrawState`, `draw_sort_key`, `pass_state`,
`PIPE_BUDGET`, `pipe_stats`. It compiles clean at `-Wall -Wextra -Werror` on C11 and C2x.
