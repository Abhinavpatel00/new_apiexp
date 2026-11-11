#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build/galaxy-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/galaxy-cmake --target galaxy galaxy_shaders -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
echo "Built build/galaxy-cmake/galaxy"
