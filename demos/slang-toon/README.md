# Vulkan Slang toon shaders

> **Status in `mu_gfx`:** the fixed-binding sources below are kept for
> reference. The integration path is `bindless/`, which uses the `vk.h` ABI
> (sampled images at set 0 binding 0, samplers at 2, one storage buffer at 3,
> 48-byte push). Its compiled stages live in `spirv/bindless/`, and
> `demos/slang-toon/host.c` is the reference host (see below). See
> [`bindless/BINDLESS.md`](bindless/BINDLESS.md), in particular "ABI v2". Rebuild with
> `SLANGC=/path/to/slangc bash build.sh`, or run
> `SLANGC=/path/to/slangc bash build_slang_toon.sh` from the repository root,
> which also rebuilds the inspired package.

## Reference host

`slang_toon_host` renders the `demos/toon` character scene through the bindless
stages with a shadow-map pass, a forward pass and an inverted-hull outline pass.
`--v1` renders the same scene with every ABI v2 feature switched off.

| `--v1` | default |
|---|---|
| ![v1 feature set](docs/host_before.png) | ![ABI v2](docs/host_after.png) |

```sh
cmake -S . -B build/toon-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/toon-cmake --target slang_toon_host
# from the repository root (shader paths are relative):
./build/toon-cmake/slang_toon_host                                  # window, Esc quits
./build/toon-cmake/slang_toon_host --headless out.ppm               # one frame
./build/toon-cmake/slang_toon_host --headless out.ppm --model inspired
./build/toon-cmake/slang_toon_host --help
```

`--headless` selects offscreen rendering; `--out FILE` only names the file.
Feature switches for A/B captures: `--v1`, `--no-shadows`, `--no-outline`,
`--no-smooth-normals`, `--no-pixel-outline`, `--no-style`, `--point-light`,
`--model tutorial|extended|inspired`, `--camera DIST YAW PITCH`, `--fov DEG`,
`--size W H`, `--outline-width X`.

What the v2 features do. Each image is the same frame with only that feature
off (left half) and on (right half):

| feature | off / on |
|---|---|
| smoothed outline normals: the crate's hull stays closed at corners | ![](docs/cmp_smooth_normals.png) |
| cast-shadow band (`styleParams.y`): shadow edges get the cel edge instead of a PCF staircase | ![](docs/cmp_shadow_edge.png) |
| style block: HSV shadow tint, band anti-aliasing, rim on the lit side only | ![](docs/cmp_style.png) |

### Verified on lavapipe

Mesa llvmpipe (software Vulkan, LLVM 19.1.7), Slang 2026.21, `spirv-val --target-env
vulkan1.1` on every stage:

- v1 compatibility: the v1 host against the v2 SPIR-V gives 0 differing pixels
  versus the v1 SPIR-V, for `fragExtended` and `fragInspired`.
- All three models render the scene (`docs/host_tutorial.png`,
  `host_after.png`, `host_inspired.png`); the windowed path was run under Xvfb.
- Pixel-width outlines measure 1.92–2.08 px across distance 3.5–13, fov 25–90°
  and 360p–1080p, where the tutorial formula gives 1.33–5.13 px
  (table in `bindless/BINDLESS.md`).
- `python3 test_math.py`: 18 CPU checks of the equations.

Not verified: a hardware GPU, the Vulkan validation layers (not installed in
the environment the work was done in), and the fixed-binding stages, which
still have no host.


This package contains **two models in one Slang source**:

1. `fragTutorial` / `fragTutorialOutline`: a renderer-adapted port of the lighting and outline logic in ColinLeung-NiloCat's public *SimpleURPToonLitOutlineExample*. This is **not** a port of the full NiloToonURP product.
2. `fragExtended` / `fragExtendedOutline`: an independently designed extension adding three diffuse tones, a painted face-threshold map, stylized specular and rim light. Its parameters and equations are ours, not claims about NiloToonURP internals or visual parity.

Original source: https://github.com/ColinLeung-NiloCat/UnityURPToonLitShaderExample, commit `fbe276e0df1d1f9379e13581c20b6a080a1a6977`. The upstream README explicitly states that the full version is closed source and is not in the repository. The public tutorial is MIT licensed; its license and attribution are in `LICENSE-Nilo-MIT.txt`. The source file contains adaptations of its equations and must be distributed with that notice.

## What is and isn't delivered

- Source, build script, SPIR-V entry points and reflection, plus the mathematical explanation in `MATH.md`.
- This is a **shader package**, not a complete Vulkan renderer. You must create render passes/pipelines, descriptor sets, textures, vertex buffers, uniforms, shadow maps and camera/light transforms.
- The tutorial's surface sampling, alpha cutout, emission, occlusion, face light adjustment, direct/indirect composition, inverted hull and its camera-scaled width are adapted. Additional lights are capped at eight **point lights**, not all of URP's supported light types. Main light uses a custom single-map PCF shadow; points do not cast shadows. Spotlights, cascades, screen-space shadows, instancing/VR, LOD fade, URP render layers, octahedral normals, special shadow bias, URP fog and all URP integration are absent. Thus this is not a pixel-identical or feature-complete URP port.
- URP's point-light attenuation, SH sampling, depth projection and shadow algorithms are host-dependent and not reproduced exactly. The tutorial source declares several property UI sliders that are not used by its lighting equations, including `_IndirectLightMultiplier`, `_DirectLightMultiplier` and the two ignore-cel sliders; these are deliberately not invented here.
- The independent model is an illustrative extension, not a complete replica of the full product; its face map shifts a threshold and does **not** implement light-direction-aware mirrored face SDFs.

## Build and validation

Install the [Slang compiler](https://github.com/shader-slang/slang), then:

```sh
SLANGC=/path/to/slangc ./build.sh   # or put slangc in PATH
python3 test_math.py
```

The included binaries were compiled with Slang `2026.21`, `spirv_1_3`, row-major matrix layout. All eight compiled successfully. The build script additionally runs `spirv-val --target-env vulkan1.1` if it is installed; it was **not installed during authoring**. No actual Vulkan rendering or visual comparison was performed. Each SPIR-V file's exported entry-point name is `main`. `spirv/reflection.json` is compiler reflection from `fragExtended`; refer to the source for per-entry actual usage. Compiler versions/options can change layout: regenerate reflection after changes.

## Render integration

1. Use Vulkan 1.1 or newer; shader target SPIR-V 1.3. Mesh inputs: location 0 `vec3` object position, 1 `vec3` object normal, 2 `vec2` UV, all float32. The shader does not perform skinning; submit already-skinned data or add skinning. Respect the Vulkan viewport/front-face winding convention used by your renderer.
2. Normal forward pass: `vertMain.spv` + `fragTutorial.spv` (or `fragExtended.spv`), opaque, blending disabled, depth test/write enabled, culling **off**, depth compare `LESS_OR_EQUAL` for conventional depth. Draw outlined objects.
3. Inverted hull: `vertOutline.spv` + corresponding outline fragment, cull **front** faces, depth test/write on, same depth comparison, opaque. Draw after the surface. Open meshes, hard split normals and thin geometry may have cracks/artifacts. This is normal for this technique.
4. Optional depth prepass: `vertMain` + `fragDepth`, no color attachment or color writes disabled. It alpha-tests exactly like the surface. Depth normals: `vertMain` + `fragNormals`, use a floating-point or SNORM color target because output is raw world normal [-1,1]. Both use the same alpha cutoff.
5. Optional shadow caster: draw `vertMain` + `fragDepth` with the light's view/projection set in the frame buffer and appropriate raster depth bias. Render into a sampleable conventional-depth image, transition it to shader-readable layout and synchronize before lighting. No URP-specific per-vertex shadow bias or near-plane clamping is implemented; use pipeline depth bias. Keep the camera and shadow matrices consistent with Vulkan's 0..1 depth. This pass is **not** for outline geometry.
6. Host computes `worldToShadow = lightProjection * lightView` matching the depth image. UV is `(ndc.xy * 0.5 + 0.5)`; encode any Y flip in light projection/viewport consistently. Set shadow enable to zero if no shadow map exists. The depth texture must be sampled as ordinary float, not a comparison sampler. It needs sampled-image usage, correct image layout/barriers, a format supporting sampling, and nearest filtering. The shader explicitly compares nine texels (out-of-bounds taps are lit). Reverse-Z shadows are unsupported. Current camera projection may use reverse-Z if its depth pipeline settings are changed accordingly, but this was not tested.
7. Default textures: base/emission/AO white, outline mask black, face threshold neutral 0.5, shadow depth 1.0. Bind valid descriptors even for disabled features unless your pipeline layout and descriptor features allow otherwise. Linear material sampler can repeat or clamp as appropriate; nearest non-comparison shadow sampler is separate. Base and emission maps generally use sRGB image formats; masks, AO, face and shadow use linear formats. Shader colors and lights must be linear. Use an sRGB output attachment or later tone-map/encode as appropriate.

## Descriptors: all set 0

| Binding | Vulkan descriptor type | Meaning |
|---|---|---|
| 0 | uniform buffer | `FrameData` (448 bytes) |
| 1 | uniform buffer | `MaterialData` (288 bytes) |
| 2–7 | sampled image | base, emission, AO, outline-mask, shadow-depth, face-map respectively |
| 8 | sampler | material linear sampler |
| 9 | storage buffer, readonly | `PointLight[]`, 32-byte stride: float4 world position/inverse range then float4 RGB/intensity |
| 10 | uniform buffer | `uint4` light counts; `.x` number of point lights, clamp to 8 |
| 11 | sampler | shadow nearest, non-comparison sampler |

Create a layout with stage flags covering all uses (vertex + fragment for bindings 0, 1, 5, 8; fragment for the remainder is sufficient). Even when count is 0, use a valid dummy buffer at binding 9 where required. Each matrix is 64 bytes, rows stored consecutively, and is mathematically multiplied as `mul(M, columnVector)`; all other fields are 16-byte vectors. Don't blindly memcpy GLM's default column-major matrices. Offsets are in `spirv/reflection.json`. Frame has five matrices at byte offsets 0,64,128,192,256, followed by eight float4s at 320,336,...,432. Material has eighteen float4s at 0,16,...,272. `normalToWorld` is the inverse transpose of object-to-world with translation ignored, not object-to-world under nonuniform scale. Set up dynamic buffer offsets respecting `minUniformBufferOffsetAlignment`. Binding 10 is 16 bytes.

Frame vectors, in order after the matrices:
- `cameraPosition` xyz; `mainDirectionShadow` xyz = normalized world-space direction **from surface to light**, w = 0/1 use shadow; `mainColor` rgb; `indirectColor` rgb.
- `fogColor` rgb; `fogParams` = (start distance, end distance, enabled 0/1, unused); view depth is `abs(view.z)`.
- `shadowParams` = (unused, depth bias, world receiver offset, unused); `outlineCamera` = (vertical field of view in degrees, orthographic half-height, is-orthographic 0/1, unused). View space must look down -Z.

Material vectors, in source order (the upstream-like defaults are a starting point, not calibrated output):

```
baseColor          = (1, 1, 1, 1)
baseUV             = (1, 1, 0, 0)
emissionColor      = (0, 0, 0, 0)       # w disables emission
emissionChannel    = (1, 1, 1, 0)       # w albedo multiplication amount
occlusionChannel   = (1, 0, 0, 0)
occlusionParams    = (0, 1, 0, 1)       # off, strength, remap start/end
indirectMin        = (0.1, 0.1, 0.1, 0)
celParams          = (-0.5, 0.05, 0, 0.65) # midpoint, softness, face, shadow strength
shadowColor        = (1, 0.825, 0.78, 0)
alphaParams        = (0, 0.5, 0, 0)    # alpha clipping off, cutoff
outlineColorWidth  = (0.5, 0.5, 0.5, 1)
outlineParams      = (0.0001, 0, 1, 0) # view offset, mask start/end
bandParams         = (0.3, 0.05, 0, 0) # second threshold/softness, face map off
midColor           = (0.75, 0.72, 0.70, 0)
specularParams     = (32, 0.5, 0.05, 0.3)
specularColor      = (1, 1, 1, 0)
rimParams          = (2, 0.4, 0.05, 0.3)
rimColor           = (1, 1, 1, 0)
```

Reasonable frame starting values: camera position actual, main direction normalized and nonzero, `mainColor=(1,1,1,0)`, `indirectColor=(0.1,0.1,0.1,0)`, `fogParams=(0,100,0,0)`, `shadowParams=(0,0.001,0,0)`, `outlineCamera=(60,5,0,0)`. Use valid matrices and projection; bind a dummy shadow even when disabled. Each point uses positive inverse range `1/range` and nonnegative intensity. Sampling and light ranges should be authored to fit scene scale; no energy conservation is promised.
