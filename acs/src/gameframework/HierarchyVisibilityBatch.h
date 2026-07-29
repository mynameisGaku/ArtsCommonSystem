// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "gameframework/ANode.h"

namespace acs::game {

/**
 * DFS pre-order の階層可視性を一括評価して保持する。
 *
 * @details 親が入力内で先行する通常経路は stack を使って線形時間で評価する。
 * 順序が不正な要素だけ scalar 判定へ戻り、結果の互換性を維持する。
 */
class FHierarchyVisibilityBatch {
public:
    /**
     * 単一 node の継承可視性を評価する。
     *
     * @param node 評価対象。
     * @return 自身と全祖先が有効かつ可視なら true。
     */
    static bool EvaluateScalar(const ANode* node) noexcept;

    /**
     * DFS pre-order の node 群を一括評価する。
     *
     * @param nodes 評価対象の連続配列。
     * @param count node 数。
     * @param boundary_root 入力直前の共通親。不要なら nullptr。
     * @return scratch を用意して全件評価できた場合は true。
     */
    bool Evaluate(ANode* const* nodes, usize count, const ANode* boundary_root) noexcept;

    /** 評価結果を破棄し、確保容量は保持する。 */
    void Clear() noexcept;

    /** 指定 index が有効かつ可視か返す。 */
    bool IsVisible(usize index) const noexcept;

    /** 評価済み node 数を返す。 */
    usize Count() const noexcept { return m_Visibility.Size(); }

    /** 順序不整合で scalar fallback した件数を返す。 */
    u32 ScalarFallbackCount() const noexcept { return m_ScalarFallbackCount; }

private:
    /** DFS 走査中の祖先状態。 */
    struct FStackEntry {
        /** 祖先 node。 */
        const ANode* node = nullptr;

        /** 祖先までを含む可視性。 */
        bool visible = false;
    };

    /** 評価済み可視性。 */
    TArray<u8> m_Visibility;

    /** 現在の祖先 stack。 */
    TArray<FStackEntry> m_Stack;

    /** scalar fallback 件数。 */
    u32 m_ScalarFallbackCount = 0u;
};

} // namespace acs::game
