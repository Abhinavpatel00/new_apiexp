# Procedural galaxy demo

This demo exercises the current `mu_gfx` API. It renders an analytic four-arm
spiral galaxy over a procedural deep-space sky into an HDR target, then tone maps
to the window or headless output. Star, dust, and core billboards are generated
from instance IDs in the vertex shader; there is no particle simulation or
per-frame instance upload. The shader applies radius-dependent differential
rotation and twinkling.

## Source layout

| file | role |
| --- | --- |
| `main.c` | scene state, recording, CLI, frame loop; no windowing or key codes |
| `../common/platform.c`/`.h` | the only GLFW file: window lifecycle, event pump, gamepad edge synthesis, WSI surface, name↔scancode mapping |

`main.c` never includes GLFW, and never names a key code. The action enum
(`ACT_ZOOM_IN`, `ACT_ORBIT_LEFT`, ...) and `k_default_bindings` — a table of
*control names* like `"W"` and `"MouseRight"` — are the demo's whole input
surface; the mechanics live in `external/mu_lib/mu/mu_input.h`.

`platform_poll()` is the single definition of a frame boundary: pump events →
push them into the device state → `mu_input_frame_begin` → `mu_input_resolve`.
After that, `platform_state()` gives raw device truth (`dt`, `wheel_y`,
`mouse_dx/dy`) and `platform_actions()` gives the per-frame action snapshot.

Bindings print from the map, not from a hardcoded list: on startup the demo
resolves each binding through the user's keyboard layout (so `W` shows as `Z` on
AZERTY), and `--bindings`/`--help` print the declared names, which needs no
display.

## Note on the present path

`tonemap.slang` flips the sampled `v` coordinate (`muFlipV` from the shared
`mu_gfx` module). `src/vk.c` sets a
negative-height viewport, so in a full-screen triangle's UV space `v = 1` is the
top of the displayed image, while the HDR target's row 0 is `v = 0`; without the
flip the window presented the frame vertically mirrored relative to the headless
readback of the same target.

## Build

From the repository root, initialize the vendored dependencies and build:

```sh
git submodule update --init --recursive
./build_galaxy.sh
```

The build expects a C compiler, CMake, Vulkan runtime/loader, and GLFW's native
window-system development dependencies. On Debian/Ubuntu with X11, install
`cmake`, `libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`,
`libxi-dev`. Rebuilding shader binaries needs `slangc` (Slang) and, optionally,
`spirv-val` from `spirv-tools`: `SLANGC=/path/to/slangc scripts/build_shaders.sh common galaxy`. GLFW 3.4 is
vendored as `external/glfw`; no system GLFW library is required. Precompiled
Vulkan 1.3 SPIR-V files are included under `shaders/`.

## Run interactively

```sh
./build/galaxy-cmake/galaxy
```

Controls:

- **W / S**: zoom; **A / D**: orbit horizontally; **Up / Down**: orbit vertically
- **Right mouse drag**: look around; **scroll wheel**: zoom
- **[ / ]**: decrease/increase differential-rotation speed; **P** pause; **Space** resume
- **R** reseed the analytic distribution; **- / +** adjust brightness; **Esc** quit

The demo prints device and timing information. Pass timestamps are automatic;
star, dust, and core GPU timing scopes are separately named. The frame report
includes annotations and declared logical-byte estimates where supplied—these
are not claims about physical memory traffic.

## Render headlessly

No window or display server is used in headless mode. The API renders once to
its internal swapchain target, reads back the HDR intermediate, applies the
same exponential tone curve and sRGB transfer, and writes a binary PPM image:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/galaxy-cmake/galaxy --headless galaxy.ppm --size 1280 720
```

Options:

```text
--headless [output.ppm]   render a single image (default file: galaxy.ppm)
--size W H                output/window size (maximum 4096 by 4096)
--time seconds            fixed galaxy animation time for headless output
--stars count             star billboards (default 120000)
--dust count              dust billboards (default 14000)
--bindings                print the action-to-control map and exit
--help                    show usage and the control list
```

Headless mode also writes the latest detailed GPU profile to
`demos/galaxy/galaxy_perf.txt`. For a smoke render on Lavapipe:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/galaxy-cmake/galaxy --headless build/galaxy.ppm --size 320 180
```
