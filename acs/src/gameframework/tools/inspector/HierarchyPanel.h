// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — SceneInspector / FHierarchyPanel
//
// シーン (ANode ツリー) を ImGui ベースの hierarchy パネルで可視化 + 編集する
// エディタ用パネル。Unity の Hierarchy / Godot の SceneTree / UE の World Outliner
// 相当の役割で、retail ビルドからは #ifdef で消す前提。
//
// 機能:
//   ・root_node 配下を再帰 TreeNode で描画 ("Scene Hierarchy" タイトルの ImGui window)
//   ・各ノードクリックで選択 (FSelectionService 経由で共有、未注入時は内部で保持)
//   ・選択ノードは ImGuiTreeNodeFlags_Selected でハイライト
//   ・右クリックで context menu (Delete / Duplicate / Reparent target 設定)
//   ・Drag & Drop で reparent (drag source = drop target、両方とも ANode*)
//   ・ExpandAll / CollapseAll で一括展開・折りたたみ
//   ・DeleteSelected で `ANode::Destroy()` 呼出 (フレーム境界 reap 任せ)
//
// 連携:
//   ・**FSelectionService** (別エージェントが実装中) — selection 状態を他パネル
//     (FInspectorPanel 等) と共有する集中点。forward decl で受け、Set されていれば
//     そちら経由で selection を読み書きする。未注入時は本パネル内の `m_SelectedId`
//     を使う (= スタンドアロン動作可能)。
//   ・**FInspectorPanel** (別エージェントが実装中) — 選択中 ANode の property を
//     編集するパネル。本パネルとは selection 経由でしか連携しない (Hierarchy は
//     Inspector を知らない)。
//   ・**Right-click callback** — Delete / Duplicate / Reparent 以外のメニュー
//     項目 (例: "Create Child", "Save as Prefab") は外部で実装。callback で
//     "今右クリックされた ANode*" を渡し、外部側で context menu 続きを描画。
//
// 設計選択:
//   ・**non-copy / non-move**: 内部 TArray<FCollapsedEntry> + raw pointer の所有を
//     曖昧にしない。FInspectorSeam / FParticleEditorPanel と同じ規約。
//   ・**全 noexcept**: ACS 規約。エラーは index out-of-range / nullptr 等を
//     no-op で表現。
//   ・**STL 不使用**: 折りたたみ状態は `TArray<FCollapsedEntry>` の linear search
//     (= FNodeId → bool マップ)。ノード数が 100k 級でなければ十分速い。binary
//     search / hash table は必要なら導入。
//   ・**ImGui ヘッダは .cpp 側のみ**: header からは imgui 依存を漏らさない
//     (`ParticleEditorPanel.h` と同じ方針)。
//   ・**FSelectionService は forward decl**: header からは依存を切り、.cpp 側で
//     のみ include する (循環や同時編集事故の回避)。実 API は
//     `FSelectionService::SelectNode(FNodeId) / CurrentSelection() const`。
//   ・**Reparent は `ANode::Reparent` の deferred 呼出**: cycle 検出 + フレーム
//     境界での実適用は ANode 側が責任を持つ (cycle ガード `IsAncestorOf` 済)。
//   ・**Drag & Drop payload は `ANode*` 直渡し**: ImGui 慣例的に `SetDragDropPayload`
//     に payload struct を渡すが、scene 内では ANode の生存期間が selection を
//     超えることは無いので、ポインタ直渡しで安全。識別子は `"HIER_NODE_PTR"`。
//
// 範囲外:
//   ・AComponent の子要素表示 (= property は FInspectorPanel 担当)
//   ・Undo / Redo
//   ・複数選択 (現状は単一選択のみ)
//   ・Search / filter
//   ・rename in-place
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/NodeId.h"

namespace acs::game {
class ANode;
} // namespace acs::game

#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::inspector {

/** selection 集中点 (別レイヤで実装、forward decl のみ受ける)。 */
class FSelectionService;

/**
 * ImGui ベースで ANode 階層ツリーを可視化・編集するエディタパネル。
 *
 * @details
 * Unity の Hierarchy / Godot の SceneTree / UE の World Outliner 相当。
 * `Begin("Scene Hierarchy")` の 1 window で root_node 配下を再帰 TreeNode 描画し、
 * クリック選択・右クリック context menu (Delete / Duplicate / Reparent)・Drag & Drop
 * による reparent・ExpandAll/CollapseAll をサポートする。selection は注入された
 * FSelectionService 経由で他パネルと共有し、未注入時は内部 m_SelectedId を使う。
 * editor_core::FEditorPanel を継承し、全 API は noexcept・STL 不使用・ImGui 依存は
 * .cpp に閉じる。
 */
class FHierarchyPanel : public ::acs::game::editor_core::FEditorPanel {
public:
    /**
     * 右クリック context menu の追加項目を外部へ委譲する関数ポインタ型。
     *
     * @details
     * 標準の "Delete" / "Duplicate" / "Reparent" は本パネル内で描画するため、この
     * callback は追加項目 ("Create Child" / "Save as Prefab" 等) のみ担当する想定。
     * @param user SetOnNodeRightClickCallback の第二引数で渡したユーザポインタ。
     * @param node 今右クリックされた対象ノード。
     */
    using NodeRightClickCallback = void (*)(void* user, class ANode* node) noexcept;

    /** 空状態で構築する (root / selection なし)。 */
    FHierarchyPanel() noexcept = default;

    /** 破棄する (外部所有の root / FSelectionService / callback は解放しない)。 */
    ~FHierarchyPanel() noexcept = default;

    /** コピー禁止 (内部 TArray + raw pointer の所有を曖昧にしないため)。 */
    FHierarchyPanel(const FHierarchyPanel&)            = delete;

    /** コピー代入も禁止。 */
    FHierarchyPanel& operator=(const FHierarchyPanel&) = delete;

    /** ムーブ禁止 (同上の所有規約)。 */
    FHierarchyPanel(FHierarchyPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FHierarchyPanel& operator=(FHierarchyPanel&&)      = delete;

    /** 折りたたみマップを空にし selection を解除して初期化する (多重 Init 可)。 */
    void Init() noexcept;

    /** 折りたたみマップを解放し callback を解除して後始末する (多重 Shutdown 可)。 */
    void Shutdown() noexcept;

    /**
     * 再帰描画の起点となる root ノードを設定する。
     *
     * @param root root として描画する ANode (nullptr で未設定)。
     */
    void SetRootNode(class ANode* root) noexcept { m_RootNode = root; }

    /**
     * 設定済みの root ノードを返す。
     *
     * @return root ノード (未設定なら nullptr)。
     */
    class ANode* RootNode() const noexcept { return m_RootNode; }

    /**
     * このパネルのウィンドウタイトルを返す。
     *
     * @return "Scene Hierarchy"。
     */
    const char* Title() const noexcept override { return "Scene Hierarchy"; }

    /**
     * メイン ImGui window を描画する。
     *
     * @details
     * `Begin("Scene Hierarchy")` の 1 window で完結し、SetRootNode で事前設定された
     * root 配下を再帰 TreeNode 描画する。root 自体も最上位 TreeNode として表示される
     * (= ユーザは root をクリックして scene 全体を選択できる)。root 未設定時は案内
     * メッセージのみ表示する。
     */
    void DrawUI() noexcept override;

    /**
     * FSelectionService を注入する。
     *
     * @details nullptr を渡すと内部 selection モード (m_SelectedId) に戻る。
     * @param svc 共有する selection サービス (non-owning)。
     */
    void SetSelectionService(class FSelectionService* svc) noexcept;

    /**
     * 現在の選択ノードの FNodeId を返す。
     *
     * @details FSelectionService 注入時はそちら経由、未注入なら内部 m_SelectedId。
     * @return 選択中ノードの FNodeId (未選択は packed==0 の無効値)。
     */
    FNodeId SelectedNodeId() const noexcept;

    /**
     * 選択を `node` に切り替える。
     *
     * @details FSelectionService 注入時はそちらにも反映する。
     * @param node 選択するノード (nullptr で選択解除)。
     */
    void SelectNode(class ANode* node) noexcept;

    /**
     * 全 TreeNode を展開状態にする。
     *
     * @details
     * ImGui の TreeNode は default が「展開」なので、折りたたみマップにエントリが
     * 無い = 展開、エントリが true なら「折りたたみ」を表す。よって本関数はマップを
     * 空にするだけでよい。
     */
    void ExpandAll() noexcept;

    /**
     * 全 TreeNode を折りたたむ。
     *
     * @details
     * 各ノードの折りたたみ反映は次回 DrawUI の走査時に行うため、ここでは
     * m_bCollapseAllPending フラグを立てるだけにする。
     */
    void CollapseAll() noexcept;

    /**
     * 選択中ノードに ANode::Destroy() を呼ぶ。
     *
     * @details
     * pending_destroy がマークされ、次フレームの ResolveStructuralChanges で実
     * destroy される。selection は解除する。選択中ノードを解決できない場合は no-op。
     */
    void DeleteSelected() noexcept;

    /**
     * 右クリック追加項目の callback を登録する。
     *
     * @param cb 呼ぶ callback (nullptr で解除)。
     * @param user callback 第一引数として戻すユーザポインタ。
     */
    void SetOnNodeRightClickCallback(NodeRightClickCallback cb, void* user) noexcept;

private:
    /**
     * 折りたたみ状態 1 件分のエントリ。
     *
     * @details TArray<FCollapsedEntry> を FNodeId で線形探索し「FNodeId → bool collapsed」マップとして使う。
     */
    struct FCollapsedEntry {
        /** 対象ノードの FNodeId。 */
        FNodeId id      {};

        /** 折りたたみ済みなら true。 */
        bool   collapsed = false;
    };

    /**
     * `node` を 1 つ描画し、子を再帰描画する。
     *
     * @details
     * TreeNodeEx で開閉状態を m_CollapsedMap に反映・書き戻しつつ、クリックで選択切替、
     * 右クリックで context menu、Drag & Drop で reparent 要求を処理する。深さ 64 で
     * 防衛的にガードする。
     * @param node 描画する対象ノード。
     * @param depth 再帰の深さ (上限ガードおよび簡易ラベル表示に使う)。
     */
    void DrawNodeRecursive(class ANode& node, u32 depth) noexcept;

    /**
     * 指定 FNodeId が折りたたみ済みかを返す。
     *
     * @param id 調べるノードの FNodeId。
     * @return 折りたたみ済みなら true (エントリ無しは展開扱いで false)。
     */
    bool IsCollapsed(FNodeId id) const noexcept;

    /**
     * 指定 FNodeId の折りたたみ状態を上書き保存する。
     *
     * @details c == false かつ既存エントリ無しなら no-op (default expanded を保つ)。
     * @param id 対象ノードの FNodeId。
     * @param c true で折りたたみ、false で展開。
     */
    void SetCollapsed(FNodeId id, bool c) noexcept;

    /** Drag & Drop payload の識別子文字列 (ImGui 仕様: 32 文字以内)。 */
    static constexpr const char* kDragDropId = "HIER_NODE_PTR";

    /** 再帰描画の起点となる root ノード (SetRootNode で設定)。 */
    class ANode*             m_RootNode          = nullptr;

    /** FNodeId → 折りたたみ状態のマップ (線形探索)。 */
    TArray<FCollapsedEntry>     m_CollapsedMap      {};

    /** 共有 selection サービス (non-owning、未注入なら nullptr)。 */
    FSelectionService*         m_SelectionService  = nullptr;

    /** FSelectionService 未注入時のフォールバック selection。 */
    FNodeId                    m_SelectedId        {};

    /** 選択中ノードの生ポインタ (Delete / Duplicate 用、DrawUI 走査で更新)。 */
    class ANode*             m_SelectedNode      = nullptr;

    /**
     * 右クリック context menu の Reparent target 一時保管。
     *
     * @details 1 段目で "Set as Reparent Target" で set し、2 段目に別ノード上で "Reparent Here" で Reparent を呼ぶ。
     */
    class ANode*             m_ReparentTarget    = nullptr;

    /** CollapseAll の遅延適用フラグ。 */
    bool                      m_bCollapseAllPending = false;

    /** 右クリック追加項目の callback (未登録なら nullptr)。 */
    NodeRightClickCallback    m_RightClickCb     = nullptr;

    /** callback に渡すユーザポインタ。 */
    void*                     m_RightClickUser   = nullptr;
};

} // namespace acs::game::inspector
