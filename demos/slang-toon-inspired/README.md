# Independent anime toon shader for Vulkan / Slang

> **Status in `mu_gfx`:** the fixed-binding sources below are kept for
> reference. The integration path is `bindless/`, which reuses the sibling
> `demos/slang-toon/bindless/toon.slang` and adds the inspired entries on the
> `vk.h` ABI. Its compiled stages live in `spirv/bindless/`;
> `demos/slang-toon/host.c --inspired` is a headless reference host that runs
> `fragInspired` with neutral defaults. See
> [`bindless/BINDLESS.md`](bindless/BINDLESS.md). The screen-space rim and hair
> mask still require renderer-generated inputs; leave `screenEffects.z = 0`
> until they exist.
>
> `TUTORIAL-PORT.md` in this directory is a copy of the base package's README
> and documents only the inherited tutorial interface; use this README and
> `INSPIRED-MATH.md` for the inspired additions.

This is a **feature-inspired prototype**, not NiloToonURP, not its source code, and not a guaranteed visual or feature match. It builds on the previous MIT-licensed public tutorial port and adds original approximations inspired by the public description of NiloToonURP: face-style shading, hair shadows, screen-space rim, PBR specular, MatCap, normal maps, and dissolve/fade. Its author-provided image gallery and feature list do not expose the proprietary implementation or all the data needed to reconstruct it. See [the original repository](https://github.com/ColinLeung-NiloCat/UnityURPToonLitShaderExample) and its "What is NOT included" section. License and attribution for the adapted tutorial are in `LICENSE-Nilo-MIT.txt`.

**This is shader code and a renderer integration contract, not a complete executable, Unity plugin, or drop-in replacement.** It has not been rendered or visually matched. A Vulkan renderer, suitable art assets, and effect-specific passes are required. The shader was compiled, but the visual quality and numerical stability across GPUs remain unverified.

## What's included

| Feature | Implementation / important qualification |
|---|---|
| Toon bands | Two smooth thresholds with 3 RGB tints; directional main light, up to 8 unshadowed point lights |
| Face shadows | Artist-provided, mirrored face SDF threshold map and head world axes; custom convention, not the proprietary algorithm |
| Hair shadow on face | Samples a renderer-generated screen-space mask; **mask generation is not included** |
| Outline | Tutorial's backface inverted hull. It does not bake separate smooth outline normals or guarantee constant pixel width |
| Rim | Fixed-pixel-offset depth-discontinuity test using a renderer-generated linear view-depth texture |
| Specular | Independent GGX/Schlick-Smith term, optional banded specular |
| Hair glint | Stylized anisotropic lobe using head-up as an approximate strand direction, not texture-driven strand tangents |
| MatCap | View-normal lookup with additive/multiply/replace and optional shadow-only mask |
| Material maps | Base, emission, AO, normal, roughness/metallic, outline offset, face SDF, dissolve noise |
| Fade / dissolve | Screen-space hash dither and UV noise threshold, with emissive dissolve edge |
| Existing passes | Forward, outline, depth/shadow cutout and world normals; inherited single directional shadow map and fog |

Not included: automatic hair-mask creation, Unity renderer features, editor tooling, smooth-normal baking, eye/eyebrow redraw or stencil, 2D mouth, HSV tools, parallax/detail maps, additional-light shadows/spotlights, shadow cascades, motion vectors, VR, bloom/anime postprocessing, volume controls, tessellation, perspective removal, shader stripping, or any full-product compatibility. Many features require extra renderer code. The public list itself is non-exhaustive. This is a starting point rather than a production-ready/full substitute.

## Files and build

- `inspired.slang` implements this extension; `toon.slang` contains the tutorial adaptation and shared vertex stages.
- `INSPIRED-MATH.md` explains these new equations. `MATH.md` explains the common tutorial equations. `TUTORIAL-PORT.md` documents the inherited interface, matrices, pipeline settings and caveats.
- `spirv/` contains all compiled stages and compiler reflection; each exported entry point is `main`.
- Build with `SLANGC=/path/to/slangc bash build.sh` and run `python3 test_math.py`. All twelve shader entries compiled with Slang 2026.21 to SPIR-V 1.3. The available CPU checks test shared math only. `spirv-val` and actual Vulkan rendering were not available; neither GPU behavior nor appearance has been validated. The build runs `spirv-val` automatically if installed.

## Pipeline and renderer requirements

Read `TUTORIAL-PORT.md` first for set 0 bindings 0–11, vertex format, matrix layout, shadow map, cutout, outlines, color space and uniform layouts. Use `vertMain.spv + fragInspired.spv`, then `vertOutline.spv + fragInspiredOutline.spv` for outlined geometry. For depth/shadow cutout use `vertMain + fragInspiredDepth`, and for raw signed world normals use `vertMain + fragInspiredNormals`; do not use the old tutorial depth/normal passes for fading objects. Correctly synchronize all render targets and image layouts. Alpha blending is disabled. Front-face culling for the outline must be set consistently with your winding.

Additional descriptor bindings, all in **set 0**:

| Binding | Type | Use |
|---|---|---|
| 12 | uniform buffer | `InspiredData`, 224 bytes, 14 float4 slots |
| 13 | sampled image | tangent-space normal RGB, linear format; neutral `(0.5,0.5,1)` |
| 14 | sampled image | material map, linear: R roughness multiplier, G metallic multiplier; white for defaults |
| 15 | sampled image | MatCap RGB; linearized sampling, default white |
| 16 | sampled image | dissolve noise R, linear, default white |
| 17 | sampled image | positive `abs(view.z)` scene depth, float format, sampled with binding 11's nearest non-comparison sampler |
| 18 | sampled image | screen-space hair-shadow coverage 0–1, 0 no shadow, sampled with binding 11 |

Binding 7 (`faceMap`) is interpreted as a red-channel face SDF in the inspired model; it is an ordinary threshold offset in the older extended model. All images need valid descriptors even when toggles are off, unless your specific Vulkan descriptor setup permits otherwise. Binding 12 and images 13–18 are fragment-stage resources. Slot 12's offsets are 0,16,...,208 as shown in `spirv/inspired-reflection.json`.

### Additional material values

All are float4; w is unused unless indicated. These are starting points, not values extracted from NiloToonURP. Set screen effects, face SDF, normal mapping, MatCap, specular and hair glint off initially; enable one at a time with appropriate input textures.

```
faceRight      = (1, 0, 0, 0)          # normalized head-right world direction
faceForward    = (0, 0, 1, 0)          # normalized head-forward world direction
faceParams     = (0, 0.03, 0, 0)       # enable SDF, softness, mirror orientation flip
materialParams = (0, 1, 0.5, 0)        # normal-map enable/strength, roughness, metallic
pbrParams      = (0, 0.1, 0.03, 0)     # spec intensity, threshold, softness, quantize toggle
pbrColor       = (1, 1, 1, 0)          # F0 multiplier, keep within [0,1]
matcapParams   = (0, 0, 0, 0)          # additive, multiply, shadow-only mix, replace
hairParams     = (0, 16, 0, 0.7)       # glint strength, exponent, direction shift, threshold
hairColor      = (1, 1, 1, 0)
screenParams   = (width, height, 2, 0.02) # target dimensions, rim pixels, depth threshold in view units
screenEffects  = (0, 0, 0, 0)          # rim strength, hair-shadow strength, enable screen sampling
fadeParams     = (1, 0, 0.05, 1)        # visibility, dissolve threshold, edge width, edge intensity
dissolveColor  = (1, 0.3, 0.05, 0)
tint           = (1, 1, 1, 1)          # RGB multiplier and alpha; keep alpha in [0,1]
```

The old material's `rimColor`, `bandParams`, `midColor` and `shadowColor` still control the respective results. `rimParams` from the older model is **not** used here. Fade, dissolve and alpha are applied on the outline as well as the surface. Keep F0 multiplier, metallic, fade, dissolve, shadow strength and opacity in [0,1], roughness positive, and head axes orthogonal and normalized. Set the main light direction to a nonzero normalized world-space direction toward the light. There is no automatic conversion from screenshots to tuned material maps.

### Building the two required screen inputs

**Linear depth (binding 17).** Before the inspired color/outline passes, render visible opaque character and scene geometry to depth, then produce a separate shader-readable, single-sample floating-point image storing *positive linear view depth* `abs(viewZ)` for the closest surface at each screen pixel. Clear empty pixels to a large far value. Match the active viewport, render resolution, image orientation, and camera projection. Implement conversion from your depth representation or output it during a prepass; this package does not include that conversion. Rim looks at an offset neighbor and turns on if its stored depth exceeds the current surface depth plus the threshold. It detects both character/background and internal depth discontinuities; it is not a semantic silhouette classifier. At occlusions and screen edges artifacts can occur.

**Hair mask (binding 18).** Supply a same-resolution, single-sample red-channel mask registered to receiver pixels (0 = none, 1 = full). A renderer can generate it by rendering a tagged hair coverage/depth pass and projecting/offsetting that coverage onto a face receiver mask; the exact projection, clipping and per-character association are application-specific and are **not provided here**. A generic screen-offset mask can leak across characters, objects or depth discontinuities; use per-character/stencil/depth separation. Binding 18 is *already receiver-aligned*—the shader does not shift or project it. With no mask, bind black and leave `screenEffects.z=0`.

The shader samples both in fragment coordinates using `SV_Position.xy / (width,height)`. It assumes the textures have the same pixel orientation and viewport origin as the forward target, with origin `(0,0)`. Account for render scaling, nonzero viewport origins, Y flips, MSAA resolve, dynamic resolution and stereo in your renderer. The rim offset uses view-space normal X and negative Y, assuming conventional top-left framebuffer coordinates; projection/Y conventions might require adjustment.

### Face SDF authoring convention

The face texture red channel is a 0..1 monotonically varying signed-distance-like visibility ramp. For light at the head forward direction, the horizontal angle normalized by π is 0; at the back it is 1. At a given face pixel, it becomes lit when texture R exceeds that angle, with softness `faceParams.y`. The shader mirrors UV around 0.5 depending on the sign of the light's dot with head right; `faceParams.z` flips the side convention. Set orthogonal head-space axes transformed into world space, and author/calibrate the texture and UVs for this particular convention. At light directly overhead, horizontal projection approaches zero and the angle is unstable: disable or provide an alternate face-light policy. This is a simple original approximation, not a recovery of any private face method.

### Dither and shadow limitations

Dither uses `SV_Position` screen pixels. A light-camera shadow pass uses different pixel coordinates than the camera passes, so dithered shadows will not line up exactly, and may shimmer. Outline geometry also has different raster positions. Use a shared object/world-space fade pattern or dedicated temporal method if stable cross-pass consistency is necessary. Dissolve uses the same UV noise across passes but raster/texture filtering may still differ. No renderer or animation state is shipped.
