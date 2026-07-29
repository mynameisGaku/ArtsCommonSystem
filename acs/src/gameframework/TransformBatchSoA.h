// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/Transform3D.h"

namespace acs::game {

/** 連続する transform 群を属性別に参照する入力ビュー。 */
struct FTransformSoAInput {
    /** 子 transform の位置配列。 */
    const FVec3* positions = nullptr;
    /** 子 transform の回転配列。 */
    const FQuat* rotations = nullptr;
    /** 子 transform のスケール配列。 */
    const FVec3* scales = nullptr;
};

/**
 * 一つの親 transform と SoA 配置された子 transform 群をまとめて合成する。
 *
 * @param parent 全ての子へ適用する親 transform。
 * @param local 属性別に配置した子 transform。
 * @param output_positions 合成後の位置配列。
 * @param output_rotations 合成後の回転配列。
 * @param output_scales 合成後のスケール配列。
 * @param count 処理する子 transform 数。
 * @return 入出力が有効なら true。
 */
inline bool ComposeTransformBatchSoA(const FTransform3D& parent, FTransformSoAInput local, FVec3* output_positions, FQuat* output_rotations, FVec3* output_scales, usize count) noexcept {
    if (count == 0u) return true;
    if (local.positions == nullptr || local.rotations == nullptr || local.scales == nullptr || output_positions == nullptr || output_rotations == nullptr || output_scales == nullptr) {
        return false;
    }
    for (usize index = 0u; index < count; ++index) {
        // 合成前の子 transform を連続配列から読み出す。
        const FVec3 local_position = local.positions[index];
        const FQuat local_rotation = local.rotations[index];
        const FVec3 local_scale = local.scales[index];
        // 親スケールを適用した子位置。
        const FVec3 scaled = {parent.scale.x * local_position.x, parent.scale.y * local_position.y, parent.scale.z * local_position.z};
        output_positions[index] = parent.position + Rotate(parent.rotation, scaled);
        output_rotations[index] = local_rotation * parent.rotation;
        output_scales[index] = {parent.scale.x * local_scale.x, parent.scale.y * local_scale.y, parent.scale.z * local_scale.z};
    }
    return true;
}

} // namespace acs::game
