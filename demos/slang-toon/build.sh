#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
SLANGC="${SLANGC:-slangc}"
mkdir -p spirv spirv/bindless
for entry in vertMain vertOutline fragTutorial fragTutorialOutline fragExtended fragExtendedOutline fragDepth fragNormals; do
  "$SLANGC" toon.slang -entry "$entry" -target spirv -profile spirv_1_3 -matrix-layout-row-major -o "spirv/$entry.spv"
  "$SLANGC" bindless/toon.slang -entry "$entry" -target spirv -profile spirv_1_3 -matrix-layout-row-major -warnings-disable 41012 -o "spirv/bindless/$entry.spv"
done
# ABI v2 only exists in the bindless variant (see bindless/BINDLESS.md).
"$SLANGC" bindless/toon.slang -entry vertShadow -target spirv -profile spirv_1_3 -matrix-layout-row-major -warnings-disable 41012 -o spirv/bindless/vertShadow.spv
"$SLANGC" toon.slang -entry fragExtended -target spirv -profile spirv_1_3 -matrix-layout-row-major -o spirv/reflection-source.spv -reflection-json spirv/reflection.json
"$SLANGC" bindless/toon.slang -entry fragTutorial -target spirv -profile spirv_1_3 -matrix-layout-row-major -warnings-disable 41012 -o spirv/bindless/reflection-source.spv -reflection-json spirv/bindless/reflection.json
rm spirv/reflection-source.spv spirv/bindless/reflection-source.spv
if command -v spirv-val >/dev/null; then
  for f in spirv/*.spv spirv/bindless/*.spv; do spirv-val --target-env vulkan1.1 "$f"; done
fi
