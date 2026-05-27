// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — SceneInspector / FSelectionService 実装 (Phase 20)
//
// 設計のポイント (詳細はヘッダ参照):
//   ・現選択 FNodeId (1 個) + (cb, user) 登録 list の最小ハブ。
//   ・selection 値が変化した場合のみ callback を順番に dispatch する。
//   ・登録 / 解除は HotReloadWatcher と同じ規約: 重複弾き / 未登録 no-op。

#include "gameframework/tools/inspector/SelectionService.h"

namespace acs::game::inspector {

// ============================================================================
// Init
// ============================================================================
void FSelectionService::Init() noexcept {
    // 完全初期化: 現選択を invalid に、callback list を空に。
    // 容量は保持 (Reserve 状態を保つ ≒ 再 Init 後のアロケーション節約)。
    m_Current = FNodeId{};
    m_Callbacks.Clear();
}

// ============================================================================
// SelectNode / ClearSelection
// ============================================================================
void FSelectionService::SelectNode(FNodeId id) noexcept {
    // 同一選択への再 Select は no-op (callback 発火しない)。FNodeId 比較は
    // packed u32 の == なので O(1)。
    if (m_Current == id) {
        return;
    }
    const FNodeId from = m_Current;
    m_Current          = id;
    // m_Current 更新後に dispatch することで、callback 内で
    // `CurrentSelection()` を呼んだ場合に新値が見える。
    FireChange(from, m_Current);
}

void FSelectionService::ClearSelection() noexcept {
    // 既に未選択なら no-op (callback 発火しない)。
    if (!m_Current.IsValid()) {
        return;
    }
    const FNodeId from = m_Current;
    m_Current          = FNodeId{};
    FireChange(from, m_Current);
}

// ============================================================================
// 問い合わせ
// ============================================================================
FNodeId FSelectionService::CurrentSelection() const noexcept {
    return m_Current;
}

bool FSelectionService::IsSelected(FNodeId id) const noexcept {
    // FNodeId::operator== は packed u32 の完全一致。
    // invalid 同士の比較も true になる (両方 packed == 0)。
    return m_Current == id;
}

// ============================================================================
// コールバック登録 / 解除
// ============================================================================
void FSelectionService::RegisterCallback(SelectionChangeCallback cb, void* user) noexcept {
    if (cb == nullptr) {
        // null コールバックは silent no-op (HotReloadWatcher と同じ規約だが
        // ここでは log を出さない: editor 用途で頻発しても煩いだけ)。
        return;
    }
    // (cb, user) ペア重複は no-op で弾く (二重 dispatch 防止)。
    // 登録数は購読 panel 数 = せいぜい数〜十数件なので線形探索で十分。
    for (usize i = 0; i < m_Callbacks.Size(); ++i) {
        if (m_Callbacks[i].cb == cb && m_Callbacks[i].user == user) {
            return;
        }
    }
    CallbackEntry e{};
    e.cb   = cb;
    e.user = user;
    m_Callbacks.PushBack(e);
}

void FSelectionService::UnregisterCallback(SelectionChangeCallback cb, void* user) noexcept {
    // null は no-op (Register と対称)。
    if (cb == nullptr) {
        return;
    }
    // (cb, user) 完全一致 1 件のみ削除 (Register が重複弾きしているので
    // 同一ペアは高々 1 件)。順序を保つ必要は仕様上ないので swap-remove。
    const usize n = m_Callbacks.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Callbacks[i].cb == cb && m_Callbacks[i].user == user) {
            m_Callbacks.RemoveAtSwap(i);
            return;
        }
    }
    // 未登録ペアの Unregister は no-op (panel の Shutdown 順序揺らぎを致命化しない)。
}

u32 FSelectionService::CallbackCount() const noexcept {
    return static_cast<u32>(m_Callbacks.Size());
}

void FSelectionService::ClearAll() noexcept {
    // shutdown 想定の一括破棄。callback 発火 (ClearSelection 相当) は **行わない**
    // — 購読側はもはや listening しない前提で呼ぶ API なので、ここで dispatch
    // すると逆に dangling user pointer に飛ぶリスクがある。
    m_Current = FNodeId{};
    m_Callbacks.Clear();
}

// ============================================================================
// 内部ヘルパ: 登録順に dispatch
// ============================================================================
void FSelectionService::FireChange(FNodeId from, FNodeId to) const noexcept {
    // callback 内で別の panel が UnregisterCallback / RegisterCallback を
    // 呼ぶ可能性があるが、Phase 20 では「dispatch 中の自己改変は未定義」と
    // 規定する (登録 list を index で走査するので、swap-remove や PushBack が
    // 走ると同じ要素が 2 回呼ばれる / スキップされる可能性がある)。実用上は
    // editor の panel 構成は起動時に確定するため問題ない。
    const usize n = m_Callbacks.Size();
    for (usize i = 0; i < n; ++i) {
        const CallbackEntry& e = m_Callbacks[i];
        // cb は nullptr で登録されない (RegisterCallback で弾く) ため、
        // ここで再チェックはしない。
        e.cb(e.user, from, to);
    }
}

} // namespace acs::game::inspector
