#!/usr/bin/env bash
# Build the clean Vulkan library and its headless smoke test.
# Vulkan-Headers and Volk are vendored; Volk loads the Vulkan runtime at
# startup, so the executable does not link against libvulkan.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

CC=${CC:-gcc}
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-error=unused-but-set-variable -O2 -fno-strict-aliasing -I src -I external/vulkan/include -I external/volk -I external/mu_lib/mu"

echo "[1/4] src/vk.c"
$CC $CFLAGS -c src/vk.c -o build/vk.o

echo "[2/4] external/volk/volk.c"
$CC $CFLAGS -c external/volk/volk.c -o build/volk.o

echo "[3/4] external/mu_lib/mu/mu_id_pool.c"
$CC $CFLAGS -c external/mu_lib/mu/mu_id_pool.c -o build/mu_id_pool.o

echo "[4/4] tests/smoke.c"
$CC $CFLAGS tests/smoke.c build/vk.o build/volk.o build/mu_id_pool.o -o build/smoke -ldl -lm
$CC $CFLAGS tests/id_pool.c build/mu_id_pool.o -o build/id_pool_test
$CC $CFLAGS tests/input.c -o build/input_test -lm

echo "built build/smoke, build/id_pool_test and build/input_test"
echo "run from the repo root:  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./build/smoke"
