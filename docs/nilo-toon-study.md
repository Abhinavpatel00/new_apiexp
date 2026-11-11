# Study: ColinLeung-NiloCat/UnityURPToonLitShaderExample

Source read: every file of the repository (MIT) — the `.shader` with its five
passes, `SimpleURPToonLitOutlineExample_Shared.hlsl`,
`..._LightingEquation.hlsl`, `NiloOutlineUtil.hlsl`, `NiloZOffset.hlsl`,
`NiloInvLerpRemap.hlsl`. This note records how the shader works, where its code
and its comments disagree, and what was taken from it for
`demos/slang-toon/bindless` (ABI v2) and `demos/toon`.

The upstream file is deliberately a *tutorial subset* of the commercial
NiloToonURP. Its README lists what was left out; the last section here maps
those to this repository.

## 1. Structure

One `.shader`, one shared vertex/fragment pair, five passes that differ only by
render state and two `#define`s.

| Pass | State | Defines | Purpose |
|---|---|---|---|
| ForwardLit (`UniversalForwardOnly`) | Cull Off, ZWrite On | — | the lit surface |
| Outline | Cull Front, ZWrite On | `ToonShaderIsOutline` | inverted hull, same fragment × `_OutlineColor` |
| ShadowCaster | Cull Off, ColorMask 0 | `ToonShaderApplyShadowBiasFix` | URP shadow bias, z clamped to near |
| DepthOnly | Cull Off, ColorMask R | **`ToonShaderIsOutline`** | camera depth texture *including the hull* |
| DepthNormalsOnly | Cull Off, ColorMask RGBA | — | depth + normals, without the hull |

Two design decisions are worth copying on any engine:

- **One keyword only** (`_UseAlphaClipping`). Every other switch (`_IsFace`,
  `_UseEmission`, `_UseOcclusion`) is a uniform branch. Variants cost 2ⁿ
  pipelines and break SRP batching; a predictable uniform branch costs
  almost nothing. `mu_gfx` has the same pressure (`MAX_PIPES` is 24), and the
  bindless port follows the same rule: features are `flags` bits and
  parameters, not entry points.
- **The outline pass shades.** It runs the full lighting function and multiplies
  by `_OutlineColor`, so outlines darken in shadow and take the surface hue,
  instead of being a flat ink colour.

## 2. Vertex stage

```
positionWS  = TransformObjectToWorld(positionOS)
#ifdef ToonShaderIsOutline
positionWS += normalWS * _OutlineWidth * GetOutlineCameraFovAndDistanceFixMultiplier(viewZ)
#endif
positionCS  = TransformWorldToHClip(positionWS)
#ifdef ToonShaderIsOutline
positionCS  = NiloGetNewClipPosWithZOffset(positionCS, _OutlineZOffset * mask + 0.03 * _IsFace)
#endif
#ifdef ToonShaderApplyShadowBiasFix
positionCS  = shadow-biased position, z clamped to the near plane
#endif
```

**Width.** `GetOutlineCameraFovAndDistanceFixMultiplier`:

```
perspective:  saturate(abs(viewZ)) * fovDegrees * 0.00005
orthographic: saturate(abs(orthoSize)) * 50 * 0.00005
```

**Z offset.** `NiloGetNewClipPosWithZOffset` computes a second clip position
from a view-space z moved by the offset and replaces *only* `clip.z` (rescaled
by `w`), so the vertex does not move on screen. `mask` is
`invLerpClamp(remapStart, remapEnd, 1 − _OutlineZOffsetMaskTex.r)`: artists
paint where the outline should be pushed back (eyes, mouth). Faces get a
constant extra 0.03.

## 3. Lighting equation

```
ShadeGI            = max(_IndirectLightMinColor, SampleSH(0)) * lerp(1, occlusion, 0.5)
litOrShadowArea    = smoothstep(_CelShadeMidPoint - _CelShadeSoftness,
                                _CelShadeMidPoint + _CelShadeSoftness, N·L) * occlusion
                     face:  lerp(0.5, 1, litOrShadowArea)
                     *=     lerp(1, shadowAttenuation, _ReceiveShadowMappingAmount)
ShadeSingleLight   = saturate(lightColor) * lerp(_ShadowMapColor, 1, litOrShadowArea)
                     * min(4, distanceAttenuation) * (additional ? 0.25 : 1)
composite          = albedo * max(indirect, sum(direct)) + emission
outline            = composite * _OutlineColor;   then MixFog
```

Defaults: midpoint −0.5, softness 0.05, shadow colour (1, 0.825, 0.78), receive
amount 0.65. The shadow test position is offset along the light,
`positionWS + L * (_ReceiveShadowMappingPosOffset + _IsFace)`.

Things that make it read as "anime" rather than "lit":

- `SampleSH(0)`: the normal is zeroed, so indirect light is one flat colour and
  the form is described only by the band.
- `max(indirect, direct)` instead of a sum: adding lights never washes a
  character out, and a character in darkness never goes black.
- `saturate(lightColor)` and `min(4, attenuation)`: HDR lights cannot overexpose.
- Additional lights at 0.25: several lights overlapping do not sum past white.
- The shadow *colour* (a tint multiplied in), not a shadow *darkness*.

## 4. Where code and comments disagree

Found by reading the code against its comments; each is a decision point for a
port.

| # | Upstream | What the code does | Decision here |
|---|---|---|---|
| 1 | ZOffset comment: "push … towards camera" | `modifiedPositionVS_Z = -originalPositionCS.w + -viewSpaceZOffsetAmount`: **away** from the camera (which is what hides the hull behind a face) | bindless port was already right; `demos/toon` followed the comment — **fixed**, see §5 |
| 2 | "keep outline similar width on screen" | `saturate(abs(viewZ))`: constant pixel width only within **one unit**; beyond that constant *world* width, so the line thins as 1/z | tutorial mode keeps it; **pixel mode added** |
| 3 | width × `fovDegrees` | linear in the angle, not in `tan(fov/2)`: a 120° camera draws the line about 1/3 thinner than a 60° one (0.667×, `test_math.py`) | same |
| 4 | orthographic path | `saturate(abs(orthoSize)) * 50`; the clip-z rewrite is written for a reversed-Z projection | pixel mode uses `1/P11`, no special case |
| 5 | `_IsFace` shadow offset | adds a full **world unit** to the receive offset: scale-dependent | kept in the tutorial stage (strict port); do not use at non-human scale |
| 6 | Properties `_IndirectLightMultiplier`, `_DirectLightMultiplier`, `_MainLightIgnoreCelShade`, `_AdditionalLightIgnoreCelShade` | declared, **never read** by the lighting code | not invented in the port |
| 7 | DepthOnly defines `ToonShaderIsOutline`, DepthNormalsOnly does not | depth texture has the hull, depth-normals does not: a depth-edge effect and a normal-edge effect see different silhouettes | documented; host pairs `vertOutline + fragDepth` when it needs the hull |

## 5. What was changed in this repository because of it

`demos/slang-toon/bindless/toon.slang` — ABI v2. `fragTutorial*` is unchanged
and stays a strict port; everything below is opt-in (`BINDLESS.md`, "ABI v2").

| Upstream idea | Status before | Now |
|---|---|---|
| ShadowCaster pass with bias + near clamp | missing: nothing could render a shadow map | `vertShadow` (depth bias along −L, normal bias × (1 − N·L), z clamp); host renders a 2048² map |
| Outline width "same on screen" | upstream formula | + exact pixel mode: 1.92–2.08 px where the formula gives 1.33–5.13 px |
| DepthOnly includes the hull | not documented | documented pairing |
| Uniform branching, no variants | already followed | kept: v2 adds flag bits, one new vertex entry |
| One `FrameData` per frame (Unity's per-draw `unity_ObjectToWorld`) | matrices inside `FrameData`: one 448-byte copy per object | `ObjectData` (128 B) per object |

From the full NiloToon feature list (not in the tutorial; implemented
independently, no upstream code exists to port):

| NiloToon feature | Here |
|---|---|
| smooth outline normal baking | `toon_scene_bake_outline_normals()` + `TOON_FLAG_SMOOTH_OUTLINE_NORMAL` |
| HSV shadow / tint controls | `MaterialStyle.shadowHSV` |
| cel-edged cast shadows | `MaterialStyle.styleParams.y` |
| anti-aliased band edges | `MaterialStyle.styleParams.x` (`fwidth` floor) |
| face SDF, hair shadow, GGX, MatCap, depth rim, dissolve | already in `demos/slang-toon-inspired` (unchanged apart from sharing the style helpers) |
| screen-space outline, perspective removal, fake SSS, per-character render features | not done; see `demo-roadmap.md` |

`demos/toon`:

- z-offset sign corrected to match the upstream code (row 1 above). The default
  frame is byte-identical (offset 1e-4); at the slider maximum (0.02) 97 pixels
  of a 1280×720 frame change.
- README now lists the unclamped distance term as a deliberate difference
  (row 2), which it was but did not say.
