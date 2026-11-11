#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
SLANGC="${SLANGC:-slangc}"
mkdir -p spirv spirv/bindless
for entry in vertMain vertOutline fragTutorial fragTutorialOutline fragExtended fragExtendedOutline fragDepth fragNormals; do
  "$SLANGC" toon.slang -entry "$entry" -target spirv -profile spirv_1_3 -matrix-layout-row-major -o "spirv/$entry.spv"
done
"$SLANGC" toon.slang -entry fragExtended -target spirv -profile spirv_1_3 -matrix-layout-row-major -o spirv/reflection-source.spv -reflection-json spirv/reflection.json
rm spirv/reflection-source.spv
if command -v spirv-val >/dev/null; then
  for f in spirv/*.spv; do spirv-val --target-env vulkan1.1 "$f"; done
fi
for entry in fragInspired fragInspiredOutline fragInspiredDepth fragInspiredNormals; do
  "$SLANGC" inspired.slang -entry "$entry" -target spirv -profile spirv_1_3 -matrix-layout-row-major -o "spirv/$entry.spv"
  # The bindless variant includes ../slang-toon/bindless/toon.slang and uses
  # the mu_gfx set 0 layout; see bindless/BINDLESS.md.
  "$SLANGC" bindless/inspired.slang -entry "$entry" -target spirv -profile spirv_1_3 -matrix-layout-row-major -warnings-disable 41012 -o "spirv/bindless/$entry.spv"
done
"$SLANGC" inspired.slang -entry fragInspired -target spirv -profile spirv_1_3 -matrix-layout-row-major -o spirv/reflection-source.spv -reflection-json spirv/inspired-reflection.json
"$SLANGC" bindless/inspired.slang -entry fragInspired -target spirv -profile spirv_1_3 -matrix-layout-row-major -warnings-disable 41012 -o spirv/bindless/reflection-source.spv -reflection-json spirv/bindless/inspired-reflection.json
rm spirv/reflection-source.spv spirv/bindless/reflection-source.spv
if command -v spirv-val >/dev/null; then
  for f in spirv/fragInspired*.spv spirv/bindless/*.spv; do spirv-val --target-env vulkan1.1 "$f"; done
fi
