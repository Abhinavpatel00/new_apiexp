# mu_gfx: clean Vulkan library

This sibling repository is the clean Vulkan library tree. The first commit,
`772866d`, preserves the v2 backend as a reviewable baseline. The current work
is the next, breaking redesign: one public `src/vk.h`, one implementation
`src/vk.c`, and a focused smoke test. The game and legacy renderer are not part
of this repository; the two original workspaces remain untouched.

## Dependencies

- **Volk** and **Vulkan-Headers** are vendored under `external/`.
- **`mu_lib`** is pinned as a Git submodule at `external/mu_lib/`.
- **GLFW 3.4** is pinned as a Git submodule at `external/glfw/` for the demos.
- The current implementation uses only `mu_id_pool` from `mu_lib`, for
  bindless descriptor-slot allocation. Resource handle tables remain fixed
  arrays with generations; other containers are not adopted without a workload
  reason.

Volk loads the Vulkan runtime dynamically. The library does not link against
`-lvulkan`; the runtime loader is required when running the program.

## Build and test

```sh
bash build.sh
./build/id_pool_test
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./build/smoke
python3 scripts/section_index.py --check
```

The smoke test is headless and exercises real compute, image upload/readback, buffer readback,
3D texture upload/readback, frame submission, and resource creation on Vulkan 1.3 (Lavapipe in
the sandbox). 3D textures (`TextureDesc.depth` / `TargetDesc.depth` > 1) and indirect compute
dispatch (`pass_dispatch_indirect`) are used by `demos/sdfworld`.
`build.sh` compiles with `-Wall -Wextra -Werror` and links Volk, `mu_id_pool`,
`dl`, and `m`—not `libvulkan`.

## Demos

The executable demos live in `demos/<name>/`, share `demos/common/` (the only
GLFW file, the WSI surface, and the Nuklear backend), and have a windowed mode
plus a headless render-to-PPM mode with a fixed `--time`.

- **`demos/galaxy`** — procedural four-arm spiral galaxy with 120k analytic star
  billboards, HDR target and tone map. Build with `./build_galaxy.sh`; see
  [`demos/galaxy/README.md`](demos/galaxy/README.md).
- **`demos/toon`** — a port of ColinLeung-NiloCat's
  [UnityURPToonLitShaderExample](https://github.com/ColinLeung-NiloCat/UnityURPToonLitShaderExample)
  (MIT) to Slang + this API: cel-shaded directional light, flat indirect term,
  inverted-hull outline pass with FOV/distance-compensated width and a depth
  bias. Build with `./build_toon.sh`; see
  [`demos/toon/README.md`](demos/toon/README.md) for the HLSL→Slang mapping,
  controls, and the deliberate differences (no shadow atlas, no base map).

```sh
./build_toon.sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/toon-cmake/toon --headless toon.ppm --size 1280 720
```

The toon demo is also the reference for the bindless-geometry pattern in
`vk.h` §17: one `ARENA_DEVICE` slice holds every mesh, one upload, and each draw
pushes a 256-byte block and fetches vertices by `SV_VertexID`.

### Framework demos

These forty-two share `demos/common/demo.h`: one argument parser, window or
headless output, a Nuklear panel, a present pass (exposure, tone map, dither),
and a numeric acceptance test behind `--check`. Each README has the pass graph
and the measured numbers.

| Demo | Shows | `--check` asserts |
|---|---|---|
| [`demos/sdf`](demos/sdf/README.md) | sphere-traced SDF scene, soft shadow, AO | hit-pixel count within 1 % of the analytic silhouette |
| [`demos/post`](demos/post/README.md) | five-level bloom, eight tone mappers side by side | bloom chain conserves energy to 1 %; zero above threshold |
| [`demos/shadows`](demos/shadows/README.md) | six shadow filters split-screen, PCSS, three cascades | PCSS penumbra width within 15 % of geometry; no acne with bias, acne without |
| [`demos/particles`](demos/particles/README.md) | compute simulation, compaction, indirect draw | particles rasterized == CPU's integer prediction; two runs bit-identical |
| [`demos/pbr`](demos/pbr/README.md) | metallic/roughness sphere grid, baked octahedral IBL, clearcoat, sheen | DFG table within 0.01 of a brute-force integral; white furnace 1 ± 0.02 |
| [`demos/deferred`](demos/deferred/README.md) | 1 024 point lights, G-buffer (3 targets), clustered light lists built in compute | clustered image bit-identical to lighting with every light; within 1 code value of a forward reference |
| [`demos/clustered`](demos/clustered/README.md) | the same lights shaded forward with Granite-style tile bit masks and z bins, spot lights; `--sdf-shadows`: a shadow ray per light through a distance volume of the scene; `--fog X`: fog lit by the same light lists through a froxel volume | bit-identical to shading with every light; masks read back and checked against the CPU; shadows: no light leaks and < 0.1 % falsely dark pixels against the exact distance function; fog: closed form to 0.001, within 1–2 % of a per-pixel march under the moon, 5–15 % off under 1 024 small lamps |
| [`demos/sdfworld`](demos/sdfworld/README.md) | a deformable world: band-limited distance volume edited in compute (digger, builder, falling stones and clay, pointer), coarse empty-space level, cone-trace pre-pass, soft shadows and AO from the field | an edit changes nothing outside its box and matches the formula on the CPU; coarse level equals a CPU rebuild; pre-pass never starts a ray beyond its plain hit; 2.2–4.9× fewer steps |
| [`demos/fluid`](demos/fluid/README.md) | 2D stable fluids in compute on a staggered grid: advection, pressure solve (red-black over-relaxation, in place), projection; two dyed jets in a closed box | the divergence and projection passes equal the same sums on the CPU; outflow left after projection (32 % of what came in at 40 passes, 0.3 % if the solver keeps going), nothing through the walls |
| [`demos/jfa`](demos/jfa/README.md) | signed distances to an arbitrary drawn shape by jump flooding, rebuilt every frame; four orders of steps | against the true closest cell found on the CPU, eight shapes: 1+JFA wrong in under 0.02 % of answers and by under 1 cell (plain JFA: up to 12 cells, reported) |
| [`demos/gpu_driven`](demos/gpu_driven/README.md) | 4 097 boxes culled in compute (frustum, then two-phase occlusion against a depth pyramid) and drawn through indirect commands | culled frame bit-identical to drawing everything at twelve cameras incl. cuts; GPU frustum set == CPU's; every box that shows a pixel is in a draw list once |
| [`demos/oit`](demos/oit/README.md) | transparency without sorting (weighted blended), next to depth peeling and to ordinary blending in draw order | coverage and single layers equal the peeled reference to 0.001; with 2+ layers the colour is off by 0.01–0.03 on average (up to 0.19 on a pixel); result independent of draw order |
| [`demos/taa`](demos/taa/README.md) | temporal anti-aliasing: jittered projection, velocity target, history clamped to the neighbourhood | against 64 samples per pixel: error 0.048 → 0.013 standing still; velocity verified by fetching the previous frame; where a moving ball has just been it is worse than no anti-aliasing (0.031 vs 0.019) |
| [`demos/clay`](demos/clay/README.md) | 4 096 clay particles (position-based dynamics on a grid, compute) rebuilt into a distance volume every frame and sphere-traced | volume equals a CPU brute force over all voxels to 0.03 mm; no overlap deeper than a tenth of a diameter; nothing leaves the tray or enters the paddle; a run repeats bit for bit |
| [`demos/bricks`](demos/bricks/README.md) | a distance volume stored sparsely: 8³ bricks near surfaces in an atlas behind an indirection volume, built in compute with an indirect dispatch, sphere-traced | every stored texel identical to the dense volume; no empty brick claims more room than there is, over all samples; 253³ in 19 MB instead of 124 MB, at a third more steps and twice the fetches per ray |
| [`demos/screenspace`](demos/screenspace/README.md) | ambient occlusion and reflections from the depth buffer, next to 256 samples of the same estimator and to the scene really mirrored in the floor | a bare floor stays exactly unoccluded; 16 samples + blur within 0.004 of 256; screen space finds 95–97 % of the true reflection at eye level, 83 % from above |
| [`demos/ocean`](demos/ocean/README.md) | an ocean patch from a Phillips spectrum: inverse FFT as 14 compute passes, choppy displacement, grid drawn from the vertex index | one wave matches its closed form at two times to 4e-7 m; the full field matches a direct Fourier sum on the CPU to 3e-7 m at 64² and 512² |
| [`demos/lines`](demos/lines/README.md) | toon outlines two ways: inverted hull with a width in pixels, and edges found in depth + normals | hull width on a ball within 0.07 px of the 2, 3, 5 px asked at two distances; edge lines 0.90 × asked (the search is along the axes); no false lines on a floor at a low angle, 29 % with a direct depth comparison |
| [`demos/blackhole`](demos/blackhole/README.md) | a Schwarzschild black hole with a thin disc: one light ray per pixel bent by the exact orbit equation, Doppler and gravitational shift on the disc | shadow size within 0.5 % of Synge's formula from three distances; leaving directions within 4e-6 rad of the polar orbit equation on the CPU; far deflection within 0.3 % of 4M/b plus its second-order term |
| [`demos/turing`](demos/turing/README.md) | reaction and diffusion (Gray–Scott) in compute: spots, mazes, coral and dividing cells from 28 seed patches, on a grid that tiles | one step identical to the CPU to 6e-8; decay of two Fourier waves under the stencil within 1e-5 of its eigenvalue after 384 steps; the pattern's own spacing measured at 17 cells |
| [`demos/schrodinger`](demos/schrodinger/README.md) | the Schrödinger equation for one particle in compute (Visscher's leapfrog, fourth-order stencil): a wave packet, a wall with two slits and a plate that adds up what arrives | five bands with two slits and none with one; bands within 2 % of where the path difference puts them; probability kept to 2e-8 over 1 201 steps; speed within 1.5e-5 of the grid's group velocity; spreading within 0.1 % |
| [`demos/mpm`](demos/mpm/README.md) | water and jelly with the moving least squares material point method (Hu et al., SIGGRAPH 2018) in compute: particles add to a fixed-point grid with atomic adds | grid mass and momentum equal the particles' to 4e-8 and 3e-6; one step matches the CPU to 4 integer units on the grid and exactly on the particles; a falling brick within 1.6e-5 of free fall; a run repeats bit for bit |
| [`demos/pat`](demos/pat/README.md) | signed distance to a point cloud by *Points as Tori* (Feng, Gkioulekas, Crane, TOG 2026): the authors' transformer in plain C gives every point a torus, 32 of them are blended per query; ray marched with offsets and Booleans. Explained in [`docs/points-as-tori.md`](docs/points-as-tori.md) | the C network matches the authors' code to 2e-6; contact of order 3; error 5 to 9 times below tangent planes on three known shapes; GPU equals CPU to 1e-6; volume-started and pointwise rays agree on every pixel |
| [`demos/snow`](demos/snow/README.md) | snow by the material point method of Stomakhin et al. (SIGGRAPH 2013) in three dimensions: cubic B-splines, fixed corotated energy with hardening, singular values clamped, PIC/FLIP, friction, a moving collider, sticky walls; explicit step in compute, the implicit solve of the technical document on the CPU. Explained in [`docs/snow-mpm.md`](docs/snow-mpm.md) | the technical document's derivatives match finite differences to 1e-10; one GPU step equals the CPU's to rounding; free fall within 4e-5; the implicit step holds at 15 times the step where the explicit one blows up |
| [`demos/xpbd`](demos/xpbd/README.md) | XPBD (Macklin et al. 2016) on the CPU: length and volume constraints with compliance, a multiplier per constraint, damping; cloth over a ball, chains, a soft block; `--pbd` runs the 2007 method for comparison. Explained in [`docs/xpbd.md`](docs/xpbd.md) | a chain's length is right to 1e-6 with 20 iterations and to 4e-12 with 80, where PBD goes rigid; the multiplier gives the top link's force to 2e-11; a swinging chain is within 1.3 mm of implicit Euler by Newton |
| [`demos/projdyn`](demos/projdyn/README.md) | Projective Dynamics (Bouaziz et al., SIGGRAPH 2014) on the CPU: local projections of each tetrahedron (strain, strain limits, volume), one prefactored global solve. Explained in [`docs/projdyn.md`](docs/projdyn.md) | both projections are nearest points to 1e-8; the objective never rises; 1000 iterations reach implicit Euler to 2e-13; a hanging bar stretches as the formula says to 1e-4; stable at a 0.5 s time step |
| [`demos/seiler`](demos/seiler/README.md) | Seiler's interpolation (Yuksel, SIGGRAPH Talks 2024): a cubic Bézier curve from three lerps, degrees 2 to 5, and a uniform Catmull-Rom spline stored as one knot and one Seiler point a sample. Explained in [`docs/seiler.md`](docs/seiler.md) | all forms agree with the Bernstein polynomials to 6e-16 for degrees 2 to 5; the Seiler point at a Catmull-Rom knot is shared by both pieces to 2e-16 |
| [`demos/avbd`](demos/avbd/README.md) | Augmented Vertex Block Descent (Giles, Diaz, Yuksel, SIGGRAPH 2025) for rigid boxes in the plane, after the authors' reference demo: multipliers for hard constraints, clamped forces for contact and Coulomb friction, stiffness ramp, safe Hessian, post-stabilisation, warm start; `--vbd` is plain VBD. Explained in [`docs/avbd.md`](docs/avbd.md) | springs with a stiffness ratio of 10 000: right to 0.14 % with 5 iterations where plain VBD sags 2.3 m; sliding matches Coulomb's law to 5e-6; a stack of ten rests at 3e-15 m/s |
| [`demos/absnewton`](demos/absnewton/README.md) | "Stabler Neo-Hookean Simulation: Absolute Eigenvalue Filtering for Projected Newton" (Chen et al., SIGGRAPH 2024): stable Neo-Hookean tetrahedra, projected Newton with each element's eigenvalues clamped (red) or made absolute (green), side by side. Explained in [`docs/absnewton.md`](docs/absnewton.md) | the paper's eq. 8, 9, 11 to the printed digits; eq. 14 to 19 to 2e-12; at Poisson 0.499 absolute needs 9 iterations, clamped 15; with only the end moved on a coarse bar clamped is quicker (11 against 16) |
| [`demos/waveparticles`](demos/waveparticles/README.md) | "Wave Particles" (Yuksel, House, Keyser, SIGGRAPH 2007), section 3: bumps on particles that move in straight lines, subdivision by dispersion angle from a heap of known times, walls by folding, the horizontal motion of the extended height field; rain on a pool, a wake, straight waves. No floating objects. Explained in [`docs/waveparticles.md`](docs/waveparticles.md) | 7 steps and 240 steps give the same surface to 6e-14; volume kept to 2e-8; a wall equals a mirror image to 1e-15; the paper's 3 % for the front's shape measures 7 % |
| [`demos/mergesplit`](demos/mergesplit/README.md) | "Particle Merging-and-Splitting" (Truong, Yuksel et al., TVCG 2021), sections 3 and 5: colliding particles become one meta-particle for a time step and are split again with the energy the merge took, two stages a step for resting contact, a limit on the size of a meta-particle; a ball on an elastic sheet, loose particles on it, a box of discs. Discs in the plane, explicit steps; no coupling of two integrators, fluids or fracture. Explained in [`docs/mergesplit.md`](docs/mergesplit.md). | a free pair splits as the textbook elastic collision to 9e-14; eq. 8, 9, 14 hold for any meta-particle velocity; energy of 150 discs unchanged to 2e-16 after 1861 merges; a ball 1, 10, 100 times heavier than the sheet never gets through (a well-chosen penalty holds it too, impulses do not); a resting particle does not sink; a limit of 2 lets 165 of 192 resting particles through, 64 none |
| [`demos/ipbf`](demos/ipbf/README.md) | "Implicit Position-Based Fluids" (Diaz, Hsu, Montalvo-Ruiz, Giles, Yuksel, SIGGRAPH Asia 2025), Algorithm 1 in the plane: the density constraint as an energy, one 2×2 Newton step a particle with half of it applied, the diagonal approximation of the Hessian, the damping; two columns that meet, a block at seven times the rest density, a column at rest; Position Based Fluids beside it. On the CPU, 1600 particles; explained in [`docs/ipbf.md`](docs/ipbf.md) |
| [`demos/sampleelim`](demos/sampleelim/README.md) | "Sample Elimination for Generating Poisson Disk Sample Sets" (Yuksel, Eurographics 2015) in the plane: weights from neighbours within 2 r_max, a heap, the heaviest sample goes; weight limiting, the ordering for progressive use, a density map; the power spectrum beside the samples; dart throwing to compare. Explained in [`docs/sampleelim.md`](docs/sampleelim.md) |
| [`demos/gogreen`](demos/gogreen/README.md) | "Go Green: General Regularized Green's Functions for Elasticity" (Chen and Desbrun, SIGGRAPH 2022): the displacement around a load in an anisotropic elastic body as a sum of spherical harmonics × radial functions × material matrices; grab, scale, twist, pinch and handles on a grid, for an isotropic, a cubic and a fibre material; checked against Kelvin's solution, a direct integral and the elasticity equation. Explained in [`docs/gogreen.md`](docs/gogreen.md) |
| [`demos/c2spline`](demos/c2spline/README.md) | "A Class of C2 Interpolating Splines" (Yuksel, ACM TOG 2020): curves through control points with a continuous second derivative, each piece a cos² / sin² blend of two curves through three points; the Bézier, circular, elliptical and hybrid functions side by side with curvature lines; the check measures the continuity, the paper's distance bounds, self-crossings, circles and local support. Explained in [`docs/c2spline.md`](docs/c2spline.md) |
| [`demos/catmullrom`](demos/catmullrom/README.md) | "On the Parameterization of Catmull-Rom Curves" (Yuksel, Schaefer, Keyser 2009): the same control polygon with uniform, centripetal and chordal parameter values and those between; the check measures over random control points that only the centripetal one never goes backward, loops or leaves its strip, and the distance bounds of eq. 6 and 8. Explained in [`docs/catmullrom.md`](docs/catmullrom.md) |
| [`demos/polyroots`](demos/polyroots/README.md) | "High-Performance Polynomial Root Finding for Graphics" (Yuksel 2022): three thick cubic curves drawn by the roots of a degree 5 polynomial for every cell, with the polynomial of one probe point and its monotonic pieces shown; the check repeats the paper's test on random Bernstein polynomials (root counts against table 1, every root against a long double reference) and its failure rates of plain Newton iterations. Explained in [`docs/polyroots.md`](docs/polyroots.md) |
| [`demos/pointlight`](demos/pointlight/README.md) | "Point Light Attenuation Without Singularity" (Yuksel 2020): a room lit by virtual point lights, left with 1 / d² and its bright spots, right with the paper's bounded function; the check reads the shader's functions back, compares them with a disc's solid angle and with a real glowing ball, and measures the spots and the loss of the printed formula in single precision. Explained in [`docs/pointlight.md`](docs/pointlight.md) |
| [`demos/wildmesh`](demos/wildmesh/README.md) | "Simplifying Textured Triangle Meshes in the Wild" (Liu, Zhang, Yuksel 2025), in 3-D: a town of loose parts, a triangle soup and a thin two-coloured cloth, each shown as input, simplified by the plane quadric alone, and by the paper's method (virtual edges between near triangles, area term worked out anew, colours through the map back along the collapses); the check compares parts left, ground kept, distances, and wrong-side colours. Explained in [`docs/wildmesh.md`](docs/wildmesh.md) |
| [`demos/softloco`](demos/softloco/README.md) | "Soft Body Locomotion" (Tan, Turk, Liu 2012), in 3-D: soft bodies with no skeleton that reach, crawl and roll by muscles; each step chooses the muscle lengths together with what every contact does (stay, slide, let go) by the paper's search over quadratic programmes, shown beside the one programme with static contacts. `--check` compares the programme with steps really taken, the search with all 10⁴ programmes, and what each scene achieves. Explained in [`docs/softloco.md`](docs/softloco.md) |
| [`demos/contact`](demos/contact/README.md) | "Contact and Friction Simulation for Computer Graphics" (Andrews, Erleben, Ferguson; SIGGRAPH 2022 course), its rigid-body thread in 3-D: sphere and box contacts, friction as a box, as Coulomb's cone and as an ellipsoid with direction-dependent and spinning friction, solved by Jacobi, Gauss–Seidel and NNCG with Moreau's step and post-step projection. `--check` compares contacts with brute force, friction with closed forms, the solvers with each other and with what the notes say of them. Explained in [`docs/contact.md`](docs/contact.md) |
| [`demos/koopman`](demos/koopman/README.md) | "Low-Rank Koopman Deformables with Log-Linear Time Integration" (Chang, Chen, Grinspun, Chiaramonte; arXiv 2602.07687), the numerical variant, in 3-D: a Neo-Hookean finger is simulated with implicit Euler, one linear operator is fitted to 208 of its steps (dynamic mode decomposition, written out) and kept as 20 complex eigenvalues and modes; N steps at once are N-th powers of those. Scenes: step size against implicit Euler, pushes the fit never saw, damping by scaling eigenvalues, control of the tip. The neural variant of the paper is not built. Explained in [`docs/koopman.md`](docs/koopman.md) |

```sh
cmake -S . -B build/cm -DCMAKE_BUILD_TYPE=Release && cmake --build build/cm -j
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json    # software Vulkan
./build/cm/shadows                                # window
./build/cm/shadows --headless out.ppm             # one image
./build/cm/shadows --calibration --check          # acceptance test
scripts/check_demos.sh                            # every demo: image vs tests/reference, every --check
```

`scripts/check_demos.sh` takes about four minutes on lavapipe. `--update` rewrites
the references; `--no-check` compares images only.

The remaining entry of [`docs/demo-roadmap.md`](docs/demo-roadmap.md), the
toon merge, is not built.

### Shader-package demos

Two further demos are vendored Vulkan/Slang shader packages. Each keeps its
original fixed-binding sources for reference and adds a **bindless variant**
under `bindless/` that uses the `vk.h` shader ABI: sampled-image and sampler
arrays at set 0 bindings 0/2, the single storage buffer at binding 3, and a
48-byte push block of byte offsets. The compiled bindless stages are checked in
under `spirv/bindless/`.

- **`demos/slang-toon`** — the tutorial port: `toon.slang` with eight stages
  (`fragTutorial`/`fragTutorialOutline` plus the independently designed
  `fragExtended`/`fragExtendedOutline`), `MATH.md`, `test_math.py`, the
  MIT attribution for the upstream URP tutorial, and `bindless/BINDLESS.md`.
- **`demos/slang-toon-inspired`** — the extended port: the same eight stages
  plus `inspired.slang` and four `fragInspired*` stages (face SDF, hair shadow,
  GGX specular, MatCap, screen-space rim, dissolve), with `INSPIRED-MATH.md`,
  `TUTORIAL-PORT.md`, and its own bindless ABI note.

The packages still document the fixed pipelines, bindings, uniform layouts,
and required renderer-generated inputs (linear depth, hair mask). The bindless
variants are self-contained for the forward/outline passes: every optional map
has a neutral fallback, so `fragTutorial` and `fragInspired` render with no
textures bound.

`slang_toon_host` is the reference host for the bindless stages. It renders the
`demos/toon` character scene with a shadow-map pass, a forward pass and an
inverted-hull outline pass, exercising the storage-buffer/push layout from
`bindless/BINDLESS.md` (ABI v2: per-object data, shadow caster stage,
pixel-width outlines, smoothed outline normals, style block). It opens a window
by default; `--headless` writes a PPM:

```sh
cmake --build build/toon-cmake --target slang_toon_host
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/toon-cmake/slang_toon_host --headless slang_toon.ppm
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/toon-cmake/slang_toon_host --headless slang_toon_inspired.ppm --model inspired
./build/toon-cmake/slang_toon_host --headless before.ppm --v1   # same scene, v2 features off
```

Rebuild the Slang sources and run the CPU equation checks with:

```sh
SLANGC=/path/to/slangc bash build_slang_toon.sh
# or, with the local research SDK:
SLANGC=../toonresearch/slang-sdk/bin/slangc bash build_slang_toon.sh
```

CMake exposes the same rebuilds as the optional `slang_toon_shaders` and
`slang_toon_inspired_shaders` targets when `slangc` is on `PATH` or passed as
`-DSLANGC=/path/to/slangc`. `slang_toon_host` is part of the normal build.

## Shaders

Every shader is Slang; there is no GLSL in the tree. `scripts/build_shaders.sh`
holds the manifest (source, entry point, stage, output), validates with
`spirv-val`, and `--check` fails if a checked-in `.spv` is stale. The SPIR-V is
checked in, so `slangc` is only needed after editing a shader:

```sh
SLANGC=/path/to/slangc scripts/build_shaders.sh          # or: toon | galaxy | common | slang-toon
SLANGC=/path/to/slangc scripts/build_shaders.sh --check
```

See [`docs/slang-shaders.md`](docs/slang-shaders.md) for the porting rules and
the image comparison against the former GLSL build.

## Documents

- [`docs/nilo-toon-study.md`](docs/nilo-toon-study.md) — how the upstream URP
  toon shader works, where its code and comments disagree, and what was taken
  from it.
- [`docs/slang-shaders.md`](docs/slang-shaders.md) — the GLSL → Slang conversion.
- [`docs/demo-roadmap.md`](docs/demo-roadmap.md) — proposed further demos.

## Research

`graphics_finding/` contains the retained graphics research and skills.
`graphics_finding/mu_lib-review.md` records the upstream guidance review;
`graphics_finding/docs/refactor-decisions.md` records the agreed API scope and
container choices; `graphics_finding/docs/api-trim-followup.md` records the
API-trimming decisions, including bindless geometry and timing diagnostics.
The `mu_lib` Slang shader functions remain available via the submodule, but are
not copied into the C Vulkan API.
