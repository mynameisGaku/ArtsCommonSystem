// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — SceneInspector / FSelectionService (Phase 20 editor 第二弾)
//
// 現在「選択されている FNodeId」を全 editor panel (FHierarchyPanel /
// FInspectorPanel / FEditorToolbar / SceneView 等) で共有するための **ハブ**。
// 1 個のシングルなインスタンスを editor 起動コードが所有し、各 panel が
// `RegisterCallback` で選択変更を購読する形を取る。
//
// 使い方:
//   acs::game::inspector::FSelectionService sel;
//   sel.Init();
//
//   // panel A: コールバックで自分の描画状態を更新
//   static void OnSelChanged(void* user, FNodeId from, FNodeId to) noexcept {
//       auto* self = static_cast<FHierarchyPanel*>(user);
//       self->ScrollTo(to);
//   }
//   sel.RegisterCallback(&OnSelChanged, &hierarchy);
//
//   // panel B: ユーザクリックで選択を切り替える
//   if (ImGui::Selectable(label, sel.IsSelected(node_id))) {
//       sel.SelectNode(node_id);   // 登録済み callback が一斉に呼ばれる
//   }
//
// 設計選択 (Pillar SceneInspector Phase 20):
//   ・**1 個の "選択" だけを持つ最小ハブ**: Unity Inspector のように
//     multi-select も将来は欲しいが、Phase 20 では「currently selected single
//     node」のみ。Multi-select は別 API (`SelectionSet`) で Phase 2+ に分離。
//   ・**callback は複数登録 (HotReloadWatcher と同形)**: (cb, user) ペアで
//     重複弾き、Unregister で 1 件除去。dispatch 順は登録順。
//   ・**from / to を渡す**: 単純な「to」だけだと、購読側で前回値を覚えて
//     diff を取る必要が出る。差分通知の典型形は (from, to) なのでハブ側で
//     渡す。`ClearSelection()` は `to = FNodeId{}` (invalid) として通知される。
//   ・**STL 不使用**: 登録 list は `acs::TArray<CallbackEntry>`。
//   ・**全 noexcept**: ACS 規約。エラーは null/重複弾きで安全 no-op。
//   ・**非コピー / 非ムーブ**: 内部 TArray<CallbackEntry> の所有を曖昧にしない。
//   ・**FGame / FSceneManager への依存なし**: FNodeId だけを扱うため、選択対象が
//     生きているか / どの Scene に属するかの検証は購読側責務。これで
//     editor の "選択は残るが対象は破棄済み" のケースも素直に表現できる。
//
// 範囲外 (Phase 2+ で):
//   ・Multi-select (Ctrl+クリックでの加算選択など)
//   ・Selection History (Back/Forward ボタン)
//   ・Asset / Component 選択 (現状は Node のみ)
//   ・hover (= pre-selection) 通知
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/NodeId.h"

namespace acs::game::inspector {

// ---- コールバック型 ------------------------------------------------------
// `from`: 変更前の選択 (未選択時は invalid)。
// `to`  : 変更後の選択 (ClearSelection 時は invalid)。
// `user`: RegisterCallback の第二引数で渡したコンテキストポインタ。
// 同じ selection を再 SelectNode した場合は callback を呼ばない (no-op)。
using SelectionChangeCallback = void (*)(void* user, FNodeId from, FNodeId to) noexcept;

// ---- FSelectionService — 選択 FNodeId の集中点 ----------------------------
class FSelectionService {
public:
    FSelectionService() noexcept = default;
    ~FSelectionService() noexcept = default;

    // 非コピー・非ムーブ (内部 callback 配列の所有権を曖昧にしない)
    FSelectionService(const FSelectionService&)            = delete;
    FSelectionService& operator=(const FSelectionService&) = delete;
    FSelectionService(FSelectionService&&)                 = delete;
    FSelectionService& operator=(FSelectionService&&)      = delete;

    // 初期化 (callback list / 現選択を空に戻す)。多重呼び出し可。
    void Init() noexcept;

    // 現選択を `id` に切り替える。値が変化した場合のみ callback を一斉発火。
    // invalid handle (= 既定構築 `FNodeId{}`) を渡すと ClearSelection と等価。
    void SelectNode(FNodeId id) noexcept;

    // 現選択を invalid に戻す。前選択が valid だった場合のみ callback 発火。
    void ClearSelection() noexcept;

    // 現選択を取得。未選択時は invalid handle (`IsValid() == false`)。
    FNodeId CurrentSelection() const noexcept;

    // 完全一致比較 (index + generation 一致時のみ true)。
    // invalid id を渡した場合、現選択が invalid なら true、それ以外なら false。
    bool IsSelected(FNodeId id) const noexcept;

    // (cb, user) ペアを登録。同一ペアの重複登録は no-op で弾く。
    // cb が null の場合は登録せず no-op。
    void RegisterCallback(SelectionChangeCallback cb, void* user) noexcept;

    // (cb, user) 完全一致 1 件を解除。該当なしは no-op。
    void UnregisterCallback(SelectionChangeCallback cb, void* user) noexcept;

    // 登録済みコールバック数。
    u32 CallbackCount() const noexcept;

    // 全 callback 登録を破棄、選択も invalid に戻す。
    // ClearSelection 相当の通知は **行わない** (= shutdown 時の一括破棄想定)。
    void ClearAll() noexcept;

private:
    // (cb, user) ペアを表す POD エントリ。重複弾き + 順序保持。
    struct CallbackEntry {
        SelectionChangeCallback cb   = nullptr;
        void*                   user = nullptr;
    };

    // 立ち上がり / 立ち下がり共通の通知ヘルパ。
    // `_current` の更新は呼び出し側で完了させてから呼ぶこと。
    void FireChange(FNodeId from, FNodeId to) const noexcept;

    FNodeId               _current;        // 現選択 (default = invalid)
    TArray<CallbackEntry> _callbacks;      // 登録 callback 群
};

} // namespace acs::game::inspector
