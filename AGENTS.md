# AGENTS.md — rules for writing code in this repository

The C style for this repository is the one in
[`external/mu_lib/AGENTS.md`](external/mu_lib/AGENTS.md). Read it; it applies
here unchanged. The two rules below are repeated here because the demos are
where they are broken most often.

## 1. Do not break code into unnecessary helper functions

Longer functions are often better. A function that reads top to bottom, with
its state in view, is easier to write and to reason about than the same code
spread over five helpers that are each called once.

- Do not extract a helper that is called from one place. Leave the code where
  it runs and give the block a one-line comment as its heading.
- Do not split a function because it is long. Length is not a reason.
- Do not write wrappers that only rename or forward a call.
- Extract a function when it is called from two or more places, when an API
  needs it as a callback (GLFW, `PassDesc.record` in the old demos), or when it is a
  separate transform with its own inputs and outputs.
- When a helper exists only to hide a ladder of checks, the fix is rule 2,
  not the helper.

## 2. Few `if` blocks, few null checks

Checks that cannot fail in a working build make code uglier to read, harder
to write and harder to reason about. Setup code should be straight lines.

- **A failure during setup is fatal where it happens.** The function that
  can fail (shader load, target or buffer creation, pipeline creation)
  prints what failed and exits. Its callers do not test the result. If a
  helper does not do that yet, change the helper, not the call site.
- No `if (!x) return false;` ladders, no `ok = ok && …` chains, no testing
  the same handle again in every function it passes through.
- Do not check what cannot happen: a pointer your own caller passed, a count
  you just computed, `malloc` in a demo or a tool.
- Check where a value enters the program and can really be wrong: command
  line arguments, files, results that vary at run time (a swapchain that is
  out of date). Check once, there.
- Prefer data that removes the branch: a loop over zero items instead of
  `if (count)`, a valid dummy resource instead of an optional one, a table
  instead of an `if` chain.
- State an invariant with `MU_ASSERT` (debug builds), not with a run-time
  `if`.
- The test: if deleting the check changes nothing unless there is a bug or a
  broken install, delete it or make it an assert.

```c
/* WRONG */
vs = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX);
fs = demo_shader(d, SHADERS "scene.frag.spv", STAGE_FRAGMENT);
if (!vs || !fs) return false;
a->color = demo_target(d, "color", FMT_RGBA16F, d->width, d->height, false, &a->color_index);
if (!HANDLE_VALID(a->color)) return false;
a->pipe = pipe_intern_graphics(d->gpu, vs, fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);
if (!HANDLE_VALID(a->pipe)) return false;

/* RIGHT: demo_shader, demo_target and the pipe helper report and exit on failure */
vs       = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX);
fs       = demo_shader(d, SHADERS "scene.frag.spv", STAGE_FRAGMENT);
a->color = demo_target(d, "color", FMT_RGBA16F, d->width, d->height, false, &a->color_index);
a->pipe  = pipe_intern_graphics(d->gpu, vs, fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);
```

Measurements in a demo's `--check` are not checks in this sense; they are
the demo's output.

## Where the existing code stands

`demos/common/demo.c` follows these rules: `demo_shader`, `demo_target`,
`demo_volume`, `demo_alloc`, `demo_upload`, `demo_readback`,
`demo_readback_buffer`, `demo_program` and `demo_write_ppm` print the reason
and exit through `demo_fatal`; none of them returns something to test.
Command-line options are a typed `DemoOption` table in `main`
(`.flag`, `.off`, `.u32`, `.f32` point at the field; range-checked, printed
by `--help`), not an `if` chain. `demo_check` counts failures itself; there
is no `ok` to carry around.

The framework does not call back into a demo, so a demo has no `void *self`
and no casts. Each of the twenty-four framework demos owns its loop:

```c
demo_open(d, options, argc, argv);
app_init(&app);
while (demo_running(d)) {
    if (d->ui) app_ui(&app, d->ui);
    app_frame(&app);
}
if (d->check) app_check(&app);
return demo_close(d);
```

`app_frame(App *a)` is the frame top to bottom: the per-frame constants and
uploads, then `demo_frame_begin`, then every pass in program order as
`pass = demo_pass(d, "Name"); pass_push(pass, ...); pass_draw(pass, ...);`,
then `demo_present` and `demo_frame_end`. There are no record callbacks
(`PassDesc.record` stays NULL); N similar passes are a `for` loop, and a
pass that has nothing to do this frame is still begun with `demo_pass` and
left empty. `--check` code renders by calling `app_frame` and reads with
`demo_read_floats`. A side program that only `--check` runs is recorded with
`frame_begin` / `frame_pass(d->gpu, program, "Name")` / `frame_end`
(`demos/fluid`, `demos/jfa`).

- `demos/lines`, `demos/sdf` and `demos/post` are the pattern to copy: each
  pass is one `PassDesc` literal inside `app_init`, pipes and attachments are
  locals, and `App` holds only what the frame and check functions read.
- The other fifteen record their frames the same way, but their `app_init`
  still fills passes field by field (`p->colors = &a->color;`) from
  attachment fields kept in `App`. Converting those to the literal form is
  open work; when you touch one, convert it.
- `demos/galaxy`, `demos/toon`, `demos/slang-toon*` and
  `demos/common/simple_example.c` predate the framework and are unchanged.

`src/vk.c` reports failure by return value and `gpu_last_error`; that is the
library's contract and stays. The demos' fatal helpers sit on top of it.
