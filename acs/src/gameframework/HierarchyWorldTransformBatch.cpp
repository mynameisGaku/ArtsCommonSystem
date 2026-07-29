// SPDX-License-Identifier: Apache-2.0
#include "gameframework/HierarchyWorldTransformBatch.h"

#include "gameframework/TransformBatchSoA.h"

#include <algorithm>

namespace acs::game {

bool FHierarchyWorldTransformBatch::PushChildrenReverse(ANode* parent, const FTransform3D& parent_world, usize expected_count) noexcept {
    if (parent == nullptr) return true;
    constexpr u32 kBatchWidth = 16u;
    FVec3 positions[kBatchWidth]{};
    FQuat rotations[kBatchWidth]{};
    FVec3 scales[kBatchWidth]{};
    FVec3 world_positions[kBatchWidth]{};
    FQuat world_rotations[kBatchWidth]{};
    FVec3 world_scales[kBatchWidth]{};
    u32 batch_end = parent->ChildCount();
    while (batch_end > 0u) {
        const u32 first = ((batch_end - 1u) / kBatchWidth) * kBatchWidth;
        const u32 count = batch_end - first;
        for (u32 lane = 0u; lane < count; ++lane) {
            ANode* child = parent->Child(first + lane);
            if (child == nullptr) continue;
            positions[lane] = child->Local().position;
            rotations[lane] = child->Local().rotation;
            scales[lane] = child->Local().scale;
        }
        const bool composed = ComposeTransformBatchSoA(parent_world, FTransformSoAInput{positions, rotations, scales}, world_positions, world_rotations, world_scales, count);
        for (u32 lane = count; lane > 0u; --lane) {
            ANode* child = parent->Child(first + lane - 1u);
            if (child == nullptr) continue;
            if (m_Transforms.Size() >= expected_count || m_Pending.Size() >= expected_count - m_Transforms.Size()) return false;
            const FTransform3D world = composed ? FTransform3D{world_positions[lane - 1u], world_rotations[lane - 1u], world_scales[lane - 1u]} : parent_world.Compose(child->Local());
            if (!m_Pending.TryPushBack(FPendingWorld{child, world})) return false;
        }
        batch_end = first;
    }
    return true;
}

bool FHierarchyWorldTransformBatch::Evaluate(ANode* root, usize expected_count) noexcept {
    Clear();
    if (root == nullptr) return expected_count == 0u;
    if (!m_Pending.TryReserve(expected_count) || !m_Transforms.TryReserve(expected_count) || !PushChildrenReverse(root, root->World(), expected_count)) {
        Clear();
        return false;
    }
    while (!m_Pending.IsEmpty()) {
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

void FHierarchyWorldTransformBatch::Clear() noexcept {
    m_Pending.Clear();
    m_Transforms.Clear();
}

} // namespace acs::game
