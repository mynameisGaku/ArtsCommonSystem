// SPDX-License-Identifier: Apache-2.0
#include "gameframework/HierarchyWorldTransformBatch.h"

#include "gameframework/TransformBatchSoA.h"

#include <algorithm>

namespace acs::game {

/** 子を SoA 合成し、pre-order 維持のため逆順 stack へ積む。 */
bool CHierarchyWorldTransformBatch::PushChildrenReverse(ANode* parent, const FTransform3D& parent_world, usize expected_count) noexcept {
    if (parent == nullptr) return true;
    /** 一度に合成する child 数。 */
    constexpr u32 kBatchWidth = 16u;
    /** child local position の SoA 入力。 */
    FVec3 positions[kBatchWidth]{};
    /** child local rotation の SoA 入力。 */
    FQuat rotations[kBatchWidth]{};
    /** child local scale の SoA 入力。 */
    FVec3 scales[kBatchWidth]{};
    /** 合成後 world position。 */
    FVec3 world_positions[kBatchWidth]{};
    /** 合成後 world rotation。 */
    FQuat world_rotations[kBatchWidth]{};
    /** 合成後 world scale。 */
    FVec3 world_scales[kBatchWidth]{};
    /** 未処理範囲の終端 child index。 */
    u32 batch_end = parent->ChildCount();
    while (batch_end > 0u) {
        /** 現在 batch の先頭 child index。 */
        const u32 first = ((batch_end - 1u) / kBatchWidth) * kBatchWidth;
        /** 現在 batch の child 数。 */
        const u32 count = batch_end - first;
        for (u32 lane = 0u; lane < count; ++lane) {
            /** lane に対応する child。 */
            ANode* child = parent->Child(first + lane);
            if (child == nullptr) continue;
            positions[lane] = child->Local().position;
            rotations[lane] = child->Local().rotation;
            scales[lane] = child->Local().scale;
        }
        /** SoA fast path が入力を合成できたか。 */
        const bool composed = ComposeTransformBatchSoA(parent_world, FTransformSoAInput{positions, rotations, scales}, world_positions, world_rotations, world_scales, count);
        for (u32 lane = count; lane > 0u; --lane) {
            /** stack へ積む child。 */
            ANode* child = parent->Child(first + lane - 1u);
            if (child == nullptr) continue;
            if (m_Transforms.Size() >= expected_count || m_Pending.Size() >= expected_count - m_Transforms.Size()) return false;
            /** fast path または scalar fallback の world transform。 */
            const FTransform3D world = composed ? FTransform3D{world_positions[lane - 1u], world_rotations[lane - 1u], world_scales[lane - 1u]} : parent_world.Compose(child->Local());
            if (!m_Pending.TryPushBack(FPendingWorld{child, world})) return false;
        }
        batch_end = first;
    }
    return true;
}

/** root 配下を非再帰 pre-order で一括合成する。 */
bool CHierarchyWorldTransformBatch::Evaluate(ANode* root, usize expected_count) noexcept {
    Clear();
    if (root == nullptr) return expected_count == 0u;
    if (!m_Pending.TryReserve(expected_count) || !m_Transforms.TryReserve(expected_count) || !PushChildrenReverse(root, root->World(), expected_count)) {
        Clear();
        return false;
    }
    while (!m_Pending.IsEmpty()) {
        /** 次に処理する node と確定 world transform。 */
        const FPendingWorld pending = m_Pending[m_Pending.Size() - 1u];
        m_Pending.PopBack();
        if (!m_Transforms.TryPushBack(pending.world) || !PushChildrenReverse(pending.node, pending.world, expected_count)) {
            Clear();
            return false;
        }
    }
    if (m_Transforms.Size() != expected_count) {
        Clear();
        return false;
    }
    return true;
}

/** 結果と pending stack を空に戻す。 */
void CHierarchyWorldTransformBatch::Clear() noexcept {
    m_Pending.Clear();
    m_Transforms.Clear();
}

} // namespace acs::game
