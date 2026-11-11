# Independent extensions: math and caveats

The inherited base equations—transform, direct bands, AO, GI, shadows, outline, emission and fog—are explained in `MATH.md`. The following describes `inspired.slang` only. None of these equations claims to reconstruct the private NiloToonURP shader. Values, vectors and colors are linear; `sat(x)=clamp(x,0,1)` and `mix(a,b,t)=a(1-t)+bt`.

## Derivative tangent-space normal mapping

With screen derivatives `p_x,p_y` and UV derivatives `u_x,u_y`, let `det = u_x.x*u_y.y - u_x.y*u_y.x`. Reconstruct `T_raw=(p_x*u_y.y-p_y*u_x.y)/det` and `B_raw=(-p_x*u_y.x+p_y*u_x.x)/det`. Normalize `N`, orthogonalize `T=normalize(T_raw-N dot(N,T_raw))`, then choose handedness `s=sign(dot(cross(N,T),B_raw))` and set `B=s normalize(cross(N,T))`. Decode map RGB to `m=2*RGB-1`, scale `m.xy` by normal strength, and normalize `T*m.x+B*m.y+N*m.z`. If UV derivatives are degenerate, use geometric N. This derivative frame may differ from a baked MikkTSpace tangent frame and can behave badly at UV seams, mirrored islands, degenerate triangles or interpolated discontinuities; authored tangent attributes are preferable for production.

## Face SDF and cel bands

Head-right `R` and head-forward `F` are normalized world vectors. Define horizontal coordinates `h=dot(L,R)` and `f=dot(L,F)`, with `L` pointing towards the light. Mirror the texture UV if h is negative (optionally flip the convention). `angle=atan2(abs(h),f)/π` runs from 0 for front illumination to 1 for rear illumination. `faceLit=smoothBand(angle,softness,SDF.r)`. This creates a stylized traveling boundary; it requires a specifically authored texture and breaks down when the horizontal projection of L is nearly zero. It is not a literal physical signed-distance evaluation, despite common SDF nomenclature.

The first diffuse band `lo` comes from this value if the face toggle is on, otherwise from `smoothBand(c,s,N·L)`. The second band `hi=smoothBand(max(c,secondThreshold),secondSoftness,N·L)` **always uses the surface normal**, including on faces, so some face-normal shading remains; disable or adjust that upper band for a flatter face. Multiply both by `AO * shadow * (1-hairMask)` and form `mix(mix(shadowColor,midColor,lo),white,hi)`. Point lights retain tutorial lighting. A face mask is an art-directed control, not a guarantee of the appearance in the reference images.

## Hair shadow mask

At the current framebuffer pixel `p`, sample `M=hairMask(p/resolution)`; set `hairMask=sat(M*strength)` and multiply main diffuse band values and GGX by `1-hairMask`. This affects lighting, not geometry. The input must already represent a projected/blurred mask on receivers; screen-space projection and occlusion are external. Hair glint and inherited point lights are not masked by this term.

## GGX specular

Let `H=normalize(L+V)`, `nL=sat(N·L)`, `nV=sat(N·V)`, `nH=sat(N·H)`, `vH=sat(V·H)`. With roughness `r` clamped to [0.04,1], `a=max(r²,0.0025)` and `a²=a*a`, the isotropic GGX normal distribution is `D=a²/[π*(nH²*(a²-1)+1)²]`. Schlick-Smith approximation uses `k=(r+1)²/8`, `G1(x)=x/[x*(1-k)+k]`, `G=G1(nV)*G1(nL)`. Fresnel uses `F0=mix(0.04,albedo,metallic)*F0Tint` and `F=F0+(1-F0)*(1-vH)^5`. The reflected-light contribution is `D*G*F/(4*nV*nL)*nL`, with small denominators guarded. Optionally threshold its luminance to form a toon highlight; the banded form multiplies by F. Multiply by intensity, main-light color, visibility and one minus hair shadow. This hybrid deliberately keeps the diffuse toon albedo even for metallic materials, so it is **not** a fully energy-conserving metallic BRDF.

## Hair glint

`up=normalize(cross(faceForward,faceRight))`; project up into the tangent plane and add a user shift along N: `T=normalize(up-N dot(up,N)+shift*N)`. Let `x=clamp(T·H,-1,1)` and `lobe=(sqrt(sat(1-x²)))^exponent`; threshold the lobe to obtain a glint. The result is view- and light-dependent and does not use hair strand tangents or a true Kajiya–Kay model. For degenerate head axes or light directions its behavior is undefined stylistically even though normalization guards avoid division by zero.

## MatCap

Transform the perturbed normal into view space and use `uv=(0.5+0.5*Nview.x, 0.5-0.5*Nview.y)`. Fetch RGB `M` from a user-supplied MatCap. The mask is `mix(1,1-lo,shadowOnly)` and operations occur in order: multiply by `mix(1,M,multiply*mask)`, add `M*additive*mask`, and replace with `mix(C,M,replace*mask)`. Thus replace can overwrite earlier effects and an HDR source can exceed one. These are artistic operations, not physically-based illumination.

## Screen-space rim

Project N to view XY and normalize `dir=(Nview.x,-Nview.y)`. Fetch positive linear view depth at `(pixel + dir*widthPixels)/resolution`. Output rim strength if neighbor depth is greater than `abs(currentViewZ)+depthThreshold`; otherwise zero. The pixel width is only as stable as the provided framebuffer resolution and this one-tap depth test. At `Nview.xy≈0`, normalization guard makes the offset near zero. A depth discontinuity is not always a silhouette and a missing/incorrect linear-depth texture makes the rim invalid.

## Fade, dissolve and edge

An opaque pixel survives screen dither if the deterministic hash of its integer pixel coordinate is below visibility. Visibility 0 clips all and visibility 1 clips none. It survives dissolve when `noise(uv)>=dissolveAmount`. After clipping, add an emissive band immediately above the threshold using the smooth-band difference defined in the source, with `edgeWidth` and `edgeStrength`. Screen dither does not match between light-view and camera-view rasterization; outline pixels also differ. The result remains opaque/cutout, not sorted alpha transparency.
