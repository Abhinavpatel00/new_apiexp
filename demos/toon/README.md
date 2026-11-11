# Toon shading demo (URP toon lit port)

A port of [`ColinLeung-NiloCat/UnityURPToonLitShaderExample`](https://github.com/ColinLeung-NiloCat/UnityURPToonLitShaderExample)
(MIT, see [`LICENSE`](LICENSE)) from Unity URP HLSL to Slang + Vulkan on
`src/vk.h`. The tutorial shader's job is kept intact: **one cel-shaded
directional light, a flat indirect term, and a second "inverted hull" pass that
draws the outline**. Everything around it — render graph, push constants,
bindless vertex fetch, headless capture — is this repository's.

![toon demo with outline](docs/toon_outline.png)

| `--no-outline` | soft band (`--cel 0.0 0.35`) |
| --- | --- |
| ![outline pass off](docs/toon_outline_off.png) | ![soft cel band](docs/toon_soft_band.png) |

## What the port keeps

The two files that matter upstream are
`SimpleURPToonLitOutlineExample_LightingEquation.hlsl` (the whole look) and
`NiloOutlineUtil.hlsl` / `NiloZOffset.hlsl` (the outline width and depth bias).
`shaders/toon.slang` is a line-for-line translation of the first, and
`shaders/outline.slang` carries the two outline helpers, so the two code bases
can be read side by side:

| URP / HLSL | this demo |
| --- | --- |
| `ShadeGI()` | `shadeGI()` in `toon.slang` |
| `ShadeSingleLight()`, the `smoothstep(_CelShadeMidPoint-…)` line | `shadeSingleLight()` |
| `litOrShadowArea = _IsFace? lerp(0.5,1,x) : x` | `pc.ambient_min.w` (`--headless` + "Head ignores cel band" toggle) |
| `CompositeAllLightResults()` `max(indirect, direct)` | `fragMain()` |
| `ConvertSurfaceColorToOutlineColor()` | `fragMain()` in `outline.slang` (constant colour) |
| `GetOutlineCameraFovAndDistanceFixMultiplier()` | `camera_mul_fix` in `outline.slang` |
| `NiloGetNewClipPosWithZOffset()` | the `z_offset` block in `outline.slang` |
| Pass 0 `UniversalForwardOnly` | `ToonForward` (CULL_BACK, depth write) |
| Pass 1 `Outline` (CULL_FRONT) | `Outline` (CULL_FRONT, depth read-only) |
| Passes 2–4 `ShadowCaster`/`DepthOnly`/`DepthNormalsOnly` | not ported: no shadow atlas, no prepass (see below) |

Material knobs keep the URP names in comments so a Unity user can find them:

| URP property | default here | where |
| --- | --- | --- |
| `_CelShadeMidPoint` | `0.25` (upstream `-0.5`) | `Demo.cel_mid`, `cel.x` |
| `_CelShadeSoftness` | `0.04` (upstream `0.05`) | `Demo.cel_soft`, `cel.y` |
| `_DirectLightMultiplier` | `1.1` | `Demo.direct_mul`, `light_color.w` |
| `_IndirectLightMultiplier` | `1.0` | `Demo.indirect_mul`, `shadow_color.w` |
| `_ShadowMapColor` | `(0.40, 0.35, 0.56)` | `Demo.shadow_color.rgb` |
| `_IndirectLightMinColor` | `(0.10, 0.11, 0.20)` | `Demo.ambient_min.rgb` |
| `_OutlineWidth` | `1.0` | `Demo.outline_width`, `params.x` |
| `_OutlineColor` | `(0.10, 0.08, 0.13)` | `Demo.outline_color` |
| `_OutlineZOffset` | `0.0001` | `Demo.outline_zoffset`, `params.y` |
| `_IsFace` | off (toggle) | `Demo.head_flat`, `ambient_min.w` |
| `_BaseColor` / `_BaseMap` | per-object colour + procedural checker | `ToonObject.color`, `meta.y` |

The midpoint default differs on purpose: the URP default of `-0.5` puts the
terminator so far to the dark side that the figure reads almost flat with a
single directional light. `0.25` is the same equation with a readable band.

## Pass graph

```
Backdrop     fullscreen gradient + sun   -> toon_color (clear), no depth
ToonForward  N objects, CULL_BACK        -> toon_color (keep), depth clear+write
Outline      N objects, CULL_FRONT       -> toon_color (keep), depth load, no write
Resolve      fullscreen, samples toon_color -> swapchain (or readback)
[DebugUI]    Nuklear overlay             -> swapchain (windowed only)
```

One `frame_program_build()` call, one submission, one present per frame —
`frame_begin` / `frame_run` / `frame_end`. All geometry for every object lives in
**one** `ARENA_DEVICE` slice uploaded once; each draw pushes a 256-byte constant
block (view-projection, model matrix, light, material, vertex offset) and issues
a non-indexed `pass_draw` that fetches vertices by `SV_VertexID` — the
bindless-geometry pattern from `vk.h` §17. There is no vertex-input state, no
index buffer and no per-object resource.

Toggling the outline off treats the pass as recording nothing: the graph stays
fixed, because `frame_program_build` is documented to run on configuration
change, not per frame.

## Deliberate differences from the URP shader

These are the places where the port is *not* a translation, and why:

1. **No shadow atlas.** `mu_gfx` has no directional shadow map, so
   `light.shadowAttenuation` is `1.0` and `_ReceiveShadowMappingAmount` has
   nothing to attenuate. The shadow side of the figure is carried by
   `_ShadowMapColor` exactly as in the tutorial.
2. **No light probes / SH.** URP's `SampleSH(0)` constant term is replaced by a
   hemispherical ambient built from the same two colours the backdrop uses, then
   `max()`-ed with `_IndirectLightMinColor` like upstream.
3. **No `_BaseMap` texture.** The demo ships without assets, so the object's UVs
   drive a procedural checker ("use checker" per object) instead of a sampled
   albedo. `_UseAlphaClipping`, `_EmissionMap` and `_OcclusionMap` are not
   ported; the occlusion term stays `1.0` so its position in the equation is
   preserved.
4. **Outline fragment shader is constant-coloured** instead of "compute the full
   lighting result, then throw it away" (`ConvertSurfaceColorToOutlineColor`).
5. **Rim light is an addition.** It is not in the tutorial shader; it defaults
   to `0.15` and `cel.z = 0` disables it. Everything else in `toon.slang` is
   upstream.
6. **Shadow/depth passes are absent** because there is no shadow atlas and no
   depth prepass in this renderer; `Outline` reads depth instead of writing it.
   (`demos/slang-toon` has both: see `slang_toon_host`.)
7. **Outline width is not clamped at one unit.** Upstream uses
   `saturate(abs(viewZ))`, so its outline is a constant pixel width only within
   one unit of the camera and thins out beyond; this demo uses the unclamped
   distance, which keeps the width constant on screen at any range.

## Build and run

From the repository root:

```sh
git submodule update --init --recursive   # mu_lib + GLFW, once
./build_toon.sh
```

`build_toon.sh` configures the shared CMake project into `build/toon-cmake`,
builds the `toon` target and recompiles + `spirv-val`s the shaders if `slangc`
is on `PATH` (or `-DSLANGC=/path/to/slangc`). The `.spv` files are also checked
in, so a machine without Slang still builds. To rebuild only the shaders:
`SLANGC=/path/to/slangc scripts/build_shaders.sh common toon`. GLFW, Volk and Vulkan-Headers are vendored;
no system GLFW is needed.

Windowed:

```sh
./build/toon-cmake/toon
```

Controls (positions on a US layout; the window prints the labels for *your*
layout, resolved through `mu_input.h`):

| action | default |
| --- | --- |
| orbit / pitch | `A` `D` / `Up` `Down` |
| zoom | `W` `S`, or the wheel |
| look around | right-mouse drag |
| swing the light | `Q` `E` |
| outline on/off | `O` |
| pause the animation | `P` |
| reset the material | `R` |
| debug panel | `F1` or `` ` `` |
| quit | `Esc` |

The panel edits the cel midpoint/softness, rim, light yaw/height/intensity,
outline width, z-offset, exposure, the three colours, `_IsFace` and auto-orbit.

Headless (no window, no display server needed):

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/toon-cmake/toon --headless toon.ppm --size 1280 720
```

```
--headless [out.ppm]   render one frame offscreen (default toon.ppm)
--size W H             output size, max 4096x4096
--time seconds         animation phase for the ball and crate
--cel MID SOFT         cel midpoint and softness
--light YAW ELEV       light direction in radians
--no-outline           skip the outline pass
--outline WIDTH ZOFF   _OutlineWidth and _OutlineZOffset (view-space units)
--no-orbit             freeze the camera (deterministic captures)
--validate             create the device with Vulkan validation + sync
--bindings / --help    print the action map / usage
```

The windowed frame is the same render graph: the HDR target is resolved to the
sRGB swapchain through `resolve.slang`, which also flips `v` because of vk.c's
negative-height viewport convention (documented in that file).

Headless mode renders the same graph, reads back the `FMT_RGBA16F` scene target,
applies the same exposure and sRGB encode the sRGB swapchain would, writes a
binary PPM, and drops the detailed pass profile in `demos/toon/toon_perf.txt`.

`--validate` currently reports the same backend-level messages the galaxy demo
does (push-constant `stageFlags`, bindless-image layout, buffer device address);
they are attributed to `src/vk.c`, not to this demo, and are listed in
`graphics_finding/docs/` for the API-trimming work.

## Files

| file | role |
| --- | --- |
| `main.c` | scene state, push constants, pass graph, input, headless capture |
| `scene.c` / `.h` | procedural sphere/capsule/quad/cube, object placements |
| `shaders/toon_common.slang` | module: `ToonVertex` + `toonLoadVertex()` (bindless vertex pull) |
| `shaders/toon.slang` | `vertMain` / `fragMain`: the toon lighting equation |
| `shaders/outline.slang` | `vertMain` / `fragMain`: inflated hull, FOV/distance-compensated width, depth bias |
| `shaders/sky.slang` | `fragMain`: flat anime backdrop (vertex stage is the shared `fullscreen.slang`) |
| `shaders/resolve.slang` | `fragMain`: HDR target to swapchain/readback |
| `shaders/*.spv` | checked-in SPIR-V, one file per entry point (`scripts/build_shaders.sh`) |
| `../common/shaders/` | `mu_gfx.slang` (bindless ABI module), `fullscreen.slang`, `nk.slang` (Nuklear overlay), shared with `demos/galaxy` |
| `docs/toon_outline.png`, `docs/toon_outline_off.png`, `docs/toon_soft_band.png` | headless captures (`--headless`) |
| `docs/toon_window.png` | the windowed run, incl. the Nuklear panel |
| `LICENSE` | MIT notice for the ported shader code |
