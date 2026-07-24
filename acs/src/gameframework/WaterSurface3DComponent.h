// SPDX-License-Identifier: Apache-2.0
// Authorable 3D interactive-water component shared by editor and runtime.
#pragma once

#include "gameframework/AComponent.h"
#include "render/WaterSurface3D.h"

namespace acs::game {

/**
 * Marks a mesh node as an interactive 3D water surface.
 *
 * The compact public field set is deliberately identical to the editor
 * reflection schema. Scene3D loading can therefore materialize the component
 * and apply authored values directly, while the renderer receives the same
 * sanitized FWaterSurface3DParams through ToRenderParams().
 */
class AWaterSurface3DComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(AWaterSurface3DComponent)

    FVec3 shallowColor{0.055f, 0.38f, 0.50f};
    FVec3 deepColor{0.008f, 0.055f, 0.16f};
    FVec3 absorption{0.34f, 0.13f, 0.040f};
    FVec3 scattering{0.018f, 0.050f, 0.085f};

    /** Henyey-Greenstein phase anisotropy for subsurface scattering. */
    f32 phaseAnisotropy = 0.62f;

    /** Whitewater and contact-foam tint. */
    FVec3 foamColor{0.88f, 0.96f, 1.0f};

    f32 roughness = 0.105f;

    /** Fine normal-map slope strength. */
    f32 normalStrength = 0.82f;

    /** Normal-map repetitions per world unit. */
    f32 normalTiling = 0.075f;

    FVec2 flowDirection{0.92f, 0.38f};

    /** Analytic macro-wave displacement. */
    f32 waveAmplitude = 0.105f;

    /** Analytic macro-wave spatial scale. */
    f32 waveScale = 0.78f;

    /** Wave and normal animation speed. */
    f32 waveSpeed = 0.72f;

    f32 rippleSpeed = 2.65f;
    f32 rippleWavelength = 0.52f;

    /** Seconds before a disturbance reaches a C2-continuous zero. */
    f32 rippleLifetime = 4.0f;

    /** Exponential attenuation applied before the smooth lifetime tail. */
    f32 rippleDamping = 0.78f;

    /** Screen-space refraction displacement strength. */
    f32 refractionStrength = 0.72f;

    f32 opticalDepth = 1.35f;
    f32 foamIntensity = 0.82f;

    FWaterSurface3DParams ToRenderParams() const noexcept {
        FWaterSurface3DParams params{};
        params.shallow_color = shallowColor;
        params.deep_color = deepColor;
        params.absorption = absorption;
        params.scattering = scattering;
        params.phase_anisotropy = phaseAnisotropy;
        params.foam_color = foamColor;
        params.roughness = roughness;
        params.normal_strength = normalStrength;
        params.normal_tiling = normalTiling;
        params.flow_direction = flowDirection;
        params.wave_amplitude = waveAmplitude;
        params.wave_scale = waveScale;
        params.wave_speed = waveSpeed;
        params.ripple_speed = rippleSpeed;
        params.ripple_wavelength = rippleWavelength;
        params.ripple_lifetime = rippleLifetime;
        params.ripple_damping = rippleDamping;
        params.refraction_strength = refractionStrength;
        params.optical_depth = opticalDepth;
        params.foam_intensity = foamIntensity;
        return params;
    }
};

} // namespace acs::game
