# Shaders are Slang

Every shader in the repository is Slang. The GLSL sources under
`demos/toon/shaders` and `demos/galaxy/shaders` (16 files, 14 distinct — the
Nuklear pair was duplicated) were ported 1:1 and removed.

## Layout

```
demos/common/shaders/mu_gfx.slang      module: the vk.h bindless ABI + helpers
demos/common/shaders/fullscreen.slang  vertFullscreen (shared by both demos)
demos/common/shaders/nk.slang          vertNk / fragNk (Nuklear overlay)
demos/toon/shaders/toon_common.slang   module: ToonVertex + toonLoadVertex()
demos/toon/shaders/{toon,outline,sky,resolve}.slang
demos/galaxy/shaders/{galaxy,sky,tonemap}.slang
scripts/build_shaders.sh               manifest + build + spirv-val + --check
```

One `.slang` holds the stages that belong together (`toon.slang` has `vertMain`
and `fragMain`, so the varyings struct is declared once). Each entry point is
compiled to its own `.spv`; the output names are unchanged, so no C code that
loads shaders moved except the Nuklear path, which both demos now read from
`demos/common/shaders/`.

## Build

```sh
SLANGC=/path/to/slangc scripts/build_shaders.sh          # all groups
SLANGC=/path/to/slangc scripts/build_shaders.sh toon     # one group
SLANGC=/path/to/slangc scripts/build_shaders.sh --check  # fail if a .spv is stale
```

CMake targets `toon_shaders`, `galaxy_shaders` and `shaders` call the script when
`slangc` is found (`-DSLANGC=...`), and print a note otherwise: the SPIR-V is
checked in, so building the demos does not need Slang. `glslangValidator` is no
longer used anywhere. Built with Slang 2026.21; `--check` is byte-exact, so it
is tied to that compiler version.

Flags: `-target spirv -profile spirv_1_3 -I demos/common/shaders -I
external/mu_lib/mu`. No `-matrix-layout-*` flag — see below.

## Porting rules that were applied

| GLSL | Slang | Why |
|---|---|---|
| `mat4` in a push block, `M * v` | `column_major float4x4`, `mul(M, v)` | the C side memcpy's column-major `mu_mat4`; this pair reads the same bytes and multiplies the same way, with no global matrix-layout flag |
| `gl_VertexIndex` / `gl_InstanceIndex` | `SV_VertexID` / `SV_InstanceID` | Slang subtracts the base vertex/instance (HLSL semantics). The demos draw with base 0, so the values are equal; a draw with a non-zero base would differ |
| `layout(std430) buffer { T v[]; }` indexed by `offset / stride + i` | `ByteAddressBuffer.Load<T>(offset + i * stride)` | removes a latent bug: the typed view needed `offset % stride == 0`, and `arena_alloc` aligns to 64, not 48. It only worked because the toon vertices sat at offset 0 |
| `mod(x, y)` | `muMod` (`x - y * floor(x / y)`) | HLSL/Slang `fmod` takes the sign of `x`, GLSL `mod` the sign of `y` |
| `fract`, `mix`, `texture`, `vecN` | `frac`, `lerp`, `Sample`, `floatN` | spelling |
| `void main()` | named entry, `-entry X -stage S` | slangc exports the SPIR-V entry as `main`, which `shader_load()` expects |
| two-file `in`/`out` with matching `location`s | one varyings struct with `[[vk::location(n)]]` | cannot drift between stages |

## Verification

Renders with the GLSL-built SPIR-V and the Slang-built SPIR-V, same binary,
lavapipe:

| Capture | Differing pixels | Max difference |
|---|---|---|
| toon 1280×720 default | 0 | 0 |
| toon 800×450 `--cel 0.0 0.35 --no-orbit` | 0 | 0 |
| toon 800×450 `--no-outline --time 2.5` | 0 | 0 |
| toon 1024×1024 `--light 2.2 0.4 --time 7` | 0 | 0 |
| galaxy 1280×720 `--time 3` | 13 (0.0014 %) | 1/255 |
| galaxy 800×450 `--time 0` | 4 | 1/255 |
| galaxy 800×450 `--time 42.5 --stars 30000 --dust 5000` | 0 | 0 |
| galaxy 800×450 `--time 3` | 5 | 1/255 |
| Nuklear panel region, windowed (Xvfb), 208 500 px | 0 | 0 |

The galaxy differences are single least-significant-bit changes in additive HDR
star accumulation (operation order differs between the two compilers); each
build is deterministic run to run. All stages pass `spirv-val --target-env
vulkan1.3`. `tests/smoke` passes.

Not covered: the Vulkan validation layers are not installed in the environment
this was done in, so `--validate` was not exercised; and nothing was run on a
hardware GPU.
