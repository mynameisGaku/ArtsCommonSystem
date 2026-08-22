// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/NodeId.h"
#include "math/Vec.h"

namespace acs::game {

/** 有効かつ可視な3D描画形状へのworld-space raycast結果。 */
struct FScene3DRaycastHit {
    /** 命中したscene nodeの世代付きID。 */
    FNodeId Node{};

    /** `RayOrigin + T * RayDirection` で表す命中parameter。 */
    f32 T = 0.0f;

    /** world空間の命中点。 */
    FVec3 Point{};

    /** world空間で正規化し、入力ray側へ向けた面法線。 */
    FVec3 Normal{};

    /** 現在のgraph nodeを指す候補が格納されているかを返す。 */
    bool IsValid() const noexcept { return Node.IsValid(); }
};

} // namespace acs::game
