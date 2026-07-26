// SPDX-License-Identifier: Apache-2.0
// 3D interactive water surface: layered normal map, analytic waves, refraction,
// Fresnel reflection, GGX sun highlights, foam, and persistent disturbances.
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "memory/UniquePtr.h"
#include "render/IRhiShader.h"
#include "render/RenderAssets.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiBuffer;
class IRhiCommandList;
class IRhiDevice;
class IRhiPipeline;
class IRhiTexture;
class FMeshAsset;

/**
 * Authoring parameters for FWaterSurface3D.
 *
 * @details
 * The surface is physically based around water IOR 1.333. Colors and absorption
 * describe the volume below the surface, while analytic waves and a generated
 * tileable normal map provide independent macro/micro detail. The renderer is
 * authored on the mesh-local XZ plane. DrawMesh derives an orthonormal
 * world-space surface frame from the model matrix, transforms the mesh's actual
 * vertex normals with an inverse-transpose matrix, and projects 3D disturbance
 * points into that frame. Translation, rotation, and non-uniform scale therefore
 * preserve displacement, normals, and ripple placement.
 */
struct FWaterSurface3DParams {
    /** Shallow-water in-scattering color. */
    FVec3 shallow_color{0.055f, 0.38f, 0.50f};

    /** Deep-water in-scattering color. */
    FVec3 deep_color{0.008f, 0.055f, 0.16f};

    /** Beer-Lambert absorption coefficient in inverse world units. */
    FVec3 absorption{0.34f, 0.13f, 0.040f};

    /** Homogeneous single-scattering coefficient in inverse world units. */
    FVec3 scattering{0.018f, 0.050f, 0.085f};

    /** Henyey-Greenstein phase anisotropy (-0.95..0.95). */
    f32 phase_anisotropy = 0.62f;

    /** Whitewater/contact-foam color. */
    FVec3 foam_color{0.88f, 0.96f, 1.0f};

    /** Main flow direction in the surface-local tangent/bitangent plane. */
    FVec2 flow_direction{0.92f, 0.38f};

    /** Microfacet roughness used by the water GGX lobe. */
    f32 roughness = 0.105f;

    /** Strength of the sampled normal map. */
    f32 normal_strength = 0.82f;

    /** World-space tiling density of the generated normal map. */
    f32 normal_tiling = 0.075f;

    /** Screen-space refraction strength. */
    f32 refraction_strength = 0.72f;

    /** Approximate optical depth when no explicit scene-depth texture is supplied. */
    f32 optical_depth = 1.35f;

    /** Analytic macro-wave displacement amplitude. */
    f32 wave_amplitude = 0.105f;

    /** Analytic macro-wave spatial scale. */
    f32 wave_scale = 0.78f;

    /** Analytic wave/normal-map animation speed. */
    f32 wave_speed = 0.72f;

    /** Dynamic ripple propagation speed in world units per second. */
    f32 ripple_speed = 2.65f;

    /** Dynamic ripple wavelength in world units. */
    f32 ripple_wavelength = 0.52f;

    /** Dynamic ripple lifetime in seconds. */
    f32 ripple_lifetime = 4.0f;

    /** Dynamic ripple amplitude damping coefficient. */
    f32 ripple_damping = 0.78f;

    /** Contact/crest foam multiplier. */
    f32 foam_intensity = 0.82f;
};

/**
 * High-quality interactive 3D water renderer.
 *
 * @details
 * FWaterSurface3D is renderer-facing and accepts any sufficiently tessellated
 * mesh authored on its local XZ plane. The model may freely translate, rotate,
 * or scale that surface in a 3D scene. Dynamic disturbances are stored as full
 * world-space points and are never overwritten while active.
 * Impact and wake reservations are separate, so a long cursor/body wake cannot
 * erase impacts (and impacts cannot starve wakes).
 *
 * Typical frame order:
 *  1. Draw opaque scene to a color target.
 *  2. Copy that target to a sampleable background texture.
 *  3. SetFrame(), then DrawMesh() with the copied background and, when
 *     available, the shader-visible opaque depth and SSR/planar reflection.
 *  4. Run bloom/tonemapping if using HDR.
 */
class FWaterSurface3D {
public:
    /** Backend-compiled shader handles awaiting owner-thread PSO creation. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> vertex;
        TUniquePtr<IRhiShader> pixel;

        EShaderStatus Status() const noexcept;
    };

    /** Maximum simultaneously visible disturbances. */
    static constexpr u32 kMaxRipples = 64;

    /** Maximum independently interactive surfaces owned by one renderer. */
    static constexpr u32 kMaxTrackedSurfaces = 64;

    /**
     * CPU-side event capacity. Each draw still uploads at most kMaxRipples,
     * but independently targeted surfaces cannot starve one another.
     */
    static constexpr u32 kMaxStoredRipples =
        kMaxRipples * kMaxTrackedSurfaces;

    /** Slots reserved exclusively for circular impact disturbances. */
    static constexpr u32 kImpactRippleSlots = 16;

    /** Slots reserved exclusively for directional wake disturbances. */
    static constexpr u32 kWakeRippleSlots = 48;

    static_assert(kImpactRippleSlots + kWakeRippleSlots == kMaxRipples);

    FWaterSurface3D() noexcept;
    ~FWaterSurface3D() noexcept;

    FWaterSurface3D(const FWaterSurface3D&) = delete;
    FWaterSurface3D& operator=(const FWaterSurface3D&) = delete;

    /**
     * Creates shaders, pipeline, constant-buffer ring, and a generated normal map.
     */
    TResult<void> Init(IRhiDevice& device,
                       EFormat rt_format = EFormat::R16G16B16A16_Float,
                       EFormat depth_format = EFormat::D32_Float,
                       u32 msaa_samples = 1) noexcept;

    /**
     * Compile raw-DX12 water HLSL without touching an RHI device.
     *
     * @details This is safe to call from a background worker. Resource and
     * pipeline creation must still be committed on the render-owner thread
     * through BeginInitWithCompiledShaders.
     */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /** Submit both shader stages without blocking on supporting backends. */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(
        IRhiDevice& device) noexcept;

    /** Commit ready shader handles and all GPU resources on the render owner. */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        EFormat rt_format = EFormat::R16G16B16A16_Float,
        EFormat depth_format = EFormat::D32_Float,
        u32 msaa_samples = 1) noexcept;

    /**
     * Commit shaders, textures and PSOs without allocating the large
     * per-draw constant-buffer ring.
     */
    TResult<void> BeginInitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        EFormat rt_format = EFormat::R16G16B16A16_Float,
        EFormat depth_format = EFormat::D32_Float,
        u32 msaa_samples = 1) noexcept;

    /**
     * Allocate a bounded number of constant-buffer pairs.
     *
     * @return true when the renderer is fully ready, false when more bounded
     *         commit work remains.
     */
    TResult<bool> AdvanceInitialization(
        u32 buffer_pairs = 16u) noexcept;

    bool InitializationPending() const noexcept {
        return m_InitializationPending;
    }

    /** Releases all GPU resources. Safe to call repeatedly. */
    void Shutdown() noexcept;

    /** Advances analytic animation and all active disturbances. */
    void Update(f32 dt) noexcept;

    /**
     * Adds a circular impact at a world-space point.
     *
     * @return true when accepted. false means all impact-reserved slots are
     *         active; existing waves are preserved and the new event is dropped.
     */
    bool AddDisturbance(FVec3 world_point, f32 radius, f32 strength) noexcept;

    /**
     * Adds a circular impact that is visible only on the identified surface.
     *
     * @details surface_id is an application-stable identity (for example an
     * editor node id). Up to kMaxTrackedSurfaces can own active events at once.
     */
    bool AddDisturbanceForSurface(
        u64 surface_id, FVec3 world_point,
        f32 radius, f32 strength) noexcept;

    /**
     * Adds one directional, elongated wake event from a moving body.
     *
     * @return true when accepted; false when the wake-reserved pool is full.
     */
    bool AddWake(FVec3 world_point, FVec3 world_velocity,
                 f32 radius, f32 strength) noexcept;

    /** Adds a directional wake visible only on the identified surface. */
    bool AddWakeForSurface(
        u64 surface_id, FVec3 world_point, FVec3 world_velocity,
        f32 radius, f32 strength) noexcept;

    /**
     * Resamples a low-frequency 3D motion segment into a coherent wake trail.
     *
     * @details Every accepted sample owns an independent lifetime and uses a
     * uniformly distributed point along the portion of the world-space segment
     * whose historical age is still within ripple_lifetime. An older prefix is
     * omitted because it would already be invisible. Existing impacts and wakes
     * are never replaced. When fewer wake-reserved slots are available than the
     * requested spacing would require, the available samples are spread across
     * the complete visible tail so the newest pointer/body position remains
     * represented instead of leaving an abrupt visual gap.
     *
     * @param duration Segment travel time in seconds.
     * @param sample_spacing Maximum desired distance between wake samples.
     * @return Number of independently persistent wake events accepted.
     */
    u32 AddWakeSegment(
        FVec3 segment_start, FVec3 segment_end,
        f32 duration, f32 sample_spacing,
        f32 radius, f32 strength) noexcept;

    /** Resamples a wake segment visible only on the identified surface. */
    u32 AddWakeSegmentForSurface(
        u64 surface_id,
        FVec3 segment_start, FVec3 segment_end,
        f32 duration, f32 sample_spacing,
        f32 radius, f32 strength) noexcept;

    /** Immediately removes every active disturbance. */
    void ClearDisturbances() noexcept;

    /** Removes disturbances owned by one surface without affecting others. */
    void ClearDisturbancesForSurface(u64 surface_id) noexcept;

    /** Number of active persistent disturbance slots. */
    u32 ActiveRippleCount() const noexcept;

    /** Number of active disturbances owned by one surface. */
    u32 ActiveRippleCountForSurface(u64 surface_id) const noexcept;

    /**
     * Normalized amplitude applied to a disturbance at the supplied age.
     *
     * @details Exponential physical damping is combined with a C2-continuous
     * lifetime tail. The tail occupies the final 35% of the lifetime and
     * reaches exactly zero with zero first and second derivatives, preventing
     * displacement, normals, and foam from popping when a slot is released.
     * This public evaluator also lets editor tooling preview the exact runtime
     * attenuation curve.
     */
    static f32 EvaluateRippleAmplitudeScale(
        f32 age, f32 lifetime, f32 damping) noexcept;

    /**
     * Replaces all authoring parameters used from the next draw onward.
     *
     * @details Non-finite values fall back to the corresponding default and
     * physically bounded quantities are clamped. A malformed inspector value
     * therefore cannot inject NaNs into every displaced vertex.
     */
    void SetParams(const FWaterSurface3DParams& params) noexcept;

    /** Returns the current authoring parameters. */
    const FWaterSurface3DParams& Params() const noexcept { return m_Params; }

    /**
     * Sets camera, viewport, and sun state for subsequent water draws.
     *
     * @details Starts a new buffered frame for this water renderer. Up to 64
     * DrawMesh calls may follow before the next SetFrame call.
     *
     * @param sun_direction Direction from the surface toward the sun.
     */
    void SetFrame(const FMat4& view_projection, FVec3 camera_pos,
                  u32 screen_width, u32 screen_height,
                  FVec3 sun_direction = FVec3{-0.42f, 0.82f, -0.38f},
                  FVec3 sun_color = FVec3{5.0f, 4.4f, 3.8f}) noexcept;

    /**
     * Sets the environment radiance used when no valid screen reflection exists.
     *
     * @details The three colors describe the actual world-space sky at +Y,
     * the horizon, and -Y. Non-finite or negative inputs are sanitized, so
     * editor/runtime callers can safely forward their current sky settings.
     */
    void SetEnvironment(FVec3 zenith, FVec3 horizon,
                        FVec3 ground) noexcept;

    /**
     * Validates that a custom water mesh is a finite, indexed surface authored
     * approximately on its local XZ plane.
     *
     * @details This CPU-side authoring check is intended to run before upload.
     * It rejects malformed indices, degenerate XZ projection, non-finite
     * vertices/normals, strongly non-planar geometry, and normals that do not
     * face approximately along local Y. Callers should use a tessellated grid
     * fallback when it returns false.
     */
    static bool IsLocalXzSurfaceMesh(
        const FMeshAsset& mesh) noexcept;

    /**
     * Selects an optional directional-light shadow map.
     *
     * @details The map must be shader-visible depth and use the supplied light
     * view-projection matrix. Passing nullptr disables shadow sampling and
     * restores fully-lit direct sunlight. The pointer remains caller-owned.
     *
     * @param depth_bias Receiver depth bias in light clip-depth units.
     * @param pcf_radius PCF kernel radius in shadow-map texels.
     */
    void SetShadowMap(IRhiTexture* shadow_map,
                      const FMat4& light_view_projection,
                      f32 depth_bias = 0.0012f,
                      f32 pcf_radius = 1.5f) noexcept;

    /**
     * Draws a tessellated water mesh.
     *
     * @details The mesh must be authored on its local XZ surface plane. The
     * model transform may place and orient it freely in world space.
     * When scene_depth is supplied, the caller must never bind that same
     * texture as a DSV for this pass. With hardware_depth_bound=true it must be
     * a shader-visible snapshot of the opaque depth while the original depth
     * resource is bound separately as a writable DSV. This enables opaque and
     * water-to-water hardware occlusion while retaining reconstructed optical
     * thickness from the pre-water snapshot.
     *
     * @param scene_color Copy of the opaque scene rendered before the water.
     *        nullptr selects a safe generated fallback while keeping all other
     *        reflection and lighting effects active.
     * @param scene_depth Optional shader-visible opaque-scene depth. Enables
     *        manual foreground occlusion, reconstructed optical thickness, and
     *        contact foam.
     * @param screen_reflection Optional SSR/planar reflection texture. RGB is
     *        reflected radiance and alpha is the valid-hit mask.
     * @param hardware_depth_bound Selects the depth-test/write PSO. The caller
     *        is responsible for binding a DSV whose viewport and projection
     *        match scene_depth.
     */
    void DrawMesh(IRhiCommandList& command_list, const FGpuMesh& mesh,
                  const FMat4& model,
                  IRhiTexture* scene_color = nullptr,
                  IRhiTexture* scene_depth = nullptr,
                  IRhiTexture* screen_reflection = nullptr,
                  u64 surface_id = 0u,
                  bool hardware_depth_bound = false) noexcept;

    IRhiPipeline* Pipeline() const noexcept { return m_Pipeline.Get(); }
    IRhiTexture* NormalTexture() const noexcept { return m_NormalMap.Get(); }
    f32 Time() const noexcept { return m_Time; }

private:
    struct FRipple {
        FVec3 center{0, 0, 0};
        FVec3 direction{1, 0, 0};
        f32 initial_radius = 0.0f;
        f32 initial_amplitude = 0.0f;
        f32 amplitude = 0.0f;
        f32 age = 0.0f;
        f32 speed = 0.0f;
        f32 lifetime = 0.0f;
        f32 damping = 0.0f;
        f32 anisotropy = 1.0f;
        u64 surface_id = 0u;
        bool wake = false;
        bool active = false;
    };

    static constexpr u32 kBufferedFrames = 4;
    static constexpr u32 kMaxDrawsPerFrame = 64;
    static constexpr u32 kConstantBufferRing =
        kBufferedFrames * kMaxDrawsPerFrame;

    bool AddEvent(u64 surface_id, bool wake,
                  FVec3 world_point, FVec3 direction, f32 anisotropy,
                  f32 radius, f32 strength,
                  f32 initial_age = 0.0f) noexcept;

    bool AddWakeEventForSurface(
        u64 surface_id, FVec3 world_point, FVec3 world_velocity,
        f32 radius, f32 strength, f32 initial_age) noexcept;

    u32 AvailableEventSlots(
        u64 surface_id, bool wake) const noexcept;

    IRhiDevice* m_Device = nullptr;
    TUniquePtr<IRhiShader> m_Vs;
    TUniquePtr<IRhiShader> m_Ps;
    TUniquePtr<IRhiPipeline> m_Pipeline;
    TUniquePtr<IRhiPipeline> m_ManualDepthPipeline;
    TUniquePtr<IRhiBuffer> m_FrameCb[kConstantBufferRing];
    TUniquePtr<IRhiBuffer> m_ObjectCb[kConstantBufferRing];
    u32 m_FrameSlot = 0;
    u32 m_DrawCursor = 0;
    bool m_DrawOverflowLogged = false;
    u32 m_InitBufferCursor = 0;
    bool m_InitializationPending = false;
    TUniquePtr<IRhiTexture> m_NormalMap;
    TUniquePtr<IRhiTexture> m_SceneFallback;

    FWaterSurface3DParams m_Params{};
    FRipple m_Ripples[kMaxStoredRipples]{};

    FMat4 m_ViewProjection{};
    FMat4 m_InverseViewProjection{};
    FVec3 m_CameraPos{0, 2, -5};
    FVec3 m_SunDirection{-0.42f, 0.82f, -0.38f};
    FVec3 m_SunColor{5.0f, 4.4f, 3.8f};
    FVec3 m_EnvironmentZenith{0.12f, 0.14f, 0.16f};
    FVec3 m_EnvironmentHorizon{0.18f, 0.19f, 0.20f};
    FVec3 m_EnvironmentGround{0.025f, 0.028f, 0.032f};
    IRhiTexture* m_ShadowMap = nullptr;
    FMat4 m_LightViewProjection{};
    f32 m_ShadowBias = 0.0012f;
    f32 m_ShadowPcfRadius = 1.5f;
    u32 m_ScreenWidth = 1;
    u32 m_ScreenHeight = 1;
    f32 m_Time = 0.0f;
};

} // namespace acs
