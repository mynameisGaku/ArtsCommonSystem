// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "gameframework/ANode.h"

namespace acs::game {

/**
 * DFS pre-order の world transform を保持 scratch で一括構築する。
 *
 * @details 明示 stack により階層深さに比例する C++ call stack を使わない。
 * sibling は SoA batch で合成し、失敗時は部分結果を公開しない。成功後の再評価は
 * 確保済み容量を再利用する。
 */
class CHierarchyWorldTransformBatch {
public:
    /**
     * root 自身を除く subtree を DFS pre-order で評価する。
     *
     * @param root 評価境界の root。
     * @param expected_count root 配下の非 null node 数。
     * @return 全 node を順序どおり評価できた場合 true。
     */
    bool Evaluate(ANode* root, usize expected_count) noexcept;

    /** 結果と作業 stack の要素数を 0 にし、確保済み容量は保持する。 */
    void Clear() noexcept;

    /** 成功した直近評価の world transform 配列を返す。 */
    const FTransform3D* Transforms() const noexcept { return m_Transforms.GetData(); }

    /** 成功した直近評価の node 数を返す。 */
    usize Count() const noexcept { return m_Transforms.Num(); }

    /** 指定 index の world transform を返す。 */
    const FTransform3D& At(usize index) const noexcept { return m_Transforms[index]; }

private:
    /** 未評価 node と計算済み world transform。 */
    struct FPendingWorld {
        /** 評価対象 node。 */
        ANode* node = nullptr;

        /** node の world transform。 */
        FTransform3D world{};
    };

    /** 子を逆順で作業 stack へ積む。 */
    bool PushChildrenReverse(ANode* parent, const FTransform3D& parent_world, usize expected_count) noexcept;

    /** DFS の未評価 node。 */
    TArray<FPendingWorld> m_Pending;

    /** DFS pre-order の計算結果。 */
    TArray<FTransform3D> m_Transforms;
};

/** 旧名を使う既存ソースとの互換alias。 */
using FHierarchyWorldTransformBatch = CHierarchyWorldTransformBatch;

} // namespace acs::game
