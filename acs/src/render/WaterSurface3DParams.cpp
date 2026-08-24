// SPDX-License-Identifier: Apache-2.0
#include "render/WaterSurface3DParams.h"

namespace acs {

/** 水域用途に合う共通の波、流れ、光学値を返す。 */
FWaterSurface3DParams FWaterSurface3DParams::ForProfile(EWaterSurface3DProfile profile) noexcept
{
    /** 既存defaultから用途別に上書きする戻り値。 */
    FWaterSurface3DParams result{};
    switch (profile) {
    case EWaterSurface3DProfile::Puddle:
        result.shallow_color = FVec3{0.16f, 0.25f, 0.28f};
        result.deep_color = FVec3{0.035f, 0.085f, 0.105f};
        result.absorption = FVec3{0.080f, 0.035f, 0.020f};
        result.scattering = FVec3{0.002f, 0.004f, 0.006f};
        result.phase_anisotropy = 0.05f;
        result.flow_direction = FVec2{0.74f, 0.67f};
        result.roughness = 0.12f;
        result.normal_strength = 0.42f;
        result.normal_tiling = 0.95f;
        result.refraction_strength = 0.18f;
        result.optical_depth = 0.035f;
        result.wave_amplitude = 0.004f;
        result.wave_scale = 7.0f;
        result.wave_speed = 0.22f;
        result.ripple_speed = 0.55f;
        result.ripple_wavelength = 0.065f;
        result.ripple_lifetime = 0.90f;
        result.ripple_damping = 2.80f;
        result.foam_intensity = 0.0f;
        break;
    case EWaterSurface3DProfile::Pool:
        result.shallow_color = FVec3{0.028f, 0.30f, 0.38f};
        result.deep_color = FVec3{0.004f, 0.075f, 0.15f};
        result.absorption = FVec3{0.12f, 0.045f, 0.018f};
        result.scattering = FVec3{0.002f, 0.008f, 0.014f};
        result.phase_anisotropy = 0.20f;
        result.flow_direction = FVec2{0.25f, 0.97f};
        result.roughness = 0.055f;
        result.normal_strength = 0.48f;
        result.normal_tiling = 0.38f;
        result.refraction_strength = 0.32f;
        result.optical_depth = 1.80f;
        result.wave_amplitude = 0.010f;
        result.wave_scale = 3.60f;
        result.wave_speed = 0.38f;
        result.ripple_speed = 1.20f;
        result.ripple_wavelength = 0.16f;
        result.ripple_lifetime = 2.0f;
        result.ripple_damping = 1.30f;
        result.foam_intensity = 0.08f;
        break;
    case EWaterSurface3DProfile::River:
        result.shallow_color = FVec3{0.065f, 0.29f, 0.27f};
        result.deep_color = FVec3{0.008f, 0.070f, 0.095f};
        result.absorption = FVec3{0.40f, 0.18f, 0.065f};
        result.scattering = FVec3{0.025f, 0.055f, 0.060f};
        result.phase_anisotropy = 0.55f;
        result.flow_direction = FVec2{0.0f, 1.0f};
        result.roughness = 0.105f;
        result.normal_strength = 0.85f;
        result.normal_tiling = 0.18f;
        result.refraction_strength = 0.38f;
        result.optical_depth = 1.0f;
        result.wave_amplitude = 0.032f;
        result.wave_scale = 1.60f;
        result.wave_speed = 1.60f;
        result.ripple_speed = 2.20f;
        result.ripple_wavelength = 0.32f;
        result.ripple_lifetime = 2.40f;
        result.ripple_damping = 1.20f;
        result.foam_intensity = 0.55f;
        break;
    case EWaterSurface3DProfile::Ocean:
        result.shallow_color = FVec3{0.030f, 0.23f, 0.34f};
        result.deep_color = FVec3{0.0015f, 0.018f, 0.075f};
        result.absorption = FVec3{0.32f, 0.115f, 0.028f};
        result.scattering = FVec3{0.012f, 0.035f, 0.070f};
        result.phase_anisotropy = 0.70f;
        result.flow_direction = FVec2{0.96f, 0.28f};
        result.roughness = 0.065f;
        result.normal_strength = 0.62f;
        result.normal_tiling = 0.045f;
        result.refraction_strength = 0.28f;
        result.optical_depth = 12.0f;
        result.wave_amplitude = 0.55f;
        result.wave_scale = 0.11f;
        result.wave_speed = 0.90f;
        result.ripple_speed = 3.40f;
        result.ripple_wavelength = 0.90f;
        result.ripple_lifetime = 6.0f;
        result.ripple_damping = 0.50f;
        result.foam_intensity = 1.10f;
        break;
    case EWaterSurface3DProfile::Lake:
    default:
        result.shallow_color = FVec3{0.055f, 0.32f, 0.42f};
        result.deep_color = FVec3{0.006f, 0.045f, 0.12f};
        result.absorption = FVec3{0.30f, 0.12f, 0.035f};
        result.scattering = FVec3{0.012f, 0.035f, 0.060f};
        result.phase_anisotropy = 0.55f;
        result.roughness = 0.085f;
        result.normal_strength = 0.48f;
        result.normal_tiling = 0.11f;
        result.refraction_strength = 0.38f;
        result.optical_depth = 1.50f;
        result.wave_amplitude = 0.055f;
        result.wave_scale = 0.65f;
        result.wave_speed = 0.62f;
        result.foam_intensity = 0.55f;
        break;
    }
    return result;
}

} // namespace acs
