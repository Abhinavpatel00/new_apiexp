# Roadmap: further graphics demos

A proposal. Nothing in this file is implemented; the order is the order I would
build in, and each demo names the `vk.h` feature it is the first to exercise,
because that is where the work and the risk are.

## Where things stand

- Three demos: `galaxy` (instanced analytic billboards, HDR, tone map), `toon`
  (forward + inverted hull, UI), `slang_toon_host` (shadow map, per-object
  data, three shading models).
- All shaders are Slang, built by one script (`docs/slang-shaders.md`).
- `external/mu_lib/mu/shaders/` is a 14-module Slang library (lighting, shadows,
  noise, sdf, post, quantize, …) that **no demo uses yet**. It compiles from a
  demo shader today with `#include "shaders/<module>.slang"` — the build script
  already passes `-I external/mu_lib/mu` (checked with shadows, post, sdf,
  lighting and noise together). Most demos below are that library plus a pass
  graph.
- Paths in `vk.h` no demo has touched: compute passes in a frame, storage
  images, `pass_draw_indirect` / `_count`, MSAA + `attach_resolve`, multiple
  colour attachments, comparison samplers, mip-chain render targets.

## Status

| # | Item | State | Where |
|---|---|---|---|
| 0 | capture + retry loop | done, as part of the demo framework | `demos/common/demo.c` (`demo_readback`, `demo_write_ppm`) |
| 0 | `scripts/check_demos.sh` + references | done | `scripts/check_demos.sh`, `tests/reference/` |
| 0 | Nuklear clipping | done — by fragment discard, `vk.h` has no scissor call | `demos/common/nk_vk.c`, `nk.slang` |
| 0 | shared shader helpers | partly: vertex pull and matrices (`mu_mesh.slang`), depth/comparison tables (`mu_depth.slang`), integer textures (`mu_uint.slang`). No shared `FrameData`: each demo's differs | `demos/common/shaders/` |
| 0 | validation layers | done: `toon` went from 17 messages to 0; every new demo is clean headless and windowed | `src/vk.c` |
| 0 | `pipe_intern_depth_only` | done | `src/vk.c` |
| 1 | sdf | done | `demos/sdf/` |
| 2 | post | done, as a standalone scene instead of on top of `galaxy` | `demos/post/` |
| 3 | shadows | done; VSM/ESM (`mu_vsm`, `mu_esm`) not included | `demos/shadows/` |
| 4 | pbr | done; octahedral maps, one target per roughness; no post pass beyond `Present` | `demos/pbr/` |
| 5 | particles | done; no bloom pass, see its README | `demos/particles/` |
| 6 | deferred | done; 3-target G-buffer (12 B) instead of the packed 8 B word, reversed depth; no "active cluster" pass | `demos/deferred/` |
| 6b | clustered | done (not in the original plan): Granite-style tile bit masks + z bins, forward, spot lights; shadows for every light from a baked distance volume added later (`--sdf-shadows`), then froxel fog lit by the same light lists (`--fog`, no temporal filtering); no subgroup scalarization | `demos/clustered/` |
| 6c | sdfworld | done (not in the original plan): Claybook-style deformable distance volume, one demo; over-relaxed stepping added later; no particle clay, no sparse storage | `demos/sdfworld/` |
| 6d | fluid | done (not in the original plan): stable fluids on a staggered grid, red-black over-relaxed pressure solve | `demos/fluid/` |
| 6e | jfa | done (not in the original plan): jump flooding in 2D, four step orders measured against brute force | `demos/jfa/` |
| 7 | gpu_driven | done: compute frustum + two-phase occlusion culling, depth pyramid, indirect draws; one mesh, `pass_draw_indirect_count` not used | `demos/gpu_driven/` |
| 7b | oit | done (not in the original plan): weighted blended transparency measured against a 12-layer depth-peeled reference; Accum and Reveal are two geometry passes (one blend state per pass) | `demos/oit/` |
| 7c | taa | done (not in the original plan): the plainest temporal anti-aliasing (jitter, velocity, neighbourhood clamp), measured against 64 samples per pixel; no history rejection, no variance clamp | `demos/taa/` |
| 7d | clay | done (not in the original plan): particle clay (position-based dynamics, compute) baked into a distance volume each frame; no static friction, no coarse level for the rays | `demos/clay/` |
| 7e | bricks | done (not in the original plan): sparse distance volume (brick atlas + indirection volume), static scene, checked texel by texel against the dense volume; not used by sdfworld or clay | `demos/bricks/` |
| 7f | screenspace | done (not in the original plan): SSAO against a 256-sample run, screen-space reflections against a planar mirror pass; no temporal filtering, reflections on the floor only | `demos/screenspace/` |
| 7g | ocean | done (not in the original plan): FFT ocean (Tessendorf), radix-2 inverse FFT in compute checked against a direct Fourier sum; one patch, no cascades | `demos/ocean/` |
| 8 | toon v2 merge | **partly**: the outline comparison (hull vs screen-space edges, widths measured) exists as its own demo. The merge of `demos/toon` and `demos/slang-toon` into one host and MSAA are **not started** | `demos/lines/` |
| 9a | blackhole | done (not in the original plan): light rays round a Schwarzschild black hole with a thin disc, checked against the shadow formula, the polar orbit equation and Einstein's deflection; no rotation, camera at rest | `demos/blackhole/` |
| 9b | turing | done (not in the original plan): Gray–Scott reaction and diffusion in compute, one step and the stencil's eigenvalues checked; 256² only | `demos/turing/` |
| 9c | schrodinger | done (not in the original plan): one quantum particle and a double slit, Visscher's scheme in compute, checked against the fringe positions, conservation of probability, group velocity and spreading; plain damping at the border | `demos/schrodinger/` |
| 9d | mpm | done (not in the original plan): 2-D moving least squares material point method, water and neo-Hookean jelly, fixed-point grid with atomic adds, checked against a CPU step, conservation sums and free fall; one density, no separation between materials | `demos/mpm/` |
| 9e | pat | done (not in the original plan): Points as Tori (TOG 2026) with the released weights: network, torus fit and blend in C and in Slang, baked volume plus pointwise ray marching, offsets, Booleans; training and the paper's other applications are not done | `demos/pat/`, `docs/points-as-tori.md` |
| 9f | snow | done (not in the original plan): 3-D material point method for snow (SIGGRAPH 2013): explicit step on the GPU; implicit solve with the technical document's derivatives on the CPU, checked by finite differences; a ball that moves, sticky walls; no volume rendering | `demos/snow/`, `docs/snow-mpm.md` |
| 9g | xpbd | done (not in the original plan): XPBD (MiG 2016) on the CPU, drawn through the shared canvas: compliance, multipliers, damping, PBD for comparison, checked against the analytic stretch and implicit Euler by Newton; Gauss-Seidel only, no finite elements | `demos/xpbd/`, `docs/xpbd.md` |
| 9h | projdyn | done (not in the original plan): Projective Dynamics (SIGGRAPH 2014) on the CPU for tetrahedra: strain, strain limiting, volume, attachments; dense Cholesky, so small meshes only; no cloth, bending or collisions | `demos/projdyn/`, `docs/projdyn.md` |
| 9i | seiler | done (not in the original plan): Seiler's interpolation for curves of degree 2 to 5 on the CPU; the texture-filter hardware of the HPG 2021 paper is not done | `demos/seiler/`, `docs/seiler.md` |
| 9j | avbd | done (not in the original plan): Augmented Vertex Block Descent in 2-D for boxes, joints, springs, contacts with friction, on the CPU; no 3-D, no GPU colouring, no soft bodies | `demos/avbd/`, `docs/avbd.md` |
| 9k | absnewton | done (not in the original plan): absolute eigenvalue filtering for projected Newton on stable Neo-Hookean tetrahedra, dense solver, quasi-static; the paper's failures of clamping are not reproduced at this size | `demos/absnewton/`, `docs/absnewton.md` |
| 9l | waveparticles | done (not in the original plan): wave particles, section 3 and the height field of section 5, on the CPU; section 4 (objects in the water) is not done | `demos/waveparticles/`, `docs/waveparticles.md` |
| 9m | mergesplit | done (not in the original plan): particle merging-and-splitting for discs in the plane with explicit steps; sections 3 and 5 of the paper, not the coupling of section 4 | `demos/mergesplit/`, `docs/mergesplit.md` |
| 9n | ipbf | done (not in the original plan): implicit position-based fluids in the plane on the CPU with position based fluids as a baseline; no viscosity, no GPU | `demos/ipbf/`, `docs/ipbf.md` |
| 9o | sampleelim | done (not in the original plan): weighted sample elimination in the plane with weight limiting, progressive ordering and a density map; no higher dimensions or surfaces | `demos/sampleelim/`, `docs/sampleelim.md` |
| 9p | gogreen | done (not in the original plan): regularized Green's functions of anisotropic elasticity by spherical harmonics, brushes on a planar grid; no spline editing of the radial functions, no 3-D model | `demos/gogreen/`, `docs/gogreen.md` |
| 9q | c2spline | done (not in the original plan): C2 interpolating splines with the four interpolation functions, closed curves in the plane; no 3-D points, no open curves | `demos/c2spline/`, `docs/c2spline.md` |
| 9r | catmullrom | done (not in the original plan): parameterizations of cubic Catmull-Rom curves between uniform and chordal, the paper's theorems measured; planar closed curves only | `demos/catmullrom/`, `docs/catmullrom.md` |
| 9s | polyroots | done (not in the original plan): real roots of polynomials in an interval by monotonic pieces and Newton iterations kept in a shrinking interval, used for the nearest point of cubic curves; without the paper's cubic fast path, deflation and infinite intervals | `demos/polyroots/`, `docs/polyroots.md` |
| 9t | pointlight | done (not in the original plan): point light attenuation with a radius (bounded at distance 0) against inverse square, on virtual point lights and lights placed near surfaces; one bounce, diffuse, every light for every pixel | `demos/pointlight/`, `docs/pointlight.md` |
| 9u | wildmesh | done (not in the original plan): edge-collapse simplification for meshes of loose parts: virtual edges between near triangles, plane quadric summed and area quadric anew, colour through the map back along the collapses; own meshes, no textures baked, no probabilistic quadrics | `demos/wildmesh/`, `docs/wildmesh.md` |
| 9v | softloco | done (not in the original plan): muscle-driven soft bodies (corotational FEM, implicit step); per step a search over contact cases, each a quadratic programme in the muscle lengths (Goldfarb-Idnani); reach, crawl and roll against the static-contact programme; checked against steps really taken and against all 10^4 programmes on small states; contacts are nodes not patches, no balance / jump / walk | `demos/softloco/`, `docs/softloco.md` |
| 9w | contact | done (not in the original plan): rigid spheres and boxes; contacts by clamping and by 15 axes with clipping; box and ellipsoid friction sets through one projection; Jacobi, Gauss-Seidel, NNCG; warm start; Moreau step; post-step projection; checked against brute force and closed forms; the rollback rule and the order result came out weaker than the notes say; no meshes, soft bodies, pivoting or Newton solvers | `demos/contact/`, `docs/contact.md` |
| 9x | koopman | done (not in the original plan): stable Neo-Hookean tetrahedra with implicit Euler as the source of data; dynamic mode decomposition on [u; h v] (QR, Jacobi SVD, complex QR eigenvalues, no library); forces as kicks; N steps at once, fractional steps, the closed sum for a held force, the real operator; damping by eigenvalues; least-squares control of the tip. No neural variant, no shape optimisation, no video | `demos/koopman/`, `docs/koopman.md` |

Verified on lavapipe only. No hardware GPU was available.

Found on the way, still open:

- `vk.c` clamps the swapchain image array to `MAX_FRAMES_IN_FLIGHT` (3); a
  surface that hands out more images would index past it.
- `vk.h` has no scissor/viewport call and no rasterizer depth bias. The demos
  work around both; `toon v2` would be simpler with them. (Buffer readback
  was missing too and has been added: `readback_request_buffer`, used by
  `particles --check` and ready for `gpu_driven`'s draw counts.)
- A pass callback (`PassDesc.record`, run by `frame_run`) receives no pass
  index, so N similar passes needed N functions. `frame_pass` was added for
  that: the caller begins each pass in program order and records it inline,
  so N similar passes are a loop. The twenty-four framework demos use it and
  have no callbacks.
- `MU_POISSON16` in `mu_lib` covers a square, not a disc
  (`demos/shadows/README.md`).
- `mu_multi_scatter_fresnel` in `mu_lib` has the wrong denominator: a white
  metal reflects up to 1.94 x the light it receives (`demos/pbr/README.md`).
- `vk.h` has no cube targets and no per-mip render or storage writes; `pbr`
  uses octahedral maps and one target per level, `post` one target per bloom
  level, `gpu_driven` one per depth pyramid level.
- `vk.h` has 3D textures and 3D storage targets since `sdfworld` was ported
  to them (a 3D target cannot be a colour attachment or have layers).
  `pass_dispatch_indirect` existed before that, unused and untested; the
  `sdfworld` check now covers it. `pass_draw_indirect_count` is still unused.
- The storage-image table is one descriptor array that shaders declare with
  a format. `fluid` declares it twice (rgba16f, rgba32f) and `gpu_driven`
  with r32f; nothing checks that a slot holds the format a shader reads it
  as.
- `mu_pack_nrm_rough_metal` in `mu_lib` overwrites half of the normal it
  packs; the round trip is off by up to 2 (`demos/deferred/README.md`).
- Leaving a windowed demo through `--frames N` destroys a semaphore the queue
  still uses (`VUID-vkDestroySemaphore-semaphore-05149`, seen with `shadows`
  and `deferred`). Closing the window normally was not checked for it.
- `--perf` reports 0.05 ms for a compute pass doing 3.5 million sphere tests
  on lavapipe (`deferred`); compute pass times from it are not trusted.
- Fixed on the way: a frame slower than one second made `frame_begin` fail
  with "timeline wait timed out" (`wait_oldest_frame` in `vk.c`); it now
  keeps waiting for up to a minute.

## Step 0 — shared groundwork (before any new demo)

Small, and every demo below depends on it.

| Item | Why | Found while doing this work |
|---|---|---|
| `demos/common/capture.c`: headless readback → PPM, with the retry loop | three demos each carry their own copy | `readback_ready()` waits 1 s; a cold software rasterizer needs longer, so the first run fails without a retry |
| `scripts/check_demos.sh`: render every demo headless, compare with checked-in references (tolerance 1/255, count threshold) | the Slang port was verified this way by hand; make it repeatable | — |
| Scissor in `demos/common/nk_vk.c` | Nuklear widgets draw outside their panel (the third colour picker in the toon UI spills past the panel bottom) | pre-existing, visible in both GLSL and Slang builds |
| `demos/common/shaders/mu_gfx.slang`: add `FrameData`/camera struct, depth linearize, the vertex-pull helper | each demo redefines them | — |
| Run `--validate` with the Khronos layers installed | not available where this work was done; `demos/toon/README.md` already lists three backend messages | — |
| Depth-only pipe helper (`pipe_intern_depth_only`) | `pipe_intern_graphics` always declares one colour format; a shadow pass needs a hand-built `PipeKey` | hit in `slang_toon_host` |

## Demos

Each one: a windowed mode with a Nuklear panel, a `--headless` capture, a
README with the pass graph, and a numeric acceptance check (not only "looks
right").

### 1. `demos/sdf` — sphere-traced scene

- **Shows:** signed-distance modelling, soft shadows, AO, all in one fullscreen
  pass; then a half-resolution trace + upsample variant.
- **Passes:** `Trace` (fullscreen) → `Resolve`.
- **Library:** `sdf.slang` (primitives, smooth-min, sphere tracing, soft shadow,
  AO), `post.slang` (tone map).
- **First use of:** nothing new — which is the point. It is the cheapest way to
  prove the "demo = mu_lib module + pass graph" recipe and the capture/check
  tooling from step 0.
- **Accept:** analytic check — a sphere of radius r at distance d must cover
  the predicted pixel count ±1 %; step count histogram in the perf file.

### 2. `demos/post` — bloom and tone-mapping lab (on top of `galaxy`)

- **Shows:** Karis-weighted downsample chain, upsample, 8 tone mappers side by
  side, CAS sharpening, dithering.
- **Passes:** scene → 5× `Down` → 5× `Up` → `Composite`.
- **Library:** `post.slang`.
- **First use of:** rendering into the mips of one target (or a target per
  level if that is simpler), many small passes in one frame program.
- **Accept:** energy conservation — sum of the bloom chain equals the sum of the
  thresholded input within 1 %; galaxy without bloom stays pixel-identical.

### 3. `demos/shadows` — shadow filtering lab

- **Shows:** hard / 3×3 PCF / bilinear PCF / Poisson / PCSS / VSM / cascades, as
  a split screen with a slider, plus a bias visualizer (acne vs. peter-panning).
- **Passes:** `Shadow` × cascades → `Forward`.
- **Library:** `shadows.slang`.
- **First use of:** comparison samplers (`SamplerDesc.compare`), several depth
  targets or an atlas with viewports, cascade selection.
- **Reuse:** `vertShadow` and the receiver from `demos/slang-toon`; the toon
  host can then swap its manual 3×3 for `mu` PCF.
- **Accept:** penumbra width at a known blocker distance against the PCSS
  estimate; cascade seams below a pixel-difference threshold.

### 4. `demos/pbr` — metallic/roughness with image-based lighting

- **Shows:** a roughness × metallic sphere grid, clearcoat and sheen rows,
  procedural sky as the environment.
- **Passes:** `SkyToCube` → `Prefilter` (compute, per mip) → `DFG LUT`
  (compute, once) → `Forward` → `Post`.
- **Library:** `lighting.slang`, `brdf_util.slang` (GGX/VNDF sampling).
- **First use of:** **compute passes and storage images** inside a frame
  program; cube or octahedral environment maps.
- **Accept:** white-furnace test — a white sphere in a white environment must
  return 1.0 ± 0.02 at every roughness (multi-scatter compensation on).

### 5. `demos/particles` — GPU particles

- **Shows:** a million curl-noise particles, simulated and drawn without the CPU
  touching them.
- **Passes:** `Simulate` (compute) → `Draw` (indirect, vertex pulling) → `Post`.
- **Library:** `noise.slang` (curl), `wave.slang`/`dispatch.slang` (group
  sizing).
- **First use of:** compute → graphics buffer hand-off (`buf_reads` barriers),
  `pass_draw_indirect`.
- **Accept:** conservation — particle count in equals count drawn (read back the
  indirect args); deterministic frame N for a fixed seed.

### 6. `demos/deferred` — G-buffer and many lights

- **Shows:** 1 000+ point lights with clustered culling; G-buffer debug views.
- **Passes:** `GBuffer` (MRT) → `ClusterLights` (compute) → `Lighting`
  (fullscreen or compute) → `Post`.
- **Library:** `quantize.slang` (octahedral normals, packed G-buffer recipes),
  `lighting.slang`.
- **First use of:** multiple colour attachments, position reconstruction from
  depth, light lists in the storage buffer.
- **Accept:** deferred result equals a brute-force forward loop over the same
  lights within 1/255 on a small scene.

### 7. `demos/gpu_driven` — culling and indirect-count drawing

- **Shows:** 100 k instances, frustum + Hi-Z occlusion culling in compute, one
  `pass_draw_indirect_count`.
- **First use of:** `pass_draw_indirect_count`, a depth pyramid, per-instance
  data pulled by `SV_InstanceID` (note: Slang subtracts the base instance — see
  `docs/slang-shaders.md`).
- **Accept:** culled image equals the unculled image exactly; drawn-instance
  count reported and plausible against a CPU reference cull.

### 8. `demos/toon` v2 — merge the two toon paths, add screen-space lines

- **Shows:** the interactive toon demo running the bindless ABI v2 stages
  (shadow map, style block and smoothed normals on sliders), plus the
  screen-space outline NiloToon uses alongside the hull: depth + normal edge
  detection, compared with the hull side by side.
- **Passes:** `Shadow` → `Depth+Normals prepass` (`vertOutline + fragDepth`,
  `fragNormals`) → `Forward` → `Outline` → `Edges` → `Resolve`.
- **First use of:** MSAA + `attach_resolve` (cel edges and hulls are exactly
  what MSAA is good at), the hull-inclusive depth prepass.
- **Depends on:** 3 (shadow filtering) and step 0.
- **Accept:** `demos/toon`'s current captures reproduced by the tutorial stage
  within tolerance; edge-detect line width measured like the hull width was.

## Order and size

| # | Demo | New `vk.h` surface | Rough size | Depends on |
|---|---|---|---|---|
| 0 | groundwork | — | small | — |
| 1 | sdf | none | small | 0 |
| 2 | post | mip-chain targets | small–medium | 0 |
| 3 | shadows | compare samplers, cascades | medium | 0 |
| 4 | pbr | compute + storage images | medium–large | 2 |
| 5 | particles | compute→draw, indirect | medium | 0 |
| 6 | deferred | MRT, clustered lights | large | 4 |
| 7 | gpu_driven | indirect-count, Hi-Z | large | 5 |
| 8 | toon v2 | MSAA resolve, prepass | medium | 3 |

The first three add no compute and little API risk, and leave the repository
with capture tooling, a post stack and shadow filtering that everything after
them reuses. 4 and 5 are the first compute users and are independent of each
other. 6 and 7 are the large ones and should come last.

## Constraints to design around

- `MAX_PIPES` is 24 and pipelines are interned by key: prefer uniform branches
  and flag bits to shader variants (the upstream toon shader's own rule; see
  `docs/nilo-toon-study.md`). The lab demos (2, 3) should switch technique with
  a push constant, not a pipeline per technique.
- `PUSH_MAX` is 256 bytes: push offsets into the storage buffer, not data
  (`demos/toon` pushes 256 B per draw; the bindless toon pushes 48).
- No depth-bias state: bias in the vertex shader, as `vertShadow` does.
- No shader compilation inside a frame: every technique's pipeline is created
  up front.
- Negative-height viewport: anything rendered and later sampled by projected
  coordinates needs the V flip (`muFlipV`, `TOON_FLAG_SHADOW_FLIP_V`).
- Everything has to run headless on lavapipe for the checks, so compute demos
  need a small default problem size and a `--size`-style scale switch.
