# Render effects quality contracts

This note records the production contracts shared by shadows, bloom, and
screen-space subsurface scattering (SSSS). They are quality and correctness
rules, not measured frame-rate claims.

## Bloom and exposure

Bloom extraction operates on the same exposed radiance that enters tone
mapping:

1. Scene-linear TAA resolves before eye adaptation.
2. Automatic exposure, when enabled, is applied once by `Exposure.Apply`.
3. Manual exposure (or EV compensation when auto exposure is enabled) is
   applied to every bloom prefilter sample before the soft threshold.
4. Tone mapping applies the same manual factor once to scene color. Bloom is
   already in that exposed domain and is not multiplied a second time.

This keeps the bloom threshold, halo energy, and visible scene luminance in one
domain. The neutral manual exposure of `1.0` is bitwise-compatible with the
previous prefilter input. Non-finite authoring values are replaced by the
neutral default and the shader-facing multiplier remains clamped to `[0, 64]`.
The 2x2 Karis prefilter, six-mip chain, fixed Gaussian gathers, and circular
upsampling remain unchanged.

## FXAA presentation

`FPostProcessParams::fxaa_enabled` is an opt-in scene setting exposed by
`ALegacyScene3DAdapter::PostParams()`. It is disabled by default to preserve
existing output and avoid a recurring full-screen pass for scenes that already
use TAA or accept the native raster result. When enabled, the post-process
graph first writes the completed HDR scene through tonemap and gamma into a
full-resolution LDR intermediate, then runs `CFxaa` once while the swapchain
pass is bound. This keeps FXAA in the correct display-color domain instead of
filtering HDR radiance.

If TAA produced a valid resolve, FXAA is skipped to avoid double filtering. If
TAA was requested but its resolve failed, FXAA can serve as the spatial
fallback. Missing FXAA shaders or the intermediate target preserve the
existing direct-tonemap path, so enabling the option cannot publish a blank
frame. The FXAA shaders join the existing CPU/backend asynchronous compilation
bundle; activation does not introduce a new synchronous shader compile stall.

The optional post-process SSR texture is explicitly scene-linear. Tone mapping
multiplies it by the same manual exposure and, only after a completed automatic
exposure pass, by the same adapted 1x1 value as scene color. The adapted value
is sampled only when SSR is non-null; otherwise a strict-backend fallback is
bound but the shader branch does not issue the sample. The editor's normal PBR
path already integrates SSR before post processing, but this keeps the public
`FPostProcessParams::ssr_texture` contract correct for standalone users.

## Temporal ownership and publication

TAA, automatic exposure, SSR, SSGI, motion vectors, and volumetric clouds are
owned by the logical scene camera, not merely by the physical swapchain.
Changing Scene/Game View, switching camera requests, changing perspective
versus orthographic projection, replacing the scene, resizing dependent
targets, or detecting an abrupt same-camera orientation/FOV/teleport cut
invalidates all related histories together. The first enabled frame uses the
current view as its previous view, takes 100 percent current color, disables
motion-vector reprojection, and meters exposure directly from current HDR.

SSR and SSGI now expose an explicit completed-output bit. It is cleared before
every attempted render and becomes true only after the final temporal target
has been recorded. The editor samples this bit instead of assuming that a
`Render` call produced a publishable texture. Initialization and resize also
cold-start the bit. SSR allocation follows the same strong-commit rule already
used by SSGI and post processing: all same-generation output/history targets
are created off to the side and committed only as a complete set.

## Strict bindings and disabled workload

Every texture slot declared by the TAA, tone-map, SSR, and SSGI pipelines is
bound on every recorded draw. Optional inputs use neutral fallback textures,
and a missing required fallback fails before opening the render pass. Shader
mode bits still prevent disabled branches from sampling those descriptors.
This keeps raw DX12 and stricter descriptor-validation backends deterministic.

Optional effects do not record recurring fullscreen work while disabled:
automatic-exposure reduction/adaptation, TAA resolve, bloom, SSR, SSGI, and
SSSS diffusion/composite remain behind their enabled-and-ready workload gates.
The final tone-map pass is mandatory presentation work and remains active.
SSR/SSGI CPU and GPU time is accumulated in the post category only inside the
actual render gates. No render scale, ray/sample/step count, bloom mip count,
SSSS kernel, or temporal quality setting was reduced for these changes.

## SSSS visibility and workload

SSSS has two deliberately separate decisions:

- Scene-wide presence keeps shader pipelines and full-resolution targets warm,
  including while every SSSS object is outside the main camera.
- Main-view presence controls the recurring four-target opaque draw, bilateral
  diffusion, and composite passes.

Main-view presence consumes the same fail-open visibility mask as normal/depth,
opaque count, and opaque draw. A missing or short mask renders rather than
drops geometry. Shadow casters and VXGI remain unmasked because they operate in
light space and world space respectively.

An offscreen-to-visible transition therefore reuses warm resources and restores
the full SSSS path immediately; visible-to-offscreen stops the full-resolution
work without discarding resources. CPU and GPU profiler time is accumulated in
the opaque category only when the SSSS workload is actually recorded.

The diffusion quality path is unchanged: authored RGB mean-free-path radii,
full-resolution HDR targets, normalized per-channel kernels, normal and
world-plane bilateral rejection, and diffuse-only energy replacement remain
active.

SSSS material presence is intentionally inspected each frame. The existing
scene-mesh revision cannot safely cache this decision: its key covers material
kind/base color/metallic/roughness, but not transmission, legacy subsurface,
Substrate slab mean-free-path/thickness, or expression roots. In addition,
`AMeshComponent3D::MaterialMut()` provides mutable access without a material
generation counter. Reusing that revision could therefore retain a stale
off/on result. The scan exits as soon as both scene-wide warmup and main-view
presence are known; a future cache requires a material revision that covers
all of those dependencies.

## Shadow audit

The current directional-shadow path retains four quality guarantees:

- orthographic cascade projections are snapped to the shadow texel grid;
- practical cascade splits blend across the final 15 percent of each range;
- PCSS gathers are clamped to a half-texel inset inside each atlas cascade;
- invalid matrices and authoring values fail to finite fallback projections.

Shadow and VXGI traversal intentionally do not consume the main-camera
visibility mask. No shadow sample count, cascade resolution, or quality LOD was
reduced as part of the bloom/SSSS workload changes.

## Verification

`post_effect_workload_tests.cpp` covers:

- neutral, non-finite, lower-bound, and upper-bound manual exposure;
- exposure placement across auto exposure, bloom extraction, and tone mapping;
- SSSS visible/offscreen/visible transitions and short-mask fail-open behavior;
- shared normal/depth and opaque mask policy;
- explicit independence of shadow and VXGI traversal;
- editor integration order and profiler scope placement;
- camera/view/projection/exposure cold-start boundaries;
- fail-closed SSR/SSGI publication and strong target-set commits;
- declared texture-slot counts versus unconditional strict bindings;
- zero optional fullscreen dispatches through disabled effect gates;
- the explicit no-cache decision for mutable SSSS material state.

The existing post-effect quality suite continues to compile the production
post shaders and validate the bloom, SSSS, temporal, and shadow contracts.
