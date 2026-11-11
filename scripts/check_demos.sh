#!/usr/bin/env bash
# check_demos.sh — render every demo headless and run every acceptance check.
#
#   scripts/check_demos.sh              compare with tests/reference/, run --check
#   scripts/check_demos.sh --update     rewrite tests/reference/ from this build
#   scripts/check_demos.sh --no-check   images only (the checks take ~2 minutes)
#   scripts/check_demos.sh sdf post     only the named demos
#
# BUILD=dir selects the build directory (default build/cm).
#
# The references were rendered with lavapipe (Mesa llvmpipe). Another driver
# rasterizes and rounds differently; expect mismatches of a few bytes there
# and judge them by eye before using --update.
set -u
cd "$(dirname "$0")/.."

BUILD=${BUILD:-build/cm}
REF=tests/reference
SIZE="320 180"
UPDATE=0
CHECK=1
ONLY=()
for a in "$@"; do
    case "$a" in
        --update)   UPDATE=1 ;;
        --no-check) CHECK=0 ;;
        -h|--help)  sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          ONLY+=("$a") ;;
    esac
done

# name | executable | arguments for the image | arguments for the acceptance check ("-" = none)
DEMOS=$(cat <<'TABLE'
sdf        sdf              --time 1.5                       | --check --size 640 360
post       post             --time 1                         | --check
post_cmp   post             --time 1 --compare               | -
shadows    shadows          --technique 3 --right 5          | --calibration --check
cascades   shadows          --cascades --view 2              | -
particles  particles        --count 65536 --frames 150       | --check
pbr        pbr              --maps                           | --check
pbr_furnace pbr             --furnace --energy 0             | -
deferred   deferred         --time 3                         | --check --time 3 --size 640 360
deferred_clusters deferred  --time 3 --view 5                | -
clustered  clustered        --time 3                         | --check --time 3 --size 640 360
clustered_tested clustered  --time 3 --view 1                | -
clustered_shadows clustered --time 3 --sdf-shadows           | -
clustered_fog clustered     --time 3 --fog 0.03 --lights 48 --sdf-shadows | -
sdfworld   sdfworld         --frames 120                     | --check --size 640 360
sdfworld_steps sdfworld     --still --frames 3 --view 1      | -
fluid      fluid            --frames 240                     | --check
fluid_speed fluid           --frames 240 --view 1            | -
jfa        jfa              --frames 90                      | --check
jfa_glow   jfa              --frames 90 --view 2             | -
gpu_driven gpu_driven       --frames 30                      | --check --size 640 360
gpu_driven_overview gpu_driven --frames 30 --overview       | -
oit        oit              --mode 0                         | --check --size 640 360
oit_peeled oit              --mode 1                         | -
oit_order  oit              --mode 2                         | -
taa        taa              --still --frames 40              | --check --size 640 360
taa_off    taa              --still --off                    | -
taa_moving taa              --frames 472 --spin 0.3          | -
clay       clay             --frames 240                     | --check --size 320 180
bricks     bricks           --view 0                         | --check --size 640 360 --bricks 18
bricks_map bricks           --view 3                         | -
screenspace screenspace     --mode 1                         | --check --size 640 360
screenspace_planar screenspace --mode 2                      | -
screenspace_ao screenspace  --view 1                         | -
ocean      ocean            --time 3                         | --check --grid 64
ocean_rough ocean           --time 3 --height 0.8 --chop 1.4 | -
lines      lines            --mode 2                         | --check --size 640 360
lines_hull lines            --mode 1                         | -
blackhole  blackhole        --time 2                         | --check --size 640 360
blackhole_grid blackhole    --grid --no-disc                 | -
turing     turing           --size 640 360                   | --check --size 640 360
turing_cells turing         --size 640 360 --preset 1        | -
schrodinger schrodinger     --size 640 360                   | --check --size 640 360
schrodinger_plate schrodinger --size 640 360 --frames 110    | -
mpm        mpm              --size 640 360 --frames 140      | --check --size 640 360
mpm_speed  mpm              --size 640 360 --frames 60 --view 1 | -
pat        pat              --size 640 360                   | --check --size 640 360
pat_planes pat              --size 640 360 --blend 1         | -
pat_cut    pat              --size 640 360 --cloud 0 --combine 3 --other 1 --dots | -
pat_slice  pat              --size 640 360 --cloud 3 --view 1 | -
snow       snow             --size 640 360                   | --check --size 640 360
snow_meet  snow             --size 640 360 --scene 1 --frames 25 --view 1 | -
snow_slab  snow             --size 640 360 --scene 2 --frames 40 --view 2 | -
snow_plough snow            --size 640 360 --scene 3 --frames 60 --view 2 | -
snow_sticky snow            --size 640 360 --frames 30 --sticky --implicit | -
xpbd       xpbd             --size 640 360 --frames 150      | --check --size 320 180
xpbd_chain xpbd             --size 640 360 --scene 1 --frames 150 --pbd | -
xpbd_block xpbd             --size 640 360 --scene 2 --frames 150 | -
projdyn    projdyn          --size 640 360 --frames 60       | --check --size 320 180
projdyn_twist projdyn       --size 640 360 --scene 1 --frames 200 --volume 6 | -
seiler     seiler           --size 640 360 --at 0.6          | --check --size 320 180
seiler_spline seiler        --size 640 360 --scene 1 --at 0.42 | -
avbd       avbd             --size 640 360 --frames 200      | --check --size 320 180
avbd_springs avbd           --size 640 360 --scene 2 --frames 300 --iterations 5 | -
absnewton  absnewton        --size 640 360 --frames 44       | --check --size 320 180
waveparticles waveparticles  --size 640 360 --frames 420      | --check --size 320 180
mergesplit  mergesplit     --size 640 360 --frames 50       | --check --size 320 180
ipbf        ipbf           --size 640 360 --frames 50       | --check --size 320 180
sampleelim  sampleelim     --size 640 360 --frames 130      | --check --size 320 180
gogreen     gogreen        --size 640 360 --frames 63       | --check --size 320 180
c2spline    c2spline       --size 640 360 --frames 90       | --check --size 320 180
catmullrom  catmullrom     --size 640 360 --frames 60       | --check --size 320 180
polyroots   polyroots      --size 640 360 --frames 70       | --check --size 320 180 --frames 3
pointlight  pointlight     --size 640 360 --time 1          | --check --size 320 180
pointlight_placed pointlight --size 640 360 --scene 1 --time 2 | -
wildmesh    wildmesh       --size 640 360 --frames 2 --keep 3 | --check --size 320 180 --frames 2
wildmesh_soup wildmesh     --size 640 360 --frames 2 --scene 1 --keep 4 --baseline 1 --wire | -
wildmesh_cloth wildmesh    --size 640 360 --frames 2 --scene 2 --keep 5 --baseline 2 | -
softloco    softloco       --size 640 360 --frames 2 --skip 150 | --check --size 320 180 --frames 2
softloco_crawl softloco    --size 640 360 --frames 2 --scene 1 --skip 400 | -
softloco_roll softloco     --size 640 360 --frames 2 --scene 2 --skip 170 | -
contact     contact        --size 640 360 --frames 2 --skip 90 | --check --size 320 180 --frames 2
contact_pucks contact      --size 640 360 --frames 2 --scene 1 --skip 150 | -
contact_ramp contact       --size 640 360 --frames 2 --scene 2 --skip 100 | -
contact_pit contact        --size 640 360 --frames 2 --scene 3 --skip 300 | -
koopman     koopman        --size 640 360 --frames 60 | --check --size 320 180 --frames 2
koopman_forces koopman     --size 640 360 --frames 100 --scene 1 | -
koopman_damping koopman    --size 640 360 --frames 60 --scene 2 | -
koopman_control koopman    --size 640 360 --frames 70 --scene 3 | -
toon       toon                                              | -
galaxy     galaxy           --time 3                         | -
slang_toon slang_toon_host                                   | -
TABLE
)

wanted() {
    [ ${#ONLY[@]} -eq 0 ] && return 0
    local n
    for n in "${ONLY[@]}"; do [ "$n" = "$1" ] && return 0; done
    return 1
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$REF"
failed=0
ran=0

while IFS= read -r line; do
    [ -z "$line" ] && continue
    image_part=${line%%|*}
    check_args=${line#*|}
    read -r name exe image_args <<<"$image_part"
    wanted "$name" || continue
    ran=$((ran + 1))
    if [ ! -x "$BUILD/$exe" ]; then
        printf '%-11s MISSING   %s/%s is not built\n' "$name" "$BUILD" "$exe"
        failed=$((failed + 1))
        continue
    fi

    out="$tmp/$name.ppm"
    # shellcheck disable=SC2086
    if ! "$BUILD/$exe" --headless "$out" --size $SIZE $image_args >"$tmp/$name.log" 2>&1 || [ ! -s "$out" ]; then
        printf '%-11s FAILED    could not render; last lines:\n' "$name"
        tail -n 5 "$tmp/$name.log" | sed 's/^/    /'
        failed=$((failed + 1))
        continue
    fi
    if [ "$UPDATE" -eq 1 ]; then
        cp "$out" "$REF/$name.ppm"
        printf '%-11s updated   %s/%s.ppm\n' "$name" "$REF" "$name"
    elif [ ! -f "$REF/$name.ppm" ]; then
        printf '%-11s NO REFERENCE (run with --update)\n' "$name"
        failed=$((failed + 1))
    else
        result=$(python3 scripts/compare_ppm.py "$REF/$name.ppm" "$out")
        status=$?
        printf '%-11s %s\n' "$name" "$result"
        [ "$status" -ne 0 ] && failed=$((failed + 1))
    fi

    check_args=$(echo "$check_args" | sed 's/^ *//; s/ *$//')
    if [ "$CHECK" -eq 1 ] && [ "$check_args" != "-" ]; then
        # shellcheck disable=SC2086
        if "$BUILD/$exe" $check_args >"$tmp/$name.check" 2>&1; then
            printf '%-11s check     PASS (%s assertions)\n' "$name" "$(grep -c ' PASS$' "$tmp/$name.check")"
        else
            printf '%-11s check     FAIL\n' "$name"
            grep -E 'FAIL|error' "$tmp/$name.check" | sed 's/^/    /'
            failed=$((failed + 1))
        fi
    fi
done <<<"$DEMOS"

# Demos and tests rewrite these tracked reports as a side effect.
git checkout -q -- demos/galaxy/galaxy_perf.txt demos/toon/toon_perf.txt 2>/dev/null || true

if [ "$ran" -eq 0 ]; then
    echo "no demo matches: ${ONLY[*]}"
    exit 2
fi
if [ "$failed" -ne 0 ]; then
    echo "$failed problem(s)"
    exit 1
fi
[ "$UPDATE" -eq 1 ] && echo "references updated" || echo "all demos match their references"
