// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/InputMap.h"
#include "gameframework/OrbitCameraController3D.h"

namespace acs::game {

/**
 * 名前付きactionを3D orbit cameraの5操作軸へ割り当てる値。
 *
 * @details platform入力を直接取得せず、明示されたIInputStateViewだけを評価する。
 * 固定tick、AI、replayは同じaction集合を使って同じcamera入力を生成できる。
 */
struct FOrbitCameraInputActionSet3D final {
    /** 前後移動へ使うaxis action。 */
    FActionId move_forward_action{"MoveForward"};

    /** 左右移動へ使うaxis action。 */
    FActionId move_right_action{"MoveRight"};

    /** 上下移動へ使うaxis action。 */
    FActionId move_up_action{"MoveUp"};

    /** 水平回転へ使うaxis action。 */
    FActionId look_yaw_action{"LookYaw"};

    /** 垂直回転へ使うaxis action。 */
    FActionId look_pitch_action{"LookPitch"};

    /**
     * 全actionが有効かつ互いに異なる場合はtrueを返す。
     * @return 5操作軸を曖昧さなく評価できる場合はtrue。
     */
    bool IsValid() const noexcept;

    /**
     * action mappingと明示入力状態から一回分のorbit camera入力を生成する。
     * @param input_map 物理入力から名前付きactionへの対応。
     * @param input 評価する固定tick、AI、replayなどの明示入力状態。
     * @param output 生成した5軸入力。失敗時は変更しない。
     * @return action集合が有効で全評価値が有限ならtrue。
     */
    bool TryEvaluate(const FInputMap& input_map, const IInputStateView& input, COrbitCameraController3D::FOrbitCameraInput3D& output) const noexcept;
};

} // namespace acs::game
