# Bindless ABI: inspired additions

`bindless/inspired.slang` includes `../../slang-toon/bindless/toon.slang` and
adds the inspired fragment entries on top of the base bindless ABI. Read
[`../../slang-toon/bindless/BINDLESS.md`](../../slang-toon/bindless/BINDLESS.md)
first: set 0, `BindlessPush`, `MaterialMaps`, the vertex layout and all fallback
defaults are shared unchanged.

The relative include is why the two packages must stay siblings under
`demos/`. It keeps one copy of the base shader so the two ports cannot drift.
When distributing this package alone, copy `slang-toon/bindless/toon.slang`
next to `inspired.slang` and change the include back to `"toon.slang"`.

## `InspiredData` (224 B, 14 `float4`)

Same fields and order as `inspired.slang`:

| Offset | Field | Use |
|---|---|---|
| 0 | `faceRight` | normalized head-right world direction |
| 16 | `faceForward` | normalized head-forward world direction |
| 32 | `faceParams` | x enable SDF, y softness, z orientation flip |
| 48 | `materialParams` | x normal-map enable, y strength, z roughness, w metallic |
| 64 | `pbrParams` | x intensity, y threshold, z softness, w quantize |
| 80 | `pbrColor` | F0 tint multiplier |
| 96 | `matcapParams` | x additive, y multiply, z shadow-only, w replace |
| 112 | `hairParams` | x strength, y exponent, z direction shift, w threshold |
| 128 | `hairColor` | glint color |
| 144 | `screenParams` | xy target size, z rim pixels, w depth threshold |
| 160 | `screenEffects` | x rim strength, y hair-shadow strength, z enable screen inputs |
| 176 | `fadeParams` | x visibility, y dissolve amount, z edge width, w edge strength |
| 192 | `dissolveColor` | emissive edge color |
| 208 | `tint` | rgba multiplier; alpha affects clipping |

Point `inspiredOffset` at a 16-byte-aligned copy of this struct and set
`flags |= 1` (`BINDLESS_HAS_INSPIRED`). When the flag is clear the shader uses
built-in neutral defaults (`fadeParams.x = 1`, all effects off, tint = 1), so a
base host can run the inspired entry points without the block.

## Extra `MaterialMaps` slots

The inspired path reads the additional indices already present in the shared
`MaterialMaps`:

| Field | Effect | Fallback |
|---|---|---|
| `normalMap` | tangent-space normal (derivative frame) | flat normal |
| `materialMap` | R roughness, G metallic multipliers | white |
| `matcapMap` | view-normal lookup | white |
| `noiseMap` | dissolve threshold R | white (always passes) |
| `sceneLinearDepth` | positive `abs(view.z)` for the rim test | far `1e6` |
| `hairShadowMask` | receiver-aligned 0..1 coverage | black (no shadow) |

`sceneLinearDepth` and `hairShadowMask` are sampled with `shadowSampler` at the
current framebuffer pixel, as in the fixed-binding shader. The host must render
and register those images; the shader does not project or blur them. Disable
`screenEffects.z` when either image is absent.

## Entry points

| Stage | Entry | Notes |
|---|---|---|
| vertex | `vertMain` / `vertOutline` | inherited from the bindless base |
| fragment | `fragInspired` | full inspired model |
| fragment | `fragInspiredOutline` | inspired × outline color |
| fragment | `fragInspiredDepth` | alpha, fade and dissolve clip only |
| fragment | `fragInspiredNormals` | perturbed normal, raw signed world space |

Compile the inspired entries from `bindless/inspired.slang`; compile the two
vertex entries from the sibling `slang-toon/bindless/toon.slang`. They are the
same code, and the build script in this package does exactly that. The
`spirv/bindless/` directory contains all twelve bindless stages (eight base,
four inspired); `spirv/bindless/inspired-reflection.json` documents the
inspired entry.

## Defaults for a textureless run

Set every `MaterialMaps` index to `0xFFFFFFFF` except `linearSampler`,
`shadowSampler`, `shadowWidth` and `shadowHeight`; set `flags = 0`; keep
`screenEffects.z = 0`. The result is the same shading as `fragExtended` plus
the inspired tint path, with no normal map, MatCap, dissolve or rim. This is the
cheapest way to smoke-test the bindless pipeline before authoring inputs.
