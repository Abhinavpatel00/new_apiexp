# vulkan-opt — a Mesa-grounded Vulkan optimization skill

A drop-in skill for a coding agent (Claude/Codex/Cursor/any "skills"-capable runner) that
turns "make my renderer faster" into a measured, driver-aware procedure instead of folklore.

It is the practical extraction of a 12-part study of the Vulkan/Mesa graphics pipeline: the
driver-level mechanisms (compression, fast clear, binning, resolve, culling, primgroups,
occupancy) that decide whether an API call takes the fast path, plus the engine-side
techniques that only pay when those mechanisms are active.

Grounded in **Mesa 27.0.0-devel**, commit `ac271714ff58086430a3817554a4b9ac0e5929d9`
(`src/amd/**`). Every "the driver does X" claim in `references/02` and `references/07` maps to
a named function in that checkout.

## Layout

```
vulkan-opt/
├── SKILL.md                          entry point: triggers, cost model, workflow, decision
│                                     tables, do-not list, ROI ranking, output contract
├── OPTIMIZATION-CATALOG.md           ~60 optimizations: saves / Mesa logic / action /
│                                     verify / when it fails
├── references/
│   ├── 01-measure.md                 attribution: timestamps, counters, budgets A–E
│   ├── 02-mesa-runtime-semantics.md  the core: compression, fast clear, load/store,
│   │                                 binning, resolve, culling, VRS, barriers, waves
│   ├── 03-passes-and-bandwidth.md    passes, clears, overdraw, prepass math, G-buffer,
│   │                                 MSAA/resolve, half-res, shadows, sky/transparency
│   ├── 04-geometry-and-shading.md    vertex fetch, small draws, culling, mesh shaders,
│   │                                 quad overdraw, occupancy, subgroup, precision
│   ├── 05-sync-descriptors-memory.md barriers, layouts, queues, descriptors, memory
│   ├── 06-temporal-and-post.md       AA choice, TAA/velocity, motion blur, tonemap/gamma/UI
│   ├── 07-mesa-source-index.md       file:line map of every emitter and decision point
│   └── 08-known-traps.md             symptom → cause → confirmation
├── assets/
│   ├── report-template.md            the deliverable format
│   ├── pass-budget.md                worksheet to fill per frame
│   └── findings-log.md               append-only log of verified findings
└── scripts/
    └── vk_scan.py                    heuristic anti-pattern scanner (no deps)
```

## Install

**Claude Code / Agent Skills layout** — copy the directory so the agent can discover it:

```
<project>/.claude/skills/vulkan-opt/SKILL.md          (plus references/, assets/, scripts/)
```

**Single-file agents** (system prompt or a "custom instructions" box) — paste
`vulkan-opt-bundle.md`, which ships next to this skill directory. It is the same content
concatenated into one Markdown file; use it when the agent cannot read files on demand.

**As a repo doc** — commit the directory, then point your agent at `SKILL.md` whenever a
performance task appears.

## How an agent should use it

1. Read `SKILL.md` (it is self-contained for the workflow).
2. Do Step 1 (measure) before anything else. `assets/pass-budget.md` is the worksheet.
3. When forming a hypothesis, open `references/02` to predict what the driver will do.
4. Pick candidates from `OPTIMIZATION-CATALOG.md` filtered by the bottleneck budget letter.
5. Run `scripts/vk_scan.py <src>` for an API-level triage list.
6. Emit the report in `assets/report-template.md` format and append to `findings-log.md`.

Progressive disclosure is deliberate: `SKILL.md` is the only file always loaded; the rest is
read on demand, so the skill stays cheap in context.

## Scanner

```bash
python3 scripts/vk_scan.py path/to/engine              # human-readable, ranked
python3 scripts/vk_scan.py path/to/engine --json       # machine-readable
python3 scripts/vk_scan.py path/to/engine --top 10
```

Exit code is 1 if any high/critical finding exists (usable in CI). It is triage, not a
verdict: every finding still needs a measurement.

## Scope and honesty

- **AMD/RADV-centric**, because the grounding is Mesa's AMD driver. Most structural advice
  (passes, bandwidth, overdraw, sync, descriptors) is vendor-neutral; the compression,
  binning and VRS specifics are AMD-flavoured. Annotate NVIDIA/Intel-specific expectations
  rather than pretending they are the same.
- **Numeric thresholds** (overdraw ratio ~1.6, prepass gates, "under ~100 primitives per draw
  is small") are heuristics to *order* experiments, not laws. The skill forbids shipping a
  change without a measurement precisely because of this.
- **Line numbers drift.** Every entry names a function; search the function if the line moved.

## Extending

- New optimization → add a row to `OPTIMIZATION-CATALOG.md` with all five columns filled.
- New mechanism → add the function to `references/07` and, if it gates behaviour, a section to
  `references/02`.
- New anti-pattern → add a rule to `RULES` in `scripts/vk_scan.py` (severity + reference is
  mandatory).
- Keep the reference numbering stable; other files link to it.

## Companion document

`/home/user/vulkan-barriers-and-begin-rendering-2026.md` is the long-form, source-verified companion
to `references/05`: barriers and `vkCmdBeginRendering` traced from `vkCmdPipelineBarrier2` down to
`ACQUIRE_MEM`, `PIPE_CONTROL` and NVK methods on RADV, ANV and NVK. Read it when a task turns on
exact barrier semantics, layout transitions, or load/store-op behaviour; the skill itself only
carries the conclusions.

`references/10` is self-contained: its derivation is included so it can be checked without the GDC
deck it quotes.

`/home/user/gtc-s3466-2012-kepler-optimization-2026-audit.md` is the companion to
`references/09`: a section-by-section audit of a hardware-architecture talk (GTC 2012, Kepler) that
re-derives each rule for 2026 silicon and marks which ones no longer hold. Read it when a task turns
on *why* an occupancy or coalescing rule exists, or when someone quotes a pre-Vulkan rule at you.

