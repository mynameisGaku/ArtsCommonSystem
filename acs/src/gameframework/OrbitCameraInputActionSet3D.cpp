// SPDX-License-Identifier: Apache-2.0
#include "gameframework/OrbitCameraInputActionSet3D.h"

#include <cmath>

namespace acs::game {

/** 全actionが有効かつ互いに異なる場合はtrueを返す。 */
bool FOrbitCameraInputActionSet3D::IsValid() const noexcept
{
    /** 操作軸の宣言順に並べた検証対象。 */
    const FActionId actions[]{move_forward_action, move_right_action, move_up_action, look_yaw_action, look_pitch_action, zoom_action};
    for (usize index = 0u; index < sizeof(actions) / sizeof(actions[0]); ++index) {
        if (actions[index].value == 0u) return false;
        for (usize other = index + 1u; other < sizeof(actions) / sizeof(actions[0]); ++other) {
            if (actions[index] == actions[other]) return false;
        }
    }
    return true;
}

/** 明示入力状態の6actionを評価し、成功時だけcamera入力へ反映する。 */
bool FOrbitCameraInputActionSet3D::TryEvaluate(const FInputMap& input_map, const IInputStateView& input, COrbitCameraController3D::FOrbitCameraInput3D& output) const noexcept
{
    if (!IsValid()) return false;

    /** 失敗時に呼び出し側の値を保つための一時出力。 */
    COrbitCameraController3D::FOrbitCameraInput3D candidate{};
    candidate.move_forward = input_map.Evaluate(move_forward_action, input).axis;
    candidate.move_right = input_map.Evaluate(move_right_action, input).axis;
    candidate.move_up = input_map.Evaluate(move_up_action, input).axis;
    candidate.look_yaw = input_map.Evaluate(look_yaw_action, input).axis;
    candidate.look_pitch = input_map.Evaluate(look_pitch_action, input).axis;
    candidate.zoom = input_map.Evaluate(zoom_action, input).axis;
    if (!std::isfinite(candidate.move_forward) || !std::isfinite(candidate.move_right) || !std::isfinite(candidate.move_up) || !std::isfinite(candidate.look_yaw) || !std::isfinite(candidate.look_pitch) || !std::isfinite(candidate.zoom)) return false;
    output = candidate;
    return true;
}

} // namespace acs::game
