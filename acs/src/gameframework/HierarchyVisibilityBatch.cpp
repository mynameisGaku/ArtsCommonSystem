// SPDX-License-Identifier: Apache-2.0
#include "gameframework/HierarchyVisibilityBatch.h"

namespace acs::game {

namespace {

/** node 自身の描画有効状態だけを返す。 */
bool IsLocallyVisible(const ANode* node) noexcept {
    return node != nullptr && node->IsVisible() && node->IsEnabled() && !node->IsPendingDestroy();
}

} // namespace

/** 一 node から root まで scalar に可視性を検査する。 */
bool CHierarchyVisibilityBatch::EvaluateScalar(const ANode* node) noexcept {
    for (const ANode* current = node; current != nullptr; current = current->Parent()) {
        if (!IsLocallyVisible(current)) return false;
    }
    return node != nullptr;
}

/** pre-order node 列を stack 再利用で一括評価する。 */
bool CHierarchyVisibilityBatch::Evaluate(ANode* const* nodes, usize count, const ANode* boundary_root) noexcept {
    Clear();
    if (count == 0u) return true;
    if (count == static_cast<usize>(-1) || nodes == nullptr || !m_Visibility.TrySetNum(count) || !m_Stack.TryReserve(count + 1u)) {
        Clear();
        return false;
    }
    if (boundary_root != nullptr) {
        const FStackEntry root_entry{boundary_root, EvaluateScalar(boundary_root)};
        if (!m_Stack.TryAdd(root_entry)) {
            Clear();
            return false;
        }
    }
    for (usize index = 0u; index < count; ++index) {
        /** 現在評価する node。 */
        ANode* node = nodes[index];
        if (node == nullptr) {
            m_Visibility[index] = 0u;
            continue;
        }
        /** pre-order 上の親 node。 */
        const ANode* parent = node->Parent();
        while (!m_Stack.IsEmpty() && m_Stack[m_Stack.Num() - 1u].node != parent) m_Stack.Pop();
        /** ancestor を含めた現在 node の可視性。 */
        bool visible = false;
        if (parent == nullptr) {
            visible = IsLocallyVisible(node);
        } else if (!m_Stack.IsEmpty()) {
            visible = m_Stack[m_Stack.Num() - 1u].visible && IsLocallyVisible(node);
        } else {
            visible = EvaluateScalar(node);
            ++m_ScalarFallbackCount;
            if (boundary_root != nullptr) {
                const FStackEntry root_entry{boundary_root, EvaluateScalar(boundary_root)};
                if (!m_Stack.TryAdd(root_entry)) {
                    Clear();
                    return false;
                }
            }
        }
        m_Visibility[index] = visible ? 1u : 0u;
        const FStackEntry entry{node, visible};
        if (!m_Stack.TryAdd(entry)) {
            Clear();
            return false;
        }
    }
    m_Stack.Reset();
    return true;
}

/** 結果と作業 stack を空に戻す。 */
void CHierarchyVisibilityBatch::Clear() noexcept {
    m_Visibility.Reset();
    m_Stack.Reset();
    m_ScalarFallbackCount = 0u;
}

/** 指定結果 index の可視性を範囲検査して返す。 */
bool CHierarchyVisibilityBatch::IsVisible(usize index) const noexcept {
    return index < m_Visibility.Num() && m_Visibility[index] != 0u;
}

} // namespace acs::game
