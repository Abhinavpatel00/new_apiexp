#!/usr/bin/env bash
# Build the toon demo (and its SPIR-V) through the shared CMake project.
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build/toon-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/toon-cmake --target toon toon_shaders -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
echo "Built build/toon-cmake/toon"
