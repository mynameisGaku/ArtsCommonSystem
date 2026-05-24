// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — SceneInspector / HierarchyPanel (Phase 20 editor 第二弾)
//
// シーン (Node2D ツリー) を ImGui ベースの hierarchy パネルで可視化 + 編集する
// エディタ用パネル。Unity の Hierarchy / Godot の SceneTree / UE の World Outliner
// 相当の役割で、retail ビルドからは #ifdef で消す前提。
//
// 機能:
//   ・root_node 配下を再帰 TreeNode で描画 ("Scene Hierarchy" タイトルの ImGui window)
//   ・各ノードクリックで選択 (SelectionService 経由で共有、未注入時は内部で保持)
//   ・選択ノードは ImGuiTreeNodeFlags_Selected でハイライト
//   ・右クリックで context menu (Delete / Duplicate / Reparent target 設定)
//   ・Drag & Drop で reparent (drag source = drop target、両方とも Node2D*)
//   ・ExpandAll / CollapseAll で一括展開・折りたたみ
//   ・DeleteSelected で `Node2D::Destroy()` 呼出 (フレーム境界 reap 任せ)
//
// 連携:
//   ・**SelectionService** (別エージェントが実装中) — selection 状態を他パネル
//     (InspectorPanel 等) と共有する集中点。forward decl で受け、Set されていれば
//     そちら経由で selection を読み書きする。未注入時は本パネル内の `_selected_id`
//     を使う (= スタンドアロン動作可能)。
//   ・**InspectorPanel** (別エージェントが実装中) — 選択中 Node2D の property を
//     編集するパネル。本パネルとは selection 経由でしか連携しない (Hierarchy は
//     Inspector を知らない)。
//   ・**Right-click callback** — Delete / Duplicate / Reparent 以外のメニュー
//     項目 (例: "Create Child", "Save as Prefab") は外部で実装。callback で
//     "今右クリックされた Node2D*" を渡し、外部側で context menu 続きを描画。
//
// 設計選択 (Phase 20):
//   ・**non-copy / non-move**: 内部 Array<CollapsedEntry> + raw pointer の所有を
//     曖昧にしない。InspectorSeam / ParticleEditorPanel と同じ規約。
//   ・**全 noexcept**: ACS 規約。エラーは index out-of-range / nullptr 等を
//     no-op で表現。
//   ・**STL 不使用**: 折りたたみ状態は `Array<CollapsedEntry>` の linear search
//     (= NodeId → bool マップ)。ノード数が 100k 級でなければ十分速い。binary
//     search / hash table は Phase 20+ で必要なら導入。
//   ・**ImGui ヘッダは .cpp 側のみ**: header からは imgui 依存を漏らさない
//     (`ParticleEditorPanel.h` と同じ方針)。
//   ・**SelectionService は forward decl**: header からは依存を切り、.cpp 側で
//     のみ include する (循環や同時編集事故の回避)。実 API は
//     `SelectionService::SelectNode(NodeId) / CurrentSelection() const`。
//   ・**Reparent は `Node2D::Reparent` の deferred 呼出**: cycle 検出 + フレーム
//     境界での実適用は Node2D 側が責任を持つ (cycle ガード `IsAncestorOf` 済)。
//   ・**Drag & Drop payload は `Node2D*` 直渡し**: ImGui 慣例的に `SetDragDropPayload`
//     に payload struct を渡すが、scene 内では Node2D の生存期間が selection を
//     超えることは無いので、ポインタ直渡しで安全。識別子は `"HIER_NODE_PTR"`。
//
// 範囲外 (Phase 20+ / 別エージェント):
//   ・Component2D の子要素表示 (= property は InspectorPanel 担当)
//   ・Undo / Redo
//   ・複数選択 (Phase 20 は単一選択のみ)
//   ・Search / filter
//   ・rename in-place
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/NodeId.h"

namespace acs::game {
class Node2D;
} // namespace acs::game

#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::inspector {

// 別エージェントが作成中の selection 集中点。forward decl のみ受ける。
class SelectionService;

// ---------------------------------------------------------------------------
// HierarchyPanel — ImGui ベースの Node2D 階層ツリー
// (Phase 24: editor_core::EditorPanel 継承)
// ---------------------------------------------------------------------------
class HierarchyPanel : public ::acs::game::editor_core::EditorPanel {
public:
    // 右クリック context menu の追加項目を外部に委譲するためのコールバック。
    // 標準の "Delete" / "Duplicate" / "Reparent" は本パネル内で描画するため、
    // この callback は **追加** 項目 ("Create Child" / "Save as Prefab" 等) のみ
    // 担当する想定。`user` は SetOnNodeRightClickCallback の第二引数。
    using NodeRightClickCallback = void (*)(void* user, class Node2D* node) noexcept;

    HierarchyPanel() noexcept = default;
    ~HierarchyPanel() noexcept = default;

    // 非コピー・非ムーブ: 内部 Array<CollapsedEntry> + raw pointer の所有を
    // 曖昧にしない。
    HierarchyPanel(const HierarchyPanel&)            = delete;
    HierarchyPanel& operator=(const HierarchyPanel&) = delete;
    HierarchyPanel(HierarchyPanel&&)                 = delete;
    HierarchyPanel& operator=(HierarchyPanel&&)      = delete;

    // 初期化: 折りたたみマップを空に、selection を解除する。多重 Init 可。
    void Init() noexcept;

    // 後片付け: 折りたたみマップを解放、callback を解除する。多重 Shutdown 可。
    void Shutdown() noexcept;

    // メイン ImGui window 描画。`Begin("Scene Hierarchy")` 1 window で完結。
    // root_node 配下を再帰 TreeNode で描画する。root 自体も最上位 TreeNode と
    // して表示される (= ユーザは root をクリックして scene 全体を選択できる)。
    // Phase 24: EditorPanel 継承で no-param DrawUI 化。root node は SetRootNode で
    // 事前 set する。
    void SetRootNode(class Node2D* root) noexcept { _root_node = root; }
    class Node2D* RootNode() const noexcept { return _root_node; }

    // EditorPanel override
    const char* Title() const noexcept override { return "Scene Hierarchy"; }
    void DrawUI() noexcept override;

    // SelectionService を注入。nullptr で内部 selection モードに戻せる。
    void SetSelectionService(class SelectionService* svc) noexcept;

    // 現選択ノードの NodeId。SelectionService が注入されていればそちら経由、
    // そうでなければ内部 `_selected_id` を返す。未選択は `NodeId{}` (packed==0)。
    NodeId SelectedNodeId() const noexcept;

    // 選択を `node` に切替。nullptr で選択解除。SelectionService 注入時は
    // そちらにも反映する。
    void SelectNode(class Node2D* node) noexcept;

    // 全 TreeNode を展開状態にする (= 折りたたみマップを空にする)。
    // ImGui の TreeNode は default が「展開」なので、map にエントリが無い =
    // 展開、エントリが true なら「折りたたみ」を表す。よって ExpandAll は
    // map を空にすれば良い。
    void ExpandAll() noexcept;

    // 全 TreeNode を折りたたむ。次回 DrawUI で各ノード描画時に
    // 既存 NodeId エントリを true に立てる必要があるため、ここでは
    // `_collapse_all_pending` フラグだけ立てて DrawUI で適用する。
    void CollapseAll() noexcept;

    // 選択中ノードに `Node2D::Destroy()` を呼ぶ。pending_destroy がマークされ、
    // 次フレームの ResolveStructuralChanges で実 destroy される。
    // 未選択 / SelectionService 経由でも該当ノードが取れない場合は no-op。
    // selection は解除される。
    void DeleteSelected() noexcept;

    // 右クリック追加項目 callback を登録。nullptr で解除可。
    void SetOnNodeRightClickCallback(NodeRightClickCallback cb, void* user) noexcept;

private:
    // 折りたたみ状態 1 エントリ。Array<CollapsedEntry> を NodeId で linear search
    // することで「NodeId → bool collapsed」マップを実現する。
    struct CollapsedEntry {
        NodeId id      {};
        bool   collapsed = false;
    };

    // root_node 配下を再帰描画。`depth` は将来的なインデント / 上限ガード用。
    void DrawNodeRecursive(class Node2D& node, u32 depth) noexcept;

    // NodeId → collapsed のルックアップ。エントリ無しは false (= 展開) 扱い。
    bool IsCollapsed(NodeId id) const noexcept;

    // NodeId に対する collapsed 状態を上書き保存 (エントリ無ければ追加)。
    // c == false かつ既存エントリ無しなら no-op (= default expanded を保つ)。
    void SetCollapsed(NodeId id, bool c) noexcept;

    // drag drop payload の識別子文字列 (ImGui 仕様: 32 文字以内)。
    static constexpr const char* kDragDropId = "HIER_NODE_PTR";

    // Phase 24: 事前 set される root node (DrawUI で再帰描画の起点)
    class Node2D*             _root_node          = nullptr;

    Array<CollapsedEntry>     _collapsed_map      {};
    SelectionService*         _selection_service  = nullptr;
    // SelectionService 未注入時のフォールバック selection。
    NodeId                    _selected_id        {};
    // 内部 selection の生ポインタ (Delete / Duplicate 用)。SelectionService 注入
    // 時もキャッシュしておく (DrawUI で更新)。
    class Node2D*             _selected_node      = nullptr;
    // 右クリック context menu の Reparent target 設定 step 用の一時保管。
    // 1 段目で "Set as Reparent Target" を押すと set、2 段目に別ノード上で
    // "Reparent here" でこれを使って `Reparent` を呼ぶ。
    class Node2D*             _reparent_target    = nullptr;
    // CollapseAll の遅延適用フラグ。
    bool                      _collapse_all_pending = false;
    NodeRightClickCallback    _right_click_cb     = nullptr;
    void*                     _right_click_user   = nullptr;
};

} // namespace acs::game::inspector
