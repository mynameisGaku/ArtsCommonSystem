// SPDX-License-Identifier: Apache-2.0
// 3D interactive water surface: layered normal map, analytic waves, refraction,
// Fresnel reflection, GGX sun highlights, foam, and persistent disturbances.
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "memory/UniquePtr.h"
#include "render/RenderAssets.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiBuffer;
class IRhiCommandList;
class IRhiDevice;
class IRhiPipeline;
class IRhiShader;
class IRhiTexture;

/**
 * Authoring parameters for FWaterSurface3D.
 *
 * @details
 * The surface is physically based around water IOR 1.333. Colors and absorption
 * describe the volume below the surface, while analytic waves and a generated
 * tileable normal map provide independent macro/micro detail. The renderer is
 * intentionally specialized for a horizontal world-XZ surface: displacement is
 * along world +Y and every disturbance ignores the supplied world-point Y.
 */
struct FWaterSurface3DParams {
    /** Shallow-water in-scattering color. */
    FVec3 shallow_color{0.055f, 0.38f, 0.50f};

    /** Deep-water in-scattering color. */
    FVec3 deep_color{0.008f, 0.055f, 0.16f};

    /** Beer-Lambert absorption coefficient in inverse world units. */
    FVec3 absorption{0.34f, 0.13f, 0.040f};

    /** Whitewater/contact-foam color. */
    FVec3 foam_color{0.88f, 0.96f, 1.0f};

    /** Main flow direction in the horizontal XZ plane. */
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
 * horizontal mesh. The mesh and model transform must keep the surface in the
 * world XZ plane; tilted/arbitrarily oriented water is not supported. Dynamic
 * disturbances are stored independently and are never overwritten while active.
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
    /** Maximum simultaneously visible disturbances. */
    static constexpr u32 kMaxRipples = 64;

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
     * Adds one directional, elongated wake event from a moving body.
     *
     * @return true when accepted; false when the wake-reserved pool is full.
     */
    bool AddWake(FVec3 world_point, FVec3 world_velocity,
                 f32 radius, f32 strength) noexcept;

    /** Immediately removes every active disturbance. */
    void ClearDisturbances() noexcept;

    /** Number of active persistent disturbance slots. */
    u32 ActiveRippleCount() const noexcept;

    /** Replaces all authoring parameters used from the next draw onward. */
    void SetParams(const FWaterSurface3DParams& params) noexcept {
        m_Params = params;
    }

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
     * @details The mesh/model must describe a horizontal world-XZ surface.
     * When scene_depth is supplied, the caller must not bind that same texture
     * as a DSV for this pass. DrawMesh selects a no-DSV pipeline and performs
     * the opaque-scene depth test in the pixel shader.
     *
     * @param scene_color Copy of the opaque scene rendered before the water.
     *        nullptr selects a safe generated fallback while keeping all other
     *        reflection and lighting effects active.
     * @param scene_depth Optional shader-visible opaque-scene depth. Enables
     *        manual foreground occlusion, reconstructed optical thickness, and
     *        contact foam.
     * @param screen_reflection Optional SSR/planar reflection texture. RGB is
     *        reflected radiance and alpha is the valid-hit mask.
     */
    void DrawMesh(IRhiCommandList& command_list, const FGpuMesh& mesh,
                  const FMat4& model,
                  IRhiTexture* scene_color = nullptr,
                  IRhiTexture* scene_depth = nullptr,
                  IRhiTexture* screen_reflection = nullptr) noexcept;

    IRhiPipeline* Pipeline() const noexcept { return m_Pipeline.Get(); }
    IRhiTexture* NormalTexture() const noexcept { return m_NormalMap.Get(); }
    f32 Time() const noexcept { return m_Time; }

private:
    struct FRipple {
        FVec2 center{0, 0};
        FVec2 direction{1, 0};
        f32 initial_radius = 0.0f;
        f32 initial_amplitude = 0.0f;
        f32 amplitude = 0.0f;
        f32 age = 0.0f;
        f32 speed = 0.0f;
        f32 lifetime = 0.0f;
        f32 anisotropy = 1.0f;
        bool active = false;
    };

    static constexpr u32 kBufferedFrames = 4;
    static constexpr u32 kMaxDrawsPerFrame = 64;
    static constexpr u32 kConstantBufferRing =
        kBufferedFrames * kMaxDrawsPerFrame;

    bool AddEvent(FVec3 world_point, FVec2 direction, f32 anisotropy,
                  f32 radius, f32 strength,
                  u32 first_slot, u32 slot_count) noexcept;

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
    TUniquePtr<IRhiTexture> m_NormalMap;
    TUniquePtr<IRhiTexture> m_SceneFallback;

    FWaterSurface3DParams m_Params{};
    FRipple m_Ripples[kMaxRipples]{};

    FMat4 m_ViewProjection{};
    FMat4 m_InverseViewProjection{};
    FVec3 m_CameraPos{0, 2, -5};
    FVec3 m_SunDirection{-0.42f, 0.82f, -0.38f};
    FVec3 m_SunColor{5.0f, 4.4f, 3.8f};
    IRhiTexture* m_ShadowMap = nullptr;
    FMat4 m_LightViewProjection{};
    f32 m_ShadowBias = 0.0012f;
    f32 m_ShadowPcfRadius = 1.5f;
    u32 m_ScreenWidth = 1;
    u32 m_ScreenHeight = 1;
    f32 m_Time = 0.0f;
};

} // namespace acs
