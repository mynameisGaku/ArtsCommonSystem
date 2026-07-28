# Interactive Water 3D

`FWaterSurface3D` is the specialized, depth-writing water renderer used by the
editor and runtime-facing samples. Water meshes are authored on local XZ and
may be translated, rotated, or non-uniformly scaled in a 3D scene.

## Persistent interaction

- Impact and wake capacity are reserved independently: 16 impact events and 48
  wake events per surface. A full wake cannot erase an impact and a new mouse
  event never overwrites an older active event.
- One renderer owns independent pools for up to 64 surfaces. Editor node IDs
  are used as stable surface identities, and deleting a node or removing its
  water component clears only that surface.
- Low-frequency 3D motion is resampled by `AddWakeSegmentForSurface`. Each
  sample stores its own world position, direction, age, and lifetime, so later
  pointer movement does not refresh or replace an earlier trail.
- The final 35 percent of every lifetime uses a quintic smootherstep tail.
  Height, analytic gradient, and ripple energy all use the same amplitude.
  Ripple foam uses a continuous saturating response rather than an early
  threshold, so displacement, normal, and foam reach zero together with zero
  first and second temporal derivatives.

Active events are also kept in a dense index list. Idle water advances only its
analytic phase clock in O(1); updating and uploading disturbances is
proportional to the number of live events rather than the 4096-slot ownership
capacity. This changes no samples, steps, resolution, or authored quality.

## Surface normal and lighting

The vertex shader differentiates all ambient and interactive wave functions,
then displaces along the transformed authored mesh normal. The pixel shader
adds two independent micro-normal sources:

1. a generated, tileable three-layer world-space detail map; and
2. the optional tangent-space normal map from the node's authored material.

Both maps are decoded as height-field slopes. Generated detail is added in the
analytic surface frame; the authored map is slope-scaled and composed through a
per-pixel derivative TBN, so rotated or scaled mesh UVs remain correct without
requiring vertex tangents. This preserves the physical relationship between
displacement and normal instead of linearly blending unrelated unit normals.
The authored map uses its material normal-strength value.

Water uses an IOR of 1.333, Schlick Fresnel with dielectric F0, Snell
refraction, Beer-Lambert absorption, homogeneous single scattering with a
Henyey-Greenstein phase function, roughness-aware GGX sunlight, SSR with an
environment fallback, crest foam, ripple foam, and reconstructed-depth contact
foam.

## Frame order and workload gates

The editor draws water after opaque geometry and before aerial perspective,
volumetric clouds, and local fog:

1. frustum-test the conservatively displacement-inflated water bounds;
2. return without any color copy, depth copy, fullscreen draw, or water draw
   when no water surface was submitted;
3. copy opaque HDR color and opaque depth;
4. reopen HDR with live depth bound for depth test/write;
5. draw water using the immutable color/depth snapshots;
6. composite atmosphere and post effects;
7. draw editor-only gizmos after post processing.

Consequently water is hidden by foreground opaque geometry, water surfaces
occlude one another through hardware depth, clouds/fog terminate against the
updated water depth, and editor gizmos cannot be covered by water or clouds.

The water pipeline declares and binds six SRVs on every draw, including safe
fallbacks for absent authored normal, scene color, depth, reflection, or shadow
inputs. This is valid on strict resource-binding backends.

## Verification

`water3d_ripple_lifetime_tests.cpp` covers reserved ownership, independent
surface pools, 3D wake resampling, C2 lifetime behavior, dense-list retirement,
normal composition, strict resource binding, and the zero-fullscreen-work
culled/absent gate. `PostEffects.PipelinesCompileOnActiveBackend` initializes
the real water pipeline on the active RHI and therefore compiles both embedded
HLSL stages on raw DX12.
