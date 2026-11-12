#!/usr/bin/env bash
# Compile the demo shaders (Slang -> SPIR-V) and validate them.
#
#   scripts/build_shaders.sh              build every group
#   scripts/build_shaders.sh toon galaxy  build only the named groups
#   scripts/build_shaders.sh --check      rebuild into a temp dir and fail if the
#                                         checked-in .spv files are stale
#   scripts/build_shaders.sh --list       print the manifest
#
# Every shader in demos/ is Slang. One .slang file may hold several entry
# points; each entry point becomes one .spv whose SPIR-V entry is named "main"
# (slangc's default), which is what shader_load() in src/vk.h expects.
#
# The SPIR-V is checked in, so slangc is only needed after editing a shader.
# Set SLANGC=/path/to/slangc or put slangc on PATH. The checked-in binaries
# were produced with Slang 2026.21.
#
# tests/smoke_*.slang are not in the manifest: they were already Slang, and
# their checked-in binaries predate this script.
#
# The two shader *packages* (demos/slang-toon, demos/slang-toon-inspired) keep
# their own build.sh because they are meant to be vendored on their own; the
# "slang-toon" group below simply runs them.
set -euo pipefail
cd "$(dirname "$0")/.."

# group | source (relative to repo root) | entry | stage | output
MANIFEST='
common  demos/common/shaders/nk.slang          vertNk          vertex    demos/common/shaders/nk.vert.spv
common  demos/common/shaders/nk.slang          fragNk          fragment  demos/common/shaders/nk.frag.spv
common  demos/common/shaders/fullscreen.slang  vertFullscreen  vertex    demos/common/shaders/fullscreen.vert.spv
common  demos/common/shaders/present.slang     fragPresent     fragment  demos/common/shaders/present.frag.spv
common  demos/common/shaders/canvas.slang      vertCanvas      vertex    demos/common/shaders/canvas.vert.spv
common  demos/common/shaders/canvas.slang      fragCanvas      fragment  demos/common/shaders/canvas.frag.spv
toon    demos/common/shaders/fullscreen.slang  vertFullscreen  vertex    demos/toon/shaders/sky.vert.spv
toon    demos/toon/shaders/sky.slang           fragMain        fragment  demos/toon/shaders/sky.frag.spv
toon    demos/toon/shaders/toon.slang          vertMain        vertex    demos/toon/shaders/toon.vert.spv
toon    demos/toon/shaders/toon.slang          fragMain        fragment  demos/toon/shaders/toon.frag.spv
toon    demos/toon/shaders/outline.slang       vertMain        vertex    demos/toon/shaders/outline.vert.spv
toon    demos/toon/shaders/outline.slang       fragMain        fragment  demos/toon/shaders/outline.frag.spv
toon    demos/toon/shaders/resolve.slang       fragMain        fragment  demos/toon/shaders/resolve.frag.spv
galaxy  demos/common/shaders/fullscreen.slang  vertFullscreen  vertex    demos/galaxy/shaders/fullscreen.vert.spv
galaxy  demos/galaxy/shaders/sky.slang         fragMain        fragment  demos/galaxy/shaders/sky.frag.spv
galaxy  demos/galaxy/shaders/galaxy.slang      vertMain        vertex    demos/galaxy/shaders/galaxy.vert.spv
galaxy  demos/galaxy/shaders/galaxy.slang      fragMain        fragment  demos/galaxy/shaders/galaxy.frag.spv
galaxy  demos/galaxy/shaders/tonemap.slang     fragMain        fragment  demos/galaxy/shaders/tonemap.frag.spv
sdf     demos/sdf/shaders/sdf.slang            fragTrace       fragment  demos/sdf/shaders/trace.frag.spv
post    demos/post/shaders/post.slang          fragScene       fragment  demos/post/shaders/scene.frag.spv
post    demos/post/shaders/post.slang          fragDown        fragment  demos/post/shaders/down.frag.spv
post    demos/post/shaders/post.slang          fragUp          fragment  demos/post/shaders/up.frag.spv
post    demos/post/shaders/post.slang          fragComposite   fragment  demos/post/shaders/composite.frag.spv
shadows demos/shadows/shaders/shadows.slang    vertShadow      vertex    demos/shadows/shaders/shadow.vert.spv
shadows demos/shadows/shaders/shadows.slang    vertMain        vertex    demos/shadows/shaders/main.vert.spv
shadows demos/shadows/shaders/shadows.slang    fragMain        fragment  demos/shadows/shaders/main.frag.spv
particles demos/particles/shaders/simulate.slang compSimulate   compute   demos/particles/shaders/simulate.comp.spv
particles demos/particles/shaders/draw.slang    vertDraw        vertex    demos/particles/shaders/draw.vert.spv
particles demos/particles/shaders/draw.slang    fragDraw        fragment  demos/particles/shaders/draw.frag.spv
pbr     demos/pbr/shaders/bake.slang           compSky         compute   demos/pbr/shaders/sky.comp.spv
pbr     demos/pbr/shaders/bake.slang           compPrefilter   compute   demos/pbr/shaders/prefilter.comp.spv
pbr     demos/pbr/shaders/bake.slang           compIrradiance  compute   demos/pbr/shaders/irradiance.comp.spv
pbr     demos/pbr/shaders/bake.slang           compDfg         compute   demos/pbr/shaders/dfg.comp.spv
pbr     demos/pbr/shaders/pbr.slang            fragSky         fragment  demos/pbr/shaders/sky.frag.spv
pbr     demos/pbr/shaders/pbr.slang            vertMain        vertex    demos/pbr/shaders/main.vert.spv
pbr     demos/pbr/shaders/pbr.slang            fragMain        fragment  demos/pbr/shaders/main.frag.spv
deferred demos/deferred/shaders/cluster.slang  compCluster     compute   demos/deferred/shaders/cluster.comp.spv
deferred demos/deferred/shaders/deferred.slang vertMain        vertex    demos/deferred/shaders/main.vert.spv
deferred demos/deferred/shaders/deferred.slang fragGBuffer     fragment  demos/deferred/shaders/gbuffer.frag.spv
deferred demos/deferred/shaders/deferred.slang fragLighting    fragment  demos/deferred/shaders/lighting.frag.spv
deferred demos/deferred/shaders/deferred.slang fragForward     fragment  demos/deferred/shaders/forward.frag.spv
clustered demos/clustered/shaders/bin.slang      compBin        compute   demos/clustered/shaders/bin.comp.spv
clustered demos/clustered/shaders/bake.slang     compBake       compute   demos/clustered/shaders/bake.comp.spv
clustered demos/clustered/shaders/fog.slang      compInject     compute   demos/clustered/shaders/fog_inject.comp.spv
clustered demos/clustered/shaders/fog.slang      compIntegrate  compute   demos/clustered/shaders/fog_integrate.comp.spv
clustered demos/clustered/shaders/fog.slang      fragFog        fragment  demos/clustered/shaders/fog.frag.spv
clustered demos/clustered/shaders/clustered.slang vertMain      vertex    demos/clustered/shaders/main.vert.spv
clustered demos/clustered/shaders/clustered.slang fragForward   fragment  demos/clustered/shaders/main.frag.spv
sdfworld demos/sdfworld/shaders/edit.slang    compEdit       compute   demos/sdfworld/shaders/edit.comp.spv
sdfworld demos/sdfworld/shaders/coarse.slang  compOccupied   compute   demos/sdfworld/shaders/occupied.comp.spv
sdfworld demos/sdfworld/shaders/coarse.slang  compReach      compute   demos/sdfworld/shaders/reach.comp.spv
sdfworld demos/sdfworld/shaders/sim.slang     compPick       compute   demos/sdfworld/shaders/pick.comp.spv
sdfworld demos/sdfworld/shaders/sim.slang     compBalls      compute   demos/sdfworld/shaders/balls.comp.spv
sdfworld demos/sdfworld/shaders/world.slang   fragPrepass    fragment  demos/sdfworld/shaders/prepass.frag.spv
sdfworld demos/sdfworld/shaders/world.slang   fragTrace      fragment  demos/sdfworld/shaders/trace.frag.spv
fluid    demos/fluid/shaders/fluid.slang      compAdvect     compute   demos/fluid/shaders/advect.comp.spv
fluid    demos/fluid/shaders/fluid.slang      compDivergence compute   demos/fluid/shaders/divergence.comp.spv
fluid    demos/fluid/shaders/fluid.slang      compJacobi     compute   demos/fluid/shaders/jacobi.comp.spv
fluid    demos/fluid/shaders/fluid.slang      compRelax      compute   demos/fluid/shaders/relax.comp.spv
fluid    demos/fluid/shaders/fluid.slang      compProject    compute   demos/fluid/shaders/project.comp.spv
fluid    demos/fluid/shaders/fluid.slang      compCopy       compute   demos/fluid/shaders/copy.comp.spv
fluid    demos/fluid/shaders/fluid.slang      fragShow       fragment  demos/fluid/shaders/show.frag.spv
jfa      demos/jfa/shaders/jfa.slang          compInit       compute   demos/jfa/shaders/init.comp.spv
jfa      demos/jfa/shaders/jfa.slang          compFlood      compute   demos/jfa/shaders/flood.comp.spv
jfa      demos/jfa/shaders/jfa.slang          fragShow       fragment  demos/jfa/shaders/show.frag.spv
gpu_driven demos/gpu_driven/shaders/cull.slang    compReset  compute   demos/gpu_driven/shaders/reset.comp.spv
gpu_driven demos/gpu_driven/shaders/cull.slang    compCull   compute   demos/gpu_driven/shaders/cull.comp.spv
gpu_driven demos/gpu_driven/shaders/pyramid.slang compReduce compute   demos/gpu_driven/shaders/reduce.comp.spv
gpu_driven demos/gpu_driven/shaders/draw.slang    vertDraw   vertex    demos/gpu_driven/shaders/draw.vert.spv
gpu_driven demos/gpu_driven/shaders/draw.slang    fragDraw   fragment  demos/gpu_driven/shaders/draw.frag.spv
oit      demos/oit/shaders/oit.slang          vertMain       vertex    demos/oit/shaders/main.vert.spv
oit      demos/oit/shaders/oit.slang          fragOpaque     fragment  demos/oit/shaders/opaque.frag.spv
oit      demos/oit/shaders/oit.slang          fragAccum      fragment  demos/oit/shaders/accum.frag.spv
oit      demos/oit/shaders/oit.slang          fragReveal     fragment  demos/oit/shaders/reveal.frag.spv
oit      demos/oit/shaders/oit.slang          fragComposite  fragment  demos/oit/shaders/composite.frag.spv
oit      demos/oit/shaders/oit.slang          fragPeel       fragment  demos/oit/shaders/peel.frag.spv
oit      demos/oit/shaders/oit.slang          fragOver       fragment  demos/oit/shaders/over.frag.spv
oit      demos/oit/shaders/oit.slang          fragBlend      fragment  demos/oit/shaders/blend.frag.spv
taa      demos/taa/shaders/taa.slang          vertMain       vertex    demos/taa/shaders/main.vert.spv
taa      demos/taa/shaders/taa.slang          fragScene      fragment  demos/taa/shaders/scene.frag.spv
taa      demos/taa/shaders/taa.slang          fragResolve    fragment  demos/taa/shaders/resolve.frag.spv
clay     demos/clay/shaders/sim.slang             compClear      compute   demos/clay/shaders/clear.comp.spv
clay     demos/clay/shaders/sim.slang             compPredict    compute   demos/clay/shaders/predict.comp.spv
clay     demos/clay/shaders/sim.slang             compBin        compute   demos/clay/shaders/bin.comp.spv
clay     demos/clay/shaders/sim.slang             compSort       compute   demos/clay/shaders/sort.comp.spv
clay     demos/clay/shaders/sim.slang             compSolve      compute   demos/clay/shaders/solve.comp.spv
clay     demos/clay/shaders/sim.slang             compFinish     compute   demos/clay/shaders/finish.comp.spv
clay     demos/clay/shaders/bake.slang            compBake       compute   demos/clay/shaders/bake.comp.spv
clay     demos/clay/shaders/draw.slang            fragDraw       fragment  demos/clay/shaders/draw.frag.spv
flightsim  demos/flightsim/shaders/world.slang     vertTerrain    vertex    demos/flightsim/shaders/terrain.vert.spv
flightsim  demos/flightsim/shaders/world.slang     fragTerrain    fragment  demos/flightsim/shaders/terrain.frag.spv
flightsim  demos/flightsim/shaders/world.slang     vertWater      vertex    demos/flightsim/shaders/water.vert.spv
flightsim  demos/flightsim/shaders/world.slang     fragWater      fragment  demos/flightsim/shaders/water.frag.spv
flightsim  demos/flightsim/shaders/world.slang     fragSky        fragment  demos/flightsim/shaders/sky.frag.spv
flightsim  demos/flightsim/shaders/world.slang     vertAircraft   vertex    demos/flightsim/shaders/aircraft.vert.spv
flightsim  demos/flightsim/shaders/world.slang     fragAircraft   fragment  demos/flightsim/shaders/aircraft.frag.spv
flightsim  demos/flightsim/shaders/hud.slang        fragHud        fragment  demos/flightsim/shaders/hud.frag.spv
flightsim  demos/flightsim/shaders/resolve.slang    fragResolve    fragment  demos/flightsim/shaders/resolve.frag.spv
flightsim  demos/flightsim/shaders/bake_volume.slang compBakeVolume compute   demos/flightsim/shaders/bake_volume.comp.spv
flightsim  demos/flightsim/shaders/bake_weather.slang compWeather   compute   demos/flightsim/shaders/bake_weather.comp.spv
flightsim  demos/flightsim/shaders/clouds.slang     compClouds     compute   demos/flightsim/shaders/clouds.comp.spv
bricks   demos/bricks/shaders/build.slang         compClassify   compute   demos/bricks/shaders/classify.comp.spv
bricks   demos/bricks/shaders/build.slang         compAllocate   compute   demos/bricks/shaders/allocate.comp.spv
bricks   demos/bricks/shaders/build.slang         compBricks     compute   demos/bricks/shaders/bricks.comp.spv
bricks   demos/bricks/shaders/build.slang         compDense      compute   demos/bricks/shaders/dense.comp.spv
bricks   demos/bricks/shaders/draw.slang          fragDraw       fragment  demos/bricks/shaders/draw.frag.spv
screenspace demos/screenspace/shaders/screenspace.slang vertMain       vertex    demos/screenspace/shaders/main.vert.spv
screenspace demos/screenspace/shaders/screenspace.slang fragScene      fragment  demos/screenspace/shaders/scene.frag.spv
screenspace demos/screenspace/shaders/screenspace.slang fragAO         fragment  demos/screenspace/shaders/ao.frag.spv
screenspace demos/screenspace/shaders/screenspace.slang fragBlur       fragment  demos/screenspace/shaders/blur.frag.spv
screenspace demos/screenspace/shaders/screenspace.slang fragSSR        fragment  demos/screenspace/shaders/ssr.frag.spv
screenspace demos/screenspace/shaders/screenspace.slang fragPlanar     fragment  demos/screenspace/shaders/planar.frag.spv
screenspace demos/screenspace/shaders/screenspace.slang fragComposite  fragment  demos/screenspace/shaders/composite.frag.spv
ocean     demos/ocean/shaders/ocean.slang     compSpectrum   compute   demos/ocean/shaders/spectrum.comp.spv
ocean     demos/ocean/shaders/ocean.slang     compFFT        compute   demos/ocean/shaders/fft.comp.spv
ocean     demos/ocean/shaders/ocean.slang     vertOcean      vertex    demos/ocean/shaders/ocean.vert.spv
ocean     demos/ocean/shaders/ocean.slang     fragOcean      fragment  demos/ocean/shaders/ocean.frag.spv
lines     demos/lines/shaders/lines.slang     vertMain       vertex    demos/lines/shaders/main.vert.spv
lines     demos/lines/shaders/lines.slang     fragScene      fragment  demos/lines/shaders/scene.frag.spv
lines     demos/lines/shaders/lines.slang     vertHull       vertex    demos/lines/shaders/hull.vert.spv
lines     demos/lines/shaders/lines.slang     fragHull       fragment  demos/lines/shaders/hull.frag.spv
lines     demos/lines/shaders/lines.slang     fragEdges      fragment  demos/lines/shaders/edges.frag.spv
blackhole demos/blackhole/shaders/blackhole.slang fragTrace   fragment  demos/blackhole/shaders/trace.frag.spv
pointlight demos/pointlight/shaders/pointlight.slang fragLight fragment demos/pointlight/shaders/light.frag.spv
turing    demos/turing/shaders/turing.slang   compSeed       compute   demos/turing/shaders/seed.comp.spv
turing    demos/turing/shaders/turing.slang   compStep       compute   demos/turing/shaders/step.comp.spv
turing    demos/turing/shaders/turing.slang   fragShade      fragment  demos/turing/shaders/shade.frag.spv
schrodinger demos/schrodinger/shaders/schrodinger.slang compSeed  compute   demos/schrodinger/shaders/seed.comp.spv
schrodinger demos/schrodinger/shaders/schrodinger.slang compStep  compute   demos/schrodinger/shaders/step.comp.spv
schrodinger demos/schrodinger/shaders/schrodinger.slang fragShade fragment  demos/schrodinger/shaders/shade.frag.spv
mpm       demos/mpm/shaders/mpm.slang         compClear      compute   demos/mpm/shaders/clear.comp.spv
mpm       demos/mpm/shaders/mpm.slang         compP2G        compute   demos/mpm/shaders/p2g.comp.spv
mpm       demos/mpm/shaders/mpm.slang         compG2P        compute   demos/mpm/shaders/g2p.comp.spv
mpm       demos/mpm/shaders/draw.slang        vertDraw       vertex    demos/mpm/shaders/draw.vert.spv
mpm       demos/mpm/shaders/draw.slang        fragDraw       fragment  demos/mpm/shaders/draw.frag.spv
pat       demos/pat/shaders/bake.slang        compBake       compute   demos/pat/shaders/bake.comp.spv
pat       demos/pat/shaders/draw.slang        fragDraw       fragment  demos/pat/shaders/draw.frag.spv
pat       demos/pat/shaders/draw.slang        vertDots       vertex    demos/pat/shaders/dots.vert.spv
pat       demos/pat/shaders/draw.slang        fragDots       fragment  demos/pat/shaders/dots.frag.spv
snow      demos/snow/shaders/snow.slang       compClear      compute   demos/snow/shaders/clear.comp.spv
snow      demos/snow/shaders/snow.slang       compP2G        compute   demos/snow/shaders/p2g.comp.spv
snow      demos/snow/shaders/snow.slang       compG2P        compute   demos/snow/shaders/g2p.comp.spv
snow      demos/snow/shaders/draw.slang       vertDraw       vertex    demos/snow/shaders/draw.vert.spv
snow      demos/snow/shaders/draw.slang       fragDraw       fragment  demos/snow/shaders/draw.frag.spv
'

# 41012 = "profile implicitly upgraded": the bindless descriptor arrays need the
# descriptor-indexing capabilities on top of spirv_1_3, which vk.c enables.
# 39001 = "explicit binding overlap": demos/common/shaders/mu_depth.slang
# re-declares the image and sampler tables with depth/comparison types.
#
# Include roots: demos/common/shaders for `import mu_gfx;`, and
# external/mu_lib/mu so a demo can `#include "shaders/lighting.slang"` (the
# mu_lib shader library: lighting, shadows, noise, sdf, post, ...).
SLANG_FLAGS=(-target spirv -profile spirv_1_3 -I demos/common/shaders -I external/mu_lib/mu -warnings-disable 41012,39001)

check=0
groups=()
for arg in "$@"; do
    case "$arg" in
        --check) check=1 ;;
        --list)  printf '%s\n' "$MANIFEST" | sed '/^$/d'; exit 0 ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) groups+=("$arg") ;;
    esac
done

if [[ -z "${SLANGC:-}" ]]; then
    if command -v slangc >/dev/null 2>&1; then
        SLANGC=$(command -v slangc)
    else
        cat >&2 <<'MSG'
error: slangc not found.

Set SLANGC to a Slang compiler, for example:
  SLANGC=/opt/slang/bin/slangc scripts/build_shaders.sh

The checked-in .spv files are used when you do not rebuild, so this script is
only needed after editing a .slang file.
MSG
        exit 1
    fi
fi
if [[ "$SLANGC" == */* ]]; then
    SLANGC=$(cd "$(dirname "$SLANGC")" && pwd)/$(basename "$SLANGC")
fi
export SLANGC

want() {
    [[ ${#groups[@]} -eq 0 ]] && return 0
    local g
    for g in "${groups[@]}"; do [[ "$g" == "$1" ]] && return 0; done
    return 1
}

tmp=""
if [[ $check -eq 1 ]]; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
fi

built=0
stale=0
while read -r group src entry stage out; do
    [[ -z "${group:-}" ]] && continue
    want "$group" || continue
    dst="$out"
    [[ $check -eq 1 ]] && dst="$tmp/$(echo "$out" | tr '/' '_')"
    "$SLANGC" "$src" "${SLANG_FLAGS[@]}" -entry "$entry" -stage "$stage" -o "$dst"
    if command -v spirv-val >/dev/null 2>&1; then
        spirv-val --target-env vulkan1.3 "$dst"
    fi
    if [[ $check -eq 1 ]]; then
        if ! cmp -s "$dst" "$out"; then
            echo "STALE  $out  (rebuild: scripts/build_shaders.sh $group)" >&2
            stale=$((stale + 1))
        fi
    else
        printf '  %-8s %-42s %-15s -> %s\n' "$group" "$src" "$entry" "$out"
    fi
    built=$((built + 1))
done <<<"$MANIFEST"

if want slang-toon && [[ $check -eq 0 ]]; then
    for dir in demos/slang-toon demos/slang-toon-inspired; do
        echo "  slang-toon package: $dir"
        ( cd "$dir" && bash build.sh )
    done
fi

if [[ $check -eq 1 ]]; then
    if [[ $stale -ne 0 ]]; then
        echo "$stale of $built checked-in SPIR-V file(s) are stale" >&2
        exit 1
    fi
    echo "all $built checked-in SPIR-V files match their Slang sources"
else
    command -v spirv-val >/dev/null 2>&1 || echo "note: spirv-val not found; SPIR-V was not validated"
    echo "built $built shader stage(s) with $("$SLANGC" -v 2>&1 | head -1)"
fi
