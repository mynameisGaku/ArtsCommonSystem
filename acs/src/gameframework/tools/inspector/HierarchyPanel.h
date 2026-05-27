// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — SceneInspector / FHierarchyPanel (Phase 20 editor 第二弾)
//
// シーン (FNode2D ツリー) を ImGui ベースの hierarchy パネルで可視化 + 編集する
// エディタ用パネル。Unity の Hierarchy / Godot の SceneTree / UE の World Outliner
// 相当の役割で、retail ビルドからは #ifdef で消す前提。
//
// 機能:
//   ・root_node 配下を再帰 TreeNode で描画 ("Scene Hierarchy" タイトルの ImGui window)
//   ・各ノードクリックで選択 (FSelectionService 経由で共有、未注入時は内部で保持)
//   ・選択ノードは ImGuiTreeNodeFlags_Selected でハイライト
//   ・右クリックで context menu (Delete / Duplicate / Reparent target 設定)
//   ・Drag & Drop で reparent (drag source = drop target、両方とも FNode2D*)
//   ・ExpandAll / CollapseAll で一括展開・折りたたみ
//   ・DeleteSelected で `FNode2D::Destroy()` 呼出 (フレーム境界 reap 任せ)
//
// 連携:
//   ・**FSelectionService** (別エージェントが実装中) — selection 状態を他パネル
//     (FInspectorPanel 等) と共有する集中点。forward decl で受け、Set されていれば
//     そちら経由で selection を読み書きする。未注入時は本パネル内の `m_SelectedId`
//     を使う (= スタンドアロン動作可能)。
//   ・**FInspectorPanel** (別エージェントが実装中) — 選択中 FNode2D の property を
//     編集するパネル。本パネルとは selection 経由でしか連携しない (Hierarchy は
//     Inspector を知らない)。
//   ・**Right-click callback** — Delete / Duplicate / Reparent 以外のメニュー
//     項目 (例: "Create Child", "Save as Prefab") は外部で実装。callback で
//     "今右クリックされた FNode2D*" を渡し、外部側で context menu 続きを描画。
//
// 設計選択 (Phase 20):
//   ・**non-copy / non-move**: 内部 TArray<CollapsedEntry> + raw pointer の所有を
//     曖昧にしない。FInspectorSeam / FParticleEditorPanel と同じ規約。
//   ・**全 noexcept**: ACS 規約。エラーは index out-of-range / nullptr 等を
//     no-op で表現。
//   ・**STL 不使用**: 折りたたみ状態は `TArray<CollapsedEntry>` の linear search
//     (= FNodeId → bool マップ)。ノード数が 100k 級でなければ十分速い。binary
//     search / hash table は Phase 20+ で必要なら導入。
//   ・**ImGui ヘッダは .cpp 側のみ**: header からは imgui 依存を漏らさない
//     (`FParticleEditorPanel.h` と同じ方針)。
//   ・**FSelectionService は forward decl**: header からは依存を切り、.cpp 側で
//     のみ include する (循環や同時編集事故の回避)。実 API は
//     `FSelectionService::SelectNode(FNodeId) / CurrentSelection() const`。
//   ・**Reparent は `FNode2D::Reparent` の deferred 呼出**: cycle 検出 + フレーム
//     境界での実適用は FNode2D 側が責任を持つ (cycle ガード `IsAncestorOf` 済)。
//   ・**Drag & Drop payload は `FNode2D*` 直渡し**: ImGui 慣例的に `SetDragDropPayload`
//     に payload struct を渡すが、scene 内では FNode2D の生存期間が selection を
//     超えることは無いので、ポインタ直渡しで安全。識別子は `"HIER_NODE_PTR"`。
//
// 範囲外 (Phase 20+ / 別エージェント):
//   ・FComponent2D の子要素表示 (= property は FInspectorPanel 担当)
//   ・Undo / Redo
//   ・複数選択 (Phase 20 は単一選択のみ)
//   ・Search / filter
//   ・rename in-place
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/NodeId.h"

namespace acs::game {
class FNode2D;
} // namespace acs::game

#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::inspector {

// 別エージェントが作成中の selection 集中点。forward decl のみ受ける。
class FSelectionService;

// ---------------------------------------------------------------------------
// FHierarchyPanel — ImGui ベースの FNode2D 階層ツリー
// (Phase 24: editor_core::FEditorPanel 継承)
// ---------------------------------------------------------------------------
class FHierarchyPanel : public ::acs::game::editor_core::FEditorPanel {
public:
    // 右クリック context menu の追加項目を外部に委譲するためのコールバック。
    // 標準の "Delete" / "Duplicate" / "Reparent" は本パネル内で描画するため、
    // この callback は **追加** 項目 ("Create Child" / "Save as Prefab" 等) のみ
    // 担当する想定。`user` は SetOnNodeRightClickCallback の第二引数。
    using NodeRightClickCallback = void (*)(void* user, class FNode2D* node) noexcept;

    FHierarchyPanel() noexcept = default;
    ~FHierarchyPanel() noexcept = default;

    // 非コピー・非ムーブ: 内部 TArray<CollapsedEntry> + raw pointer の所有を
    // 曖昧にしない。
    FHierarchyPanel(const FHierarchyPanel&)            = delete;
    FHierarchyPanel& operator=(const FHierarchyPanel&) = delete;
    FHierarchyPanel(FHierarchyPanel&&)                 = delete;
    FHierarchyPanel& operator=(FHierarchyPanel&&)      = delete;

    // 初期化: 折りたたみマップを空に、selection を解除する。多重 Init 可。
    void Init() noexcept;

    // 後片付け: 折りたたみマップを解放、callback を解除する。多重 Shutdown 可。
    void Shutdown() noexcept;

    // メイン ImGui window 描画。`Begin("Scene Hierarchy")` 1 window で完結。
    // root_node 配下を再帰 TreeNode で描画する。root 自体も最上位 TreeNode と
    // して表示される (= ユーザは root をクリックして scene 全体を選択できる)。
    // Phase 24: FEditorPanel 継承で no-param DrawUI 化。root node は SetRootNode で
    // 事前 set する。
    void SetRootNode(class FNode2D* root) noexcept { m_RootNode = root; }
    class FNode2D* RootNode() const noexcept { return m_RootNode; }

    // FEditorPanel override
    const char* Title() const noexcept override { return "Scene Hierarchy"; }
    void DrawUI() noexcept override;

    // FSelectionService を注入。nullptr で内部 selection モードに戻せる。
    void SetSelectionService(class FSelectionService* svc) noexcept;

    // 現選択ノードの FNodeId。FSelectionService が注入されていればそちら経由、
    // そうでなければ内部 `m_SelectedId` を返す。未選択は `FNodeId{}` (packed==0)。
    FNodeId SelectedNodeId() const noexcept;

    // 選択を `node` に切替。nullptr で選択解除。FSelectionService 注入時は
    // そちらにも反映する。
    void SelectNode(class FNode2D* node) noexcept;

    // 全 TreeNode を展開状態にする (= 折りたたみマップを空にする)。
    // ImGui の TreeNode は default が「展開」なので、map にエントリが無い =
    // 展開、エントリが true なら「折りたたみ」を表す。よって ExpandAll は
    // map を空にすれば良い。
    void ExpandAll() noexcept;

    // 全 TreeNode を折りたたむ。次回 DrawUI で各ノード描画時に
    // 既存 FNodeId エントリを true に立てる必要があるため、ここでは
    // `m_bCollapseAllPending` フラグだけ立てて DrawUI で適用する。
    void CollapseAll() noexcept;

    // 選択中ノードに `FNode2D::Destroy()` を呼ぶ。pending_destroy がマークされ、
    // 次フレームの ResolveStructuralChanges で実 destroy される。
    // 未選択 / FSelectionService 経由でも該当ノードが取れない場合は no-op。
    // selection は解除される。
    void DeleteSelected() noexcept;

    // 右クリック追加項目 callback を登録。nullptr で解除可。
    void SetOnNodeRightClickCallback(NodeRightClickCallback cb, void* user) noexcept;

private:
    // 折りたたみ状態 1 エントリ。TArray<CollapsedEntry> を FNodeId で linear search
    // することで「FNodeId → bool collapsed」マップを実現する。
    struct CollapsedEntry {
        FNodeId id      {};
        bool   collapsed = false;
    };

    // root_node 配下を再帰描画。`depth` は将来的なインデント / 上限ガード用。
    void DrawNodeRecursive(class FNode2D& node, u32 depth) noexcept;

    // FNodeId → collapsed のルックアップ。エントリ無しは false (= 展開) 扱い。
    bool IsCollapsed(FNodeId id) const noexcept;

    // FNodeId に対する collapsed 状態を上書き保存 (エントリ無ければ追加)。
    // c == false かつ既存エントリ無しなら no-op (= default expanded を保つ)。
    void SetCollapsed(FNodeId id, bool c) noexcept;

    // drag drop payload の識別子文字列 (ImGui 仕様: 32 文字以内)。
    static constexpr const char* kDragDropId = "HIER_NODE_PTR";

    // Phase 24: 事前 set される root node (DrawUI で再帰描画の起点)
    class FNode2D*             m_RootNode          = nullptr;

    TArray<CollapsedEntry>     m_CollapsedMap      {};
    FSelectionService*         m_SelectionService  = nullptr;
    // FSelectionService 未注入時のフォールバック selection。
    FNodeId                    m_SelectedId        {};
    // 内部 selection の生ポインタ (Delete / Duplicate 用)。FSelectionService 注入
    // 時もキャッシュしておく (DrawUI で更新)。
    class FNode2D*             m_SelectedNode      = nullptr;
    // 右クリック context menu の Reparent target 設定 step 用の一時保管。
    // 1 段目で "Set as Reparent Target" を押すと set、2 段目に別ノード上で
    // "Reparent here" でこれを使って `Reparent` を呼ぶ。
    class FNode2D*             m_ReparentTarget    = nullptr;
    // CollapseAll の遅延適用フラグ。
    bool                      m_bCollapseAllPending = false;
    NodeRightClickCallback    m_RightClickCb     = nullptr;
    void*                     m_RightClickUser   = nullptr;
};

} // namespace acs::game::inspector
