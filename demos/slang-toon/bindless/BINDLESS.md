# Bindless ABI for `mu_gfx`

`bindless/toon.slang` is the same tutorial/extended model as `toon.slang`, but
it speaks the `mu_gfx` shader contract instead of the fixed 12-binding layout
from `README.md`:

| Set 0 | Resource | Use |
|---|---|---|
| binding 0 | `Texture2D<float4> sampledImages[]` | every 2D map, selected by index |
| binding 2 | `SamplerState sampledSamplers[]` | every sampler, selected by index |
| binding 3 | `ByteAddressBuffer scene` | one storage buffer: frame, material, lights, vertices, map indices |
| push | `BindlessPush` (48 B) | byte offsets into `scene` |

This matches `vk.h` §14 exactly: no UBO, no SSBO array, no per-feature binding,
and no vertex-input state. `spirv/bindless/` contains the compiled stages;
`spirv/bindless/reflection.json` is compiler reflection for `fragTutorial`.

The fixed-binding `toon.slang` and its `spirv/*.spv` remain in the package for
hosts that already implement the original layout. Entry point names are the
same in both variants, so a host selects the directory, not the code.

## Push constants

`BindlessPush` is 48 bytes, all `uint` (it was 32 in v1; see [ABI v2](#abi-v2)):

| Offset | Field | Meaning |
|---|---|---|
| 0 | `frameOffset` | byte offset of `FrameData` |
| 4 | `materialOffset` | byte offset of `MaterialData` |
| 8 | `inspiredOffset` | byte offset of `InspiredData` (inspired package only) |
| 12 | `mapsOffset` | byte offset of `MaterialMaps` |
| 16 | `pointLightOffset` | byte offset of `PointLight[0]` |
| 20 | `pointLightCount` | clamped to 8 by the shader |
| 24 | `vertexOffset` | byte offset of `ToonVertex[0]` |
| 28 | `flags` | `BINDLESS_HAS_INSPIRED = 1` and the `TOON_FLAG_*` bits below |
| 32 | `objectOffset` | byte offset of `ObjectData` (`TOON_FLAG_OBJECT_DATA`) |
| 36 | `reserved0..2` | must be 0 |

Offsets are byte offsets into the same `ARENA_DEVICE` storage buffer the host
already binds at binding 3, i.e. **`slice.offset` + the offset inside your
slice** — an `arena_alloc` slice does not have to start at 0. Align each block
to 16 bytes; `ByteAddressBuffer`
loads are typed, so the shader reads `FrameData`, `MaterialData`, `PointLight`
and `ToonVertex` directly. The layouts of `FrameData` (448 B) and
`MaterialData` (288 B) are identical to the fixed-binding package, including
row-major matrices.

`pointLightCount` may be zero; `pointLightOffset` still has to be a valid
4-byte-aligned offset (a dummy offset is fine). The vertex stage also reads from
`scene`, so `vertexOffset` is required for `vertMain`/`vertOutline`.

## `MaterialMaps` (64 B, 16 `uint`)

| Offset | Field | Default when `0xFFFFFFFF` |
|---|---|---|
| 0 | `baseMap` | white |
| 4 | `emissionMap` | white (emission is disabled by `emissionColor.w`) |
| 8 | `occlusionMap` | white |
| 12 | `outlineMask` | black (full outline) |
| 16 | `shadowMap` | 1.0 (fully lit) |
| 20 | `faceMap` | 0.5 (neutral threshold) |
| 24 | `normalMap` | flat `(0.5,0.5,1)` |
| 28 | `materialMap` | white (roughness/metallic multipliers) |
| 32 | `matcapMap` | white |
| 36 | `noiseMap` | white (dissolve always passes) |
| 40 | `sceneLinearDepth` | far `1e6` (rim disabled by default) |
| 44 | `hairShadowMask` | black (no hair shadow) |
| 48 | `linearSampler` | linear/filtering sampler bindless index |
| 52 | `shadowSampler` | nearest, non-comparison sampler bindless index |
| 56 | `shadowWidth` | shadow-map width in texels |
| 60 | `shadowHeight` | shadow-map height in texels |

Indices come from `texture_info()` / `sampler_bindless_index()`; the shader uses
`NonUniformResourceIndex` on both, so the Vulkan device must enable the
`SPV_EXT_descriptor_indexing` features `vk.c` already requests. `NO_TEXTURE`
(`0xFFFFFFFF`) takes the fallback branch, which is why a host can omit optional
maps without reserving descriptor slots.

`shadowWidth`/`shadowHeight` replace the original `GetDimensions` call, because
the shadow texture is now a dynamic array element. Set them to the actual map
size even when shadows are disabled; a zero size is clamped to 1.

## Vertex layout

`ToonVertex` is the 48-byte record used by `demos/toon/shaders/toon_common.slang`:

| Offset | Field |
|---|---|
| 0 | `float4 pos` (object space, xyz used) |
| 16 | `float4 nrm` (object space, xyz used) |
| 32 | `float2 uv` |
| 40 | `float2 pad` — 0, or the octahedral-encoded smoothed outline normal (`TOON_FLAG_SMOOTH_OUTLINE_NORMAL`) |

`vertMain`, `vertOutline` and `vertShadow` fetch
`scene.Load<ToonVertex>(vertexOffset + SV_VertexID * 48)`. `vertexOffset` is a
byte offset and does not have to be a multiple of 48. There is no `VkVertexInputBindingDescription`; draw with
`pass_draw(verts, instances, 0, 0)` as in `vk.h` §17.

## Entry points

| Stage | Entry | Notes |
|---|---|---|
| vertex | `vertMain` | bindless fetch, no outline offset |
| vertex | `vertOutline` | expanded hull, front-face culling |
| vertex | `vertShadow` | shadow caster: biased, projected by `worldToShadow` (v2) |
| fragment | `fragTutorial` | tutorial model |
| fragment | `fragTutorialOutline` | tutorial model × outline color |
| fragment | `fragExtended` | 3-band extended model |
| fragment | `fragExtendedOutline` | extended × outline color |
| fragment | `fragDepth` | alpha clip only; no color output. Pair with `vertMain` (prepass), `vertOutline` (prepass including the hull) or `vertShadow` (shadow map) |
| fragment | `fragNormals` | raw signed world normal |

All compile with Slang 2026.21 as `spirv_1_3`, row-major matrices. The compiler
emits `SPV_EXT_descriptor_indexing` capabilities because of the descriptor
arrays; that is expected and already required by `vk.h`'s bindless layout.

## Host obligations

The shader does not create or transition resources. A host still has to:

1. allocate/upload the `scene` blocks and vertices, then push their byte
   offsets with `pass_push()`;
2. bind at least one valid sampled image, sampler and storage buffer (the
   pipeline layout has exactly one set);
3. supply a real shadow map, point lights, face SDF, hair mask, linear depth,
   MatCap and noise map only for the features it enables; otherwise use the
   fallbacks above;
4. keep the Vulkan Y-flip/projection conventions documented in `README.md`.

The supplied fallbacks make the tutorial path render with no textures at all:
bind one white image, one black image, a linear sampler and a nearest sampler,
leave every index at `NO_TEXTURE`, and set `shadowWidth/Height` to 1.

## ABI v2

v2 adds five opt-in flags, one vertex stage and two records. **A host that sets
none of the new flags gets the v1 image**: this was checked by rendering the v1
host against the v2 SPIR-V (`fragExtended` and `fragInspired`, 1280×720,
lavapipe) and comparing with the v1 SPIR-V — 0 differing pixels. The one thing
a v1 host has to change is the push size: push 48 bytes with the last 16 zeroed.

| Flag | Bit | Effect |
|---|---|---|
| `BINDLESS_HAS_INSPIRED` | 0 | (v1) `InspiredData` present at `inspiredOffset` |
| `TOON_FLAG_OBJECT_DATA` | 1 | matrices come from `ObjectData` at `objectOffset` instead of `FrameData` |
| `TOON_FLAG_MATERIAL_STYLE` | 2 | a `MaterialStyle` follows `MaterialData` at `materialOffset + 288` |
| `TOON_FLAG_SMOOTH_OUTLINE_NORMAL` | 3 | `vertOutline` extrudes along the normal decoded from `ToonVertex.pad` |
| `TOON_FLAG_OUTLINE_PIXEL_WIDTH` | 4 | `outlineColorWidth.w` is a width in pixels |
| `TOON_FLAG_SHADOW_FLIP_V` | 5 | the shadow map was rendered with NDC +y at row 0 |

### `ObjectData` (128 B)

| Offset | Field |
|---|---|
| 0 | `row_major float4x4 objectToWorld` |
| 64 | `row_major float4x4 normalToWorld` (inverse transpose; translation ignored) |

In v1 the two matrices live in `FrameData`, so every object needs its own
448-byte `FrameData` with the camera, light and shadow matrices repeated. With
the flag, one `FrameData` serves the whole frame — and the shadow pass, because
`vertShadow` reads `worldToShadow` from the same record.

### `MaterialStyle` (32 B, at `materialOffset + 288`)

Read by `fragExtended*` and `fragInspired*` only; the tutorial stages stay a
strict port. Without the flag the shader uses the neutral values.

| Offset | Field | Neutral | Meaning |
|---|---|---|---|
| 0 | `shadowHSV.x` | 0 | hue shift of the albedo in shadow, in turns |
| 4 | `shadowHSV.y` | 1 | saturation multiplier in shadow |
| 8 | `shadowHSV.z` | 1 | value multiplier in shadow |
| 16 | `styleParams.x` | 0 | band anti-aliasing: softness floor of `x` pixels (`fwidth`) on every band edge |
| 20 | `styleParams.y` | 0 | cast-shadow band softness; 0 keeps the raw 3×3 PCF ramp |
| 24 | `styleParams.z` | 0 | 0..1: restrict the rim to the lit side |

### Reused fields

These were documented as unused in v1 and are neutral at 0.

| Field | Meaning |
|---|---|
| `FrameData.outlineCamera.w` | render-target height in pixels (pixel-width outlines; 0 falls back to the tutorial formula) |
| `FrameData.shadowParams.x` | caster depth bias, world units along −L (`vertShadow`) |
| `FrameData.shadowParams.w` | caster normal bias, world units along −N, scaled by `1 − saturate(N·L)` (`vertShadow`) |
| `MaterialData.outlineParams.w` | pixel mode: view distance beyond which the outline stops growing in world space (0 = constant pixel width at any range) |

### Outline width

Tutorial mode is the upstream formula, quirks included:
`width × saturate(|z|) × fovDegrees × 0.00005`. Pixel mode is exact:

```
world = widthPx × 2 × d / (|viewToClip[1][1]| × targetHeight)     d = |z| (perspective) or 1 (orthographic)
```

Measured on the reference scene (mean thickness of the pixels the outline pass
changed, `--no-outline` as the reference):

| camera | tutorial formula, width 9 | pixel mode, 2 px |
|---|---|---|
| distance 3.5 | 5.13 px | 2.08 px |
| distance 6.6 | 2.62 px | 2.01 px |
| distance 13 | 1.33 px | 1.92 px |
| fov 25° | 2.75 px | 2.02 px |
| fov 90° | 4.23 px | 2.01 px |
| 640×360 | 1.33 px | 1.94 px |
| 1920×1080 | 3.88 px | 2.02 px |

### Smoothed outline normals

Extruding along the shading normal tears the hull apart at every hard edge,
because the copies of one position carry different normals. Bake the normalized
sum of the *distinct* normals that share a position, octahedral-encode it into
`ToonVertex.pad`, and set the flag. `toon_scene_bake_outline_normals()` in
`demos/toon/scene.c` is a reference bake; `octDecode()` in the shader is the
decoder. The lit normal is not affected.

![hull along face normals vs smoothed normals](../docs/cmp_smooth_normals.png)

### Shadow pass

```
ShadowMap   vertShadow + fragDepth   depth-only pipe (PipeKey.color_count = 0), CULL_NONE
```

`worldToShadow` must be orthographic with z in [0,1]. The caster stage applies
the two biases above and clamps `clip.z` to 0 so casters in front of the light's
near plane are flattened onto it instead of clipped. The receiver is unchanged
(`mainShadow`: nearest sampler, manual 3×3 PCF, `shadowParams.y` depth bias).

On `mu_gfx` set `TOON_FLAG_SHADOW_FLIP_V`: `vk.c` rasterizes through a
negative-height viewport, so the map's row 0 is NDC y = +1 while the receiver
computes `v = ndc.y × 0.5 + 0.5`. Without the flag the shadows are mirrored
(left), with it they are attached to their casters (right):

![shadow map sampled without and with the flip flag](../docs/cmp_shadow_flip.png)

A `FMT_D32F` target created with `TGT_DEPTH | TGT_SAMPLED` and listed in the
forward pass's `tex_reads` can be sampled as a regular `Texture2D<float4>`; no
separate R32F colour target is needed.

### Depth prepass that includes the hull

Upstream's `DepthOnly` pass defines `ToonShaderIsOutline`, so the camera depth
texture contains the outline hull as well as the surface. The equivalent here is
two depth-only draws per object, `vertMain + fragDepth` and
`vertOutline + fragDepth` (the second with front-face culling). Use it when a
later pass reads scene depth (screen-space rim, SSAO, depth of field); otherwise
those effects see the outline as background. The reference host does not need a
prepass and does not run one.
