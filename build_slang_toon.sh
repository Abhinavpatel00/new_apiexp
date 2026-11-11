#!/usr/bin/env bash
# Rebuild and CPU-check the two vendored Slang toon shader packages.
#
# These are shader packages, not executables: they need a host renderer that
# implements the descriptor layout documented in each package's README.
# The SPIR-V is checked in, so this script is only needed after editing the
# Slang sources.
set -euo pipefail
cd "$(dirname "$0")"

if [[ -z "${SLANGC:-}" ]]; then
    if command -v slangc >/dev/null 2>&1; then
        SLANGC=$(command -v slangc)
    else
        cat >&2 <<'EOF'
error: slangc not found.

Set SLANGC to a Slang compiler, for example:
  SLANGC=/opt/shader-slang-bin/bin/slangc ./build_slang_toon.sh

The checked-in SPIR-V under demos/slang-toon*/spirv/ is used when you do not
rebuild, so this script is optional for building the C demos.
EOF
        exit 1
    fi
fi

# The package build scripts cd into their own directory, so a relative
# SLANGC=../toonresearch/... would break. Resolve it before exporting.
if [[ "$SLANGC" == */* ]]; then
    if command -v realpath >/dev/null 2>&1; then
        SLANGC=$(realpath "$SLANGC")
    else
        SLANGC=$(cd "$(dirname "$SLANGC")" && pwd)/$(basename "$SLANGC")
    fi
fi

export SLANGC

for dir in demos/slang-toon demos/slang-toon-inspired; do
    echo "[slang] $dir"
    ( cd "$dir" && bash build.sh )
    python3 "$dir/test_math.py"
done

echo "rebuilt demos/slang-toon and demos/slang-toon-inspired"
