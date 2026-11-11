# Status — the two Vulkan skills

One page, current as of **2026-11-07**. Target machine: RX 9070 XT (Navi 48, GFX12), RADV on Mesa
`27.0.0-devel` @ `ac271714ff58086430a3817554a4b9ac0e5929d9`, Slang `slangc` 2026.21.

## Where they are

| | `vulkan-shader-opt` | `vulkan-opt` |
|---|---|---|
| Job | why a *shader* is slow: ISA, precision, packing, subgroups, scheduling | why a *frame* is slow: passes, layouts, occupancy, sync, geometry |
| References | 16 | 10 (+ `OPTIMIZATION-CATALOG.md`) |
| Words (refs + SKILL + README) | 42,500 | 24,142 |
| Citations (Mesa `ac27171…`, file:line) | 86 bounds-checked, 28/28 quoted anchors verified 2026-11-07; **local clone removed 2026-11-08** | shared by the same checker |
| Self-measuring script | `slang_probe.sh` — 20 probes, 55 assertions | — |
| Triage script | `shader_scan.py` — 35 rules | `vk_scan.py` — 26 rules |
| Bundle | `/home/user/vulkan-shader-opt-bundle.md` — **44,943 w** | `/home/user/vulkan-opt-bundle.md` — **27,356 w** |
| Author rows in `AUDIT.md` | Goldberg, Aaltonen, Themaister, Chips-and-Cheese, López, Chajdas, Wihlidal, Khronos samples | — |

Every claim is one of three things: reproducible from Mesa at the pinned commit (with file:line),
reproducible from `slangc` (with a probe in `assets/slang-probes/`), or labelled
`[third-party measurement]` and kept out of the decision tables.

The 75 MB `/home/user/mesa` source clone was deleted on 2026-11-08 to keep the workspace small.
The citations above were verified against it before deletion; `verify_citations.py` now skips
cleanly without it and re-checks everything when given `--mesa <path>` to a fresh clone.

## Validation, run just now

```
slang_probe.sh   ALL EXPECTATIONS HELD — 55/55 assertions, 20 probes, exit 0
shader_scan.py   2 findings on a planted fixture (atan2, pow) — fires as documented
vk_scan.py       3 findings on a planted fixture (all-commands barrier, 1-wide workgroup,
                 GENERAL layout) — fires as documented, exit 1 on "high"
verify_citations.py  86 citations bounds-checked, 28/28 quoted anchors verified, 0 problem(s)
bundles          44,943 w / 16 refs, 27,356 w / 10 refs, code fences balanced
```

**One script fix this pass.** `slang_probe.sh` used to report ten `CHANGED` lines when `spirv-dis`
was simply absent — the `.spv` files existed, no disassembly was written, and every `grep -c`
returned empty. Missing tooling now exits **2** with `MISSING TOOL` and the sentence "no conclusion
about the reference tables can be drawn", so the failure can no longer be mistaken for a toolchain
regression. Verified both ways: exit 2 with a bare `PATH`, exit 0 / 55 assertions with
`/tmp/tools/bin` and `/tmp/tools/spirv/usr/bin` ahead of it.

## The two corrections that came out of the audit

1. **The register budget was quoted in the wrong wave size.** `03` §3 said GFX12 gives "48 VGPRs/thread
   at 16 waves/SIMD" — true in wave64 terms only; the shipping wave32 budget is **96**. Derivation now
   in the text: 768 wave64-VGPRs × 64 lanes = 49,152 regs = 192 KB per SIMD, ×2 SIMD = 98,304 per CU,
   ÷ 1,024 threads (16 wave32 slots × 32 lanes × 2 SIMD) = **96**. Mesa does this conversion in
   `radv_get_max_waves` (`radv_shader.c:3129-3155`); Chips and Cheese state the silicon figure
   independently. Consequence for the Aaltonen comparison: GCN held **2,048 threads at 32 VGPRs**, GFX12
   holds **1,024 at 96** — half the threads, triple the budget. The earlier "same 2,048 threads" summary
   was wrong and is marked as such in `AUDIT.md` §5o.
2. **The same bad arithmetic had been restated** in `14` §6 as "98,304/2048 = 48". Fixed with the
   correction spelled out, because restatements are what readers actually quote.

## What is closed, and what is deliberately open

**28 of ~30** queue items are integrated or declined in writing — the 28th being the 2023 ACM
survey, closed as a *checklist*: `14` §11 maps all 28 of its technique classes to a section here or to
a written decline, and found exactly one gap (**software prefetching**, now `09` §2 — data prefetch
does not exist on RADV; the instruction-stream form is driver-owned, and ACO's loop retuning is
disabled on GFX12 by a `gfx_level <= GFX11` guard). `references/14-gist-corpus-triage.md`
§3 and §6 now agree on every author row — eight of them still said "queued" after their content had
landed, which is why the file was worth auditing before adding source 28.

Open, on purpose, with reasons recorded in `14`:

- **Khronos, *Optimising a AAA Vulkan Title on Desktop* (2019)** — video only; no transcript obtained,
  and no claim in the skills depends on it.
- **ACM, *Optimization Techniques for GPU Programming* (2023 survey)** — closed as a checklist
  (`14` §11). §7 and the appendices were not read: they re-analyse the same 28 classes, none of it a
  claim about GFX12.
- **Lottes (#14)** — his two used claims arrive via secondary sources; the decks are video-only, so the
  row reads *partial* rather than pretending otherwise.
- **Schreiner (#23)** — dead site, archive-only, conceptual (no measurements). Closed as a negative.

Declined with reasons, not to be re-opened: NVIDIA SER whitepaper, Ada/Blackwell PDFs, GPU Reshape/RGD
(they answer *where*, not *what to change*), the Arm-only fixed-rate-compression numbers (RADV exposes
`VK_EXT_image_compression_control` only below GFX12 — `radv_physical_device.c:241`), and RDNA4 dynamic
VGPR allocation (no `s_alloc_vgpr` in Mesa at all).

## Reading order for someone new

`vulkan-shader-opt`: `SKILL.md` → `01` (measurement discipline) → `02` (ALU/precision/packing) → `03`
(interpolants and registers — includes the corrected budget) → `05` (control flow, wave ops) → `10`
(the `slangc` workflow, so you can check any of it yourself).
`vulkan-opt`: `SKILL.md` → `01` → `02` (Mesa runtime semantics: DCC/HTILE/layouts) → `09` (occupancy,
architecture, memory) → `05` (sync, descriptors, coherence).

Rebuild: `python3 /tmp/mksbundle.py && python3 /tmp/mkvb.py` (generators are session-local; recreate
from the reference lists in `AUDIT.md` if `/tmp` has been cleared).
