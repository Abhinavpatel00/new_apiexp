# Mathematics: tutorial adaptation and independent extension

All vectors below are in world space unless otherwise stated. RGB operations, `max`, `clamp`, and products are component-wise. `sat(x)=clamp(x,0,1)`, `mix(a,b,t)=a(1-t)+bt`. Colors are linear. Dots require normalized vectors. These equations describe `toon.slang`, not the unpublished full NiloToonURP. For original intent and exact URP implementation see the upstream commit linked in README.

## 1. Coordinate transforms

For object position `p`, `p_world = M_object (p,1)` and `p_clip = P V (p_world,1)`. A normal transforms as `normalize((M_object^-1)^T (n,0))`; inverse transpose matters under nonuniform scaling. Rasterization perspective-interpolates varyings, and the pixel shader normalizes the interpolated normal again. Its unit vectors are `N`, main surface-to-light `L`, and camera vector `V = normalize(cameraPosition - p_world)`.

UV is `u' = u * baseUV.xy + baseUV.zw`. Base, AO, emission and independent face map use u'; the outline offset mask deliberately uses original UV, matching the tutorial's pattern. `A = baseTexture(u') * baseColor`, including alpha. If enabled, discard when `A.a < cutoff`. This is cutout, not alpha blending; output alpha does not make an opaque pipeline transparent.

## 2. Ambient occlusion and emission

Let `T` be the sampled AO RGBA and `m` the channel mask. Compute `a0 = dot(T,m)` and `a1 = mix(1,a0,strength)`. Remap `ao = sat((a1-start)/(end-start))`; when the endpoints are equal, our port defines a hard step rather than the upstream's division by zero. Reversed endpoints intentionally invert the result. With AO disabled, `ao=1`. For example, a red sample 0.2 with strength 0.5 and remap 0..1 gives `ao=0.6`. A non-one-hot channel mask can produce a value outside 0..1 before remapping.

`E0 = emissionTexture.rgb * channelMask.rgb * emissionColor.rgb` when enabled, else zero. `E = mix(E0, E0*A.rgb, emissionMulByBaseColor)`. This is emissive radiance added *after* lighting, so it can remain bright in darkness. HDR values are retained until output/tone mapping.

## 3. Smooth bands

`smoothBand(c,s,x)` is a hard step at `c` if `s <= 1e-6`; otherwise `smoothstep(c-s,c+s,x)`. Explicitly, `t = sat((x-(c-s))/(2s))`; result `t²(3-2t)`. It is 0 at/below `c-s`, 1 at/above `c+s`, and 0.5 at `c`. This adds a defined hard-step behavior for zero softness, where upstream smoothstep has coincident edges. Smaller `s` makes sharper cartoon edges.

## 4. Tutorial light equation

For each light `i`, `d=N·L_i` spans -1..1. Calculate `b = smoothBand(midpoint,softness,d) * ao`. If this material is a face, set `b = mix(0.5,1,b)`, reducing darkening. Then `b *= mix(1,shadow_i,receiveShadowAmount)`, and `tint = mix(shadowColor,1,b)`. The main result is `D_main = sat(lightColor) * tint * min(4,attenuation)`; the additional result uses the same expression multiplied by 0.25. This tutorial behavior tints its shadow rather than forcing black; saturating each light RGB reduces overbrightness but alters intensities above one. Face handling happens before shadow attenuation. A red-ish shadow tint can produce stylized skin shadows.

For the main directional light, attenuation is 1. For our point lights, with vector `r=lightPosition-p`, distance `d=max(length(r),0.0001)`, inverse range `q`, we use `L=r/d` and `attenuation = sat(1-d*q)^2 / max(d²,0.01)`, subsequently capped at 4 by the lighting function. This is a simple, non-URP approximation: distance is measured in world units, and RGB/intensity can be clipped by `sat` before attenuation.

GI is `I=max(indirectMin,indirectColor) * mix(1,ao,0.5)`. Supply `indirectColor` from the renderer, ideally evaluating the same Unity `SampleSH(0)` if matching the tutorial; it's not a normal-dependent irradiance probe in this shader. The upstream clamps the minimum **before** AO, so the final GI can be below the minimum. Sum additional light results `D_add`; compose `C = A.rgb * max(I, D_main + D_add) + E`. The `max` is per RGB channel, not a luminance comparison, and can change hue. It avoids summing GI and direct while preserving a floor; it is stylized and not physically energy conserving.

## 5. Directional shadow map

Offset the shadow receiver along the main light direction: `p' = p + L*(receiverOffset + isFace)`, using the tutorial's face offset of one world unit. Project `q=worldToShadow*(p',1)` and divide by `q.w`. Convert XY to UV with `uv=ndc.xy/2+0.5`. If behind the light, outside depth [0,1] or outside UV, visibility is 1. Otherwise each of nine nearest texels yields 1 if `ndc.z-bias <= storedDepth`, else 0; average to form `shadow` in [0,1]. A small positive depth bias fights acne but excessive bias makes shadows detach. UV/Y conventions and matching shadow pass transforms are the host's responsibility. Unlike the tutorial, this is one non-cascaded, conventional-depth 3x3 PCF map and additional lights don't shadow.

## 6. Outline geometry and depth

For each outline vertex, transform position and normal to world, then expand `p_outline = p_world + N * width * cameraFix * 0.00005`. For perspective, `cameraFix = sat(abs(viewZ))*fovDegrees`; for orthographic, `sat(abs(orthoHalfHeight))*50`. These follow the tutorial's heuristic and are **not** an exact constant-pixel-width solution. Backfaces of this expanded hull are rendered while frontfaces are culled; the base surface occludes the interior and the protruding silhouette becomes the outline. Lighting is computed on the expanded position and multiplied by `outlineColor` before fog.

For outline depth, read raw UV mask `m=remapClamp(start,end,1-mask.r)`; `offset = outlineZOffset*m + 0.03*isFace`. Transform the expanded point into view space, displace its view Z by `-offset` (under the -Z forward convention, this is farther from the camera), project that imaginary point to get `z'/w'`, and set the original clip `z = (z'/w')*originalClipW`. Original clip XY and W are kept, so this changes depth testing without moving screen XY. This generic projection-based depth rewrite is conceptually analogous but differs from the tutorial's Unity-specific perspective/orthographic formulas. It does not guarantee identical results, especially around the near plane or reverse-Z.

## 7. Independent extended model

The extended model deliberately uses a different three-band main-light equation. Start with `c=midpoint`; when enabled add `2*faceTexture.r-1` to `c`. This painted map shifts a scalar threshold only: it does not calculate a head-oriented or mirrored directional SDF. Compute `lo=smoothBand(c,softness,N·L)*ao`, `hi=smoothBand(max(c,secondThreshold),secondSoftness,N·L)*ao`. Multiply both by `mix(1,shadow,receiveAmount)`. Form `tint = mix(mix(shadowColor,midColor,lo),white,hi)` and `D_main=sat(mainColor)*tint`. The two overlapping smooth transitions mean intermediate colors can blend; do not interpret `lo` and `hi` as a partition of unity. Additional point lights use the tutorial light equation. Indirect and emission composite as above.

Stylized Blinn–Phong specular: `H=normalize(L+V)`, `s0=max(N·H,0)^max(exponent,0.001)`, `spec=smoothBand(specThreshold,specSoftness,s0)*specIntensity*shadow*step(0,N·L)`. Add `spec*specColor*sat(mainColor)`. The threshold quantizes a conventional smooth highlight; `step` suppresses most back-facing lighting. The implementation guards the degenerate `L+V=0` normalization. Higher exponent generally narrows the underlying highlight. This specular term is independent of the diffuse albedo.

View rim: `r0=(1-sat(N·V))^max(rimPower,0.001)`, `rim=smoothBand(rimThreshold,rimSoftness,r0)*rimIntensity`; add `rim*rimColor`. Rim grows at grazing angles, is unshadowed, and is not light-facing-limited. Both rim and specular are artistic additions, not physical BRDFs.

## 8. Fog, depth and color output

With fog enabled and `end>start`, `visibility = 1 - sat((abs(viewZ)-start)/(end-start))`; `outputRGB = mix(fogColor,C,visibility)`. This is our view-depth linear fog, not URP's per-vertex `ComputeFogFactor/MixFog`. When fog is off, `outputRGB=C`. Outline multiplication precedes fog. Return `(outputRGB,A.a)`; HDR content may exceed 1, and handling it depends on the target attachment and postprocessing.

Depth-only fragment shares the same alpha clip; the depth value comes from fixed-function rasterization, and its output has no color. Normals pass writes normalized world XYZ directly to a signed-capable target. These differ in encoding and pipeline conventions from URP depth-normal variants.
