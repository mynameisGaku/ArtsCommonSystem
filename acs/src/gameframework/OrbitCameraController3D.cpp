// SPDX-License-Identifier: Apache-2.0
#include "gameframework/OrbitCameraController3D.h"

#include "math/Math.h"

#include <cmath>

namespace acs::game {
namespace {

/** 円周角を[-pi, pi]へ折り返す。 */
f32 WrapRadians(f32 radians) noexcept
{
    constexpr f32 Pi = 3.14159265358979323846f;
    constexpr f32 TwoPi = Pi * 2.0f;
    if (radians >= -Pi && radians <= Pi) return radians;
    f32 wrapped = std::fmod(radians + Pi, TwoPi);
    if (wrapped < 0.0f) wrapped += TwoPi;
    return wrapped - Pi;
}

/** 値を[-1, 1]へ制限する。 */
f32 ClampAxis(f32 value) noexcept
{
    if (value < -1.0f) return -1.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

/** 3D値の全成分が有限ならtrueを返す。 */
bool IsFinite(FVec3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/** controller設定が安全な範囲ならtrueを返す。 */
bool IsValidSettings(const COrbitCameraController3D::FOrbitCameraSettings3D& settings) noexcept
{
    constexpr f32 HalfPi = 1.57079632679489661923f;
    const bool valid_yaw = std::isfinite(settings.yaw_radians_per_second) && settings.yaw_radians_per_second >= 0.0f;
    const bool valid_pitch = std::isfinite(settings.pitch_radians_per_second) && settings.pitch_radians_per_second >= 0.0f;
    const bool valid_movement = std::isfinite(settings.movement_distance_scale_per_second) && settings.movement_distance_scale_per_second >= 0.0f;
    const bool valid_distance = std::isfinite(settings.minimum_movement_distance) && settings.minimum_movement_distance > 0.0f;
    const bool valid_limit = std::isfinite(settings.pitch_limit_radians) && settings.pitch_limit_radians > 0.0f && settings.pitch_limit_radians < HalfPi;
    const bool valid_zoom = std::isfinite(settings.zoom_distance_scale_per_second) && settings.zoom_distance_scale_per_second >= 0.0f;
    const bool valid_zoom_range = std::isfinite(settings.minimum_distance) && std::isfinite(settings.maximum_distance) && settings.minimum_distance > 0.0f && settings.maximum_distance >= settings.minimum_distance;
    return valid_yaw && valid_pitch && valid_movement && valid_distance && valid_limit && valid_zoom && valid_zoom_range;
}

/** 外部所有stateが計算可能ならtrueを返す。 */
bool IsValidState(const COrbitCameraController3D::FOrbitCameraState3D& state) noexcept
{
    const bool valid_angles = std::isfinite(state.yaw_radians) && std::isfinite(state.pitch_radians);
    const bool valid_distance = std::isfinite(state.distance) && state.distance > 0.0f;
    return IsFinite(state.target) && valid_angles && valid_distance;
}

/** 一回分の全入力が有限ならtrueを返す。 */
bool IsValidInput(const COrbitCameraController3D::FOrbitCameraInput3D& input) noexcept
{
    const bool valid_movement = std::isfinite(input.move_forward) && std::isfinite(input.move_right) && std::isfinite(input.move_up);
    const bool valid_look = std::isfinite(input.look_yaw) && std::isfinite(input.look_pitch);
    return valid_movement && valid_look && std::isfinite(input.zoom);
}

/** stateが現在設定から安全なviewを構築できる範囲ならtrueを返す。 */
bool IsViewState(const COrbitCameraController3D::FOrbitCameraSettings3D& settings, const COrbitCameraController3D::FOrbitCameraState3D& state) noexcept
{
    if (!IsValidState(state)) return false;
    if (state.pitch_radians < -settings.pitch_limit_radians || state.pitch_radians > settings.pitch_limit_radians)
        return false;
    return state.distance >= settings.minimum_distance && state.distance <= settings.maximum_distance;
}

} // namespace

/** 設定を隔離検証し、成功時だけ現在設定へ反映する。 */
bool COrbitCameraController3D::TryConfigure(const FOrbitCameraSettings3D& settings) noexcept
{
    if (!IsValidSettings(settings)) return false;
    m_Settings = settings;
    return true;
}

/** 入力を角度とworld移動へ変換し、成功時だけstateを更新する。 */
bool COrbitCameraController3D::TryStep(const FOrbitCameraInput3D& input, f32 delta_seconds, FOrbitCameraState3D& state) const noexcept
{
    if (!IsValidSettings(m_Settings) || !IsValidInput(input)) return false;
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0f) return false;
    if (!IsValidState(state)) return false;

    FOrbitCameraState3D candidate = state;
    const f32 yaw_delta = ClampAxis(input.look_yaw) * m_Settings.yaw_radians_per_second * delta_seconds;
    candidate.yaw_radians = WrapRadians(candidate.yaw_radians + yaw_delta);
    candidate.pitch_radians += ClampAxis(input.look_pitch) * m_Settings.pitch_radians_per_second * delta_seconds;
    if (candidate.pitch_radians > m_Settings.pitch_limit_radians)
        candidate.pitch_radians = m_Settings.pitch_limit_radians;
    if (candidate.pitch_radians < -m_Settings.pitch_limit_radians)
        candidate.pitch_radians = -m_Settings.pitch_limit_radians;

    if (candidate.distance < m_Settings.minimum_distance) candidate.distance = m_Settings.minimum_distance;
    if (candidate.distance > m_Settings.maximum_distance) candidate.distance = m_Settings.maximum_distance;
    const f64 zoom_axis = static_cast<f64>(ClampAxis(input.zoom));
    const f64 zoom_distance = static_cast<f64>(candidate.distance) * static_cast<f64>(m_Settings.zoom_distance_scale_per_second) * static_cast<f64>(delta_seconds);
    const f64 next_distance = static_cast<f64>(candidate.distance) - zoom_axis * zoom_distance;
    if (next_distance <= static_cast<f64>(m_Settings.minimum_distance))
        candidate.distance = m_Settings.minimum_distance;
    else if (next_distance >= static_cast<f64>(m_Settings.maximum_distance))
        candidate.distance = m_Settings.maximum_distance;
    else
        candidate.distance = static_cast<f32>(next_distance);

    const f32 yaw_sine = Sin(candidate.yaw_radians);
    const f32 yaw_cosine = Cos(candidate.yaw_radians);
    const FVec3 horizontal_forward{yaw_sine, 0.0f, yaw_cosine};
    const FVec3 right{yaw_cosine, 0.0f, -yaw_sine};
    const FVec3 forward_movement = horizontal_forward * ClampAxis(input.move_forward);
    const FVec3 right_movement = right * ClampAxis(input.move_right);
    const FVec3 up_movement = FVec3::UnitY() * ClampAxis(input.move_up);
    FVec3 movement = forward_movement + right_movement + up_movement;
    const f32 movement_length_squared = LengthSq(movement);
    if (m_Settings.normalize_movement && movement_length_squared > 1.0f)
        movement = movement * (1.0f / Sqrt(movement_length_squared));

    const f32 movement_distance = candidate.distance > m_Settings.minimum_movement_distance ? candidate.distance : m_Settings.minimum_movement_distance;
    const f32 travel_distance = movement_distance * m_Settings.movement_distance_scale_per_second * delta_seconds;
    candidate.target += movement * travel_distance;
    if (!IsValidState(candidate)) return false;
    state = candidate;
    return true;
}

/** 前回と現在の固定tick状態を最短yaw経路で描画用状態へ補間する。 */
bool COrbitCameraController3D::TryInterpolateState(const FOrbitCameraState3D& previous, const FOrbitCameraState3D& current, f64 interpolation_alpha, FOrbitCameraState3D& output) const noexcept
{
    if (!IsValidSettings(m_Settings) || !IsViewState(m_Settings, previous) || !IsViewState(m_Settings, current))
        return false;
    if (!std::isfinite(interpolation_alpha) || interpolation_alpha < 0.0 || interpolation_alpha > 1.0) return false;

    /** float状態へ適用する検証済み補間率。 */
    const f32 blend = static_cast<f32>(interpolation_alpha);
    /** ±pi境界を越える場合も長回りさせない水平角差。 */
    const f32 yaw_delta = WrapRadians(current.yaw_radians - previous.yaw_radians);
    /** 全項目を検証してから公開する描画用候補。 */
    FOrbitCameraState3D candidate{};
    candidate.target = Lerp(previous.target, current.target, blend);
    candidate.yaw_radians = WrapRadians(previous.yaw_radians + yaw_delta * blend);
    candidate.pitch_radians = Lerp(previous.pitch_radians, current.pitch_radians, blend);
    candidate.distance = Lerp(previous.distance, current.distance, blend);
    if (!IsViewState(m_Settings, candidate)) return false;
    output = candidate;
    return true;
}

/** previous/currentを変更せず、現在設定で復元可能な組か検証する。 */
bool COrbitCameraController3D::IsSnapshotValid(const FOrbitCameraFixedStepSnapshot3D& snapshot) const noexcept
{
    return IsValidSettings(m_Settings) && IsViewState(m_Settings, snapshot.previous) && IsViewState(m_Settings, snapshot.current);
}

/** desired stateを維持し、障害物の手前へ置くpresentation stateだけを計算する。 */
bool COrbitCameraController3D::TryResolveObstructedState(const FOrbitCameraState3D& state, f32 obstruction_distance, f32 camera_clearance, FOrbitCameraState3D& output) const noexcept
{
    if (!IsValidSettings(m_Settings) || !IsViewState(m_Settings, state)) return false;
    if (!std::isfinite(obstruction_distance) || !std::isfinite(camera_clearance) || obstruction_distance < 0.0f || obstruction_distance > state.distance || camera_clearance < 0.0f)
        return false;
    const f32 unobstructed_distance = obstruction_distance - camera_clearance;
    if (!std::isfinite(unobstructed_distance) || unobstructed_distance < m_Settings.minimum_distance) return false;
    FOrbitCameraState3D candidate = state;
    candidate.distance = unobstructed_distance;
    if (!IsViewState(m_Settings, candidate)) return false;
    output = candidate;
    return true;
}

/** orbit状態をLegacyScene3Dと同じ左手系view座標へ変換する。 */
bool COrbitCameraController3D::TryBuildView(const FOrbitCameraState3D& state, FOrbitCameraView3D& view) const noexcept
{
    if (!IsValidSettings(m_Settings) || !IsViewState(m_Settings, state)) return false;

    const f32 pitch_cosine = Cos(state.pitch_radians);
    const FVec3 forward{Sin(state.yaw_radians) * pitch_cosine, -Sin(state.pitch_radians), Cos(state.yaw_radians) * pitch_cosine};
    FOrbitCameraView3D candidate{};
    candidate.eye = state.target - forward * state.distance;
    candidate.look_at = state.target;
    candidate.up = FVec3::UnitY();
    if (!IsFinite(candidate.eye) || !IsFinite(candidate.look_at)) return false;
    view = candidate;
    return true;
}

} // namespace acs::game
