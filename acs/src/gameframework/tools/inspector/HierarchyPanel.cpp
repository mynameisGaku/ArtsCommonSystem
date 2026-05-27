// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — SceneInspector / FHierarchyPanel 実装 (Phase 20 editor 第二弾)
//
// 仕様の意図は FHierarchyPanel.h を参照。本ファイルでは:
//   ・root_node を起点に FNode2D ツリーを再帰描画 (ImGui::TreeNodeEx)
//   ・各ノードクリックで selection 切替 (FSelectionService 経由 or 内部 fallback)
//   ・右クリックで context menu (Delete / Duplicate / Reparent target 設定)
//   ・Drag & Drop で reparent (BeginDragDropSource / BeginDragDropTarget)
//   ・折りたたみ状態を FNodeId キーで永続化
// を実装する。すべて noexcept、STL 不使用、ImGui include は本 .cpp 限定。
#include "gameframework/tools/inspector/HierarchyPanel.h"

#include "gameframework/Node2D.h"
#include "gameframework/tools/inspector/SelectionService.h"

#include <imgui.h>

#include <cstdio>  // snprintf (ノードラベル整形)

namespace acs::game::inspector {

// ---- ローカルヘルパ ------------------------------------------------------
// 描画ラベル "Node #idx (id=N:gen)" を整形する。FNodeId が invalid なら
// "Node #idx (no-id)" の表記。これにより同名 (ラベル無し) のノードでも
// hierarchy 上で区別できる (Unity の "GameObject (1)" 命名相当)。
static void FormatNodeLabel(char* buf, usize buf_size,
                            const FNode2D& node, u32 sibling_index) noexcept {
    if (buf == nullptr || buf_size == 0) return;
    const FNodeId id = node.Id();
    if (id.IsValid()) {
        std::snprintf(buf, buf_size, "Node #%u (id=%u:%u)",
                      static_cast<unsigned>(sibling_index),
                      static_cast<unsigned>(id.Index()),
                      static_cast<unsigned>(id.Generation()));
    } else {
        std::snprintf(buf, buf_size, "Node #%u (no-id)",
                      static_cast<unsigned>(sibling_index));
    }
}

// =============================================================================
// Init / Shutdown
// =============================================================================
void FHierarchyPanel::Init() noexcept {
    m_CollapsedMap.Clear();
    m_SelectedId         = FNodeId{};
    m_SelectedNode       = nullptr;
    m_ReparentTarget     = nullptr;
    m_bCollapseAllPending = false;
    // FSelectionService は外部所有 (non-owning)、callback は外部 API なので
    // Init で触らない (= Set 状態を維持)。
}

void FHierarchyPanel::Shutdown() noexcept {
    m_CollapsedMap.Clear();
    m_SelectedId         = FNodeId{};
    m_SelectedNode       = nullptr;
    m_ReparentTarget     = nullptr;
    m_bCollapseAllPending = false;
    m_SelectionService   = nullptr;
    m_RightClickCb      = nullptr;
    m_RightClickUser    = nullptr;
}

// =============================================================================
// SetSelectionService / SelectedNodeId / SelectNode
// =============================================================================
void FHierarchyPanel::SetSelectionService(FSelectionService* svc) noexcept {
    m_SelectionService = svc;
    // FSelectionService 注入直後、internal cache を seam 側の現選択に同期して
    // おく (= 既に何か選択されていた場合に表示を合わせる)。FNode2D* は
    // FNodeId からは逆引きできないので nullptr に戻す (= 次の DrawUI 走査で
    // ヒットしたら再キャッシュする)。
    if (m_SelectionService != nullptr) {
        m_SelectedId   = m_SelectionService->CurrentSelection();
        m_SelectedNode = nullptr;
    }
}

FNodeId FHierarchyPanel::SelectedNodeId() const noexcept {
    if (m_SelectionService != nullptr) {
        return m_SelectionService->CurrentSelection();
    }
    return m_SelectedId;
}

void FHierarchyPanel::SelectNode(FNode2D* node) noexcept {
    m_SelectedNode = node;
    if (node != nullptr) {
        m_SelectedId = node->Id();
    } else {
        m_SelectedId = FNodeId{};
    }
    // FSelectionService がいれば反映 (callback 発火は seam 側責務)。
    if (m_SelectionService != nullptr) {
        if (node != nullptr) {
            m_SelectionService->SelectNode(node->Id());
        } else {
            m_SelectionService->ClearSelection();
        }
    }
}

// =============================================================================
// ExpandAll / CollapseAll
// =============================================================================
void FHierarchyPanel::ExpandAll() noexcept {
    // 折りたたみマップを空にする = 全 FNodeId が "default expanded" 扱い。
    m_CollapsedMap.Clear();
    m_bCollapseAllPending = false;
}

void FHierarchyPanel::CollapseAll() noexcept {
    // DrawUI で各ノード走査時に既存エントリを true に立てる。ここでは
    // 単にフラグを立てるだけ (ノードを 1 つも DrawUI 内で見ていない時点での
    // CollapseAll は意味が無く、DrawUI 内で適用するのが安全)。
    m_bCollapseAllPending = true;
}

// =============================================================================
// DeleteSelected
// =============================================================================
void FHierarchyPanel::DeleteSelected() noexcept {
    // selection の Node* は DrawUI 走査中にキャッシュされている前提
    // (FSelectionService 注入時も同様)。キャッシュが無ければ no-op。
    FNode2D* target = m_SelectedNode;
    if (target == nullptr) return;
    target->Destroy();  // フレーム境界で reap される
    // selection は明示解除 (次フレーム描画前に古い FNodeId を引きずらない)。
    m_SelectedNode = nullptr;
    m_SelectedId   = FNodeId{};
    if (m_SelectionService != nullptr) {
        m_SelectionService->ClearSelection();
    }
    // reparent target に同ノードが指定されていれば clear (dangling 回避)。
    if (m_ReparentTarget == target) {
        m_ReparentTarget = nullptr;
    }
}

// =============================================================================
// SetOnNodeRightClickCallback
// =============================================================================
void FHierarchyPanel::SetOnNodeRightClickCallback(NodeRightClickCallback cb,
                                                 void* user) noexcept {
    m_RightClickCb   = cb;
    m_RightClickUser = user;
}

// =============================================================================
// IsCollapsed / SetCollapsed (FNodeId → bool linear search)
// =============================================================================
bool FHierarchyPanel::IsCollapsed(FNodeId id) const noexcept {
    for (u32 i = 0; i < m_CollapsedMap.Size(); ++i) {
        if (m_CollapsedMap[i].id == id) return m_CollapsedMap[i].collapsed;
    }
    return false;  // エントリ無し = default expanded
}

void FHierarchyPanel::SetCollapsed(FNodeId id, bool c) noexcept {
    for (u32 i = 0; i < m_CollapsedMap.Size(); ++i) {
        if (m_CollapsedMap[i].id == id) {
            m_CollapsedMap[i].collapsed = c;
            return;
        }
    }
    // c == false (= expanded) かつ既存なし → no-op (default expanded を保つ)。
    // c == true (= collapsed) なら新規追加。
    if (c) {
        CollapsedEntry e {};
        e.id        = id;
        e.collapsed = true;
        m_CollapsedMap.PushBack(e);
    }
}

// =============================================================================
// DrawNodeRecursive — 1 ノードを描画し、子を再帰描画
// =============================================================================
// TreeNode の expand/collapse 状態:
//   ・最初の描画では `ImGui::SetNextItemOpen(open)` で `m_CollapsedMap` の
//     状態を反映する (= 起動直後は全展開)。
//   ・ユーザがクリックして変えた場合は `ImGui::IsItemToggledOpen()` を見て
//     map に書き戻す。
//   ・CollapseAll の遅延適用: フレーム最初のノード描画前にこの関数が呼ばれ、
//     `m_bCollapseAllPending` が立っていれば「現ノード以下を強制 close」する。
//
// 選択 / 右クリック / Drag & Drop:
//   ・Selectable behavior は TreeNode の `ImGuiTreeNodeFlags_Selected` で実現。
//     クリックされたら `SelectNode(&node)` を呼ぶ。
//   ・右クリックは `ImGui::BeginPopupContextItem()` で開く。Delete / Duplicate /
//     Reparent (Set Target / Reparent Here) + 外部 callback。
//   ・Drag source = この TreeNode、payload = &node。
//   ・Drag target = この TreeNode 行、accept 時に source node を target に
//     `Reparent` 要求する (cycle/self は FNode2D 側で弾かれる)。
// =============================================================================
void FHierarchyPanel::DrawNodeRecursive(FNode2D& node, u32 depth) noexcept {
    // depth 上限ガード (ACS の FNode2D は構造的に木構造で循環不能だが、防衛策)。
    if (depth >= 64u) {
        ImGui::TextDisabled("(depth limit reached)");
        return;
    }

    const FNodeId  id        = node.Id();
    const bool    has_child = node.ChildCount() > 0u;
    const bool    selected  = (id.IsValid() && id == SelectedNodeId());

    // selection が valid な場合、走査中にこの node を見つけた = キャッシュ更新
    if (selected) {
        m_SelectedNode = &node;
    }

    // CollapseAll の遅延適用: FNodeId が valid なら collapsed=true を立てる。
    if (m_bCollapseAllPending && id.IsValid()) {
        SetCollapsed(id, true);
    }

    // TreeNode flags
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected)   flags |= ImGuiTreeNodeFlags_Selected;
    if (!has_child) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    // expand 状態を m_CollapsedMap から強制反映。
    if (id.IsValid()) {
        const bool want_open = !IsCollapsed(id);
        ImGui::SetNextItemOpen(want_open, ImGuiCond_Always);
    } else {
        // FNodeId が無いノードは default 展開 (= 初期表示しっかり)。
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    }

    // ImGui ID は FNodeId.m_Packed を使う (兄弟同名でも衝突しない)。
    // FNodeId 無効ノードは pointer をフォールバック ID にする。
    if (id.IsValid()) {
        ImGui::PushID(static_cast<int>(id.m_Packed));
    } else {
        ImGui::PushID(static_cast<const void*>(&node));
    }

    char label[64];
    // sibling_index は parent から見た index だが、root では 0。簡易表示で十分。
    FormatNodeLabel(label, sizeof(label), node, depth);
    const bool open = ImGui::TreeNodeEx("##hier_tn", flags, "%s", label);

    // TreeNode toggle を検知して m_CollapsedMap に書き戻す。
    // (リーフは Leaf flag のため toggle は来ない = no-op で良い)
    //
    // ロジック:
    //   ・直前の `SetNextItemOpen(want_open)` で「今フレームの表示開閉状態」は
    //     want_open に強制された。
    //   ・ユーザが矢印クリックで toggle すると、表示状態が反転して
    //     新表示 = !want_open になる (IsItemToggledOpen が true)。
    //   ・次フレームの want_open を新表示に合わせるため、新 collapsed = !新表示
    //                                                             = want_open
    //   ・want_open == !IsCollapsed(id) (まだ書き戻し前なので OLD 値)。
    if (id.IsValid() && has_child && ImGui::IsItemToggledOpen()) {
        const bool prev_want_open = !IsCollapsed(id);
        SetCollapsed(id, prev_want_open);
    }

    // クリックで selection 切替。ItemClicked() は左クリック + 同フレーム検知。
    // 既選択を再クリックは no-op (FSelectionService 側で重複弾く)。
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)
        && !ImGui::IsItemToggledOpen()) {
        SelectNode(&node);
    }

    // ----- Drag source: 自分を payload に -----
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        FNode2D* self_ptr = &node;
        ImGui::SetDragDropPayload(kDragDropId, &self_ptr, sizeof(self_ptr));
        ImGui::TextUnformatted(label);
        ImGui::EndDragDropSource();
    }

    // ----- Drop target: 受け取った FNode2D* を自分の子に reparent -----
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(kDragDropId);
        if (payload != nullptr
            && payload->DataSize == static_cast<int>(sizeof(FNode2D*))) {
            FNode2D* src = nullptr;
            // 安全のため memcpy (payload->Data は void*、align 保証なし)。
            for (int b = 0; b < static_cast<int>(sizeof(FNode2D*)); ++b) {
                reinterpret_cast<unsigned char*>(&src)[b] =
                    static_cast<const unsigned char*>(payload->Data)[b];
            }
            if (src != nullptr && src != &node) {
                // FNode2D::Reparent は cycle / self / pending_destroy を内部で弾く。
                src->Reparent(node);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // ----- 右クリック context menu -----
    if (ImGui::BeginPopupContextItem("##hier_ctx")) {
        // selection をこのノードに切り替えてから menu 操作 (UX 安心感)。
        SelectNode(&node);

        if (ImGui::MenuItem("Delete")) {
            DeleteSelected();
            ImGui::EndPopup();
            // 削除後は children も無効な可能性が高いので、TreePop を整えて return。
            if (open && has_child) ImGui::TreePop();
            ImGui::PopID();
            return;
        }

        // Duplicate: FNode2D 側に組み込み Clone API が未だ無いため、ここでは
        // 「メニュー項目だけ提示し、実処理は外部 callback に委譲」する設計。
        // callback 未登録時は disabled 表示にしてユーザに気付かせる。
        const bool dup_enabled = (m_RightClickCb != nullptr);
        if (!dup_enabled) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Duplicate")) {
            if (m_RightClickCb != nullptr) {
                m_RightClickCb(m_RightClickUser, &node);
            }
        }
        if (!dup_enabled) ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::MenuItem("Set as Reparent Target")) {
            m_ReparentTarget = &node;
        }
        const bool can_reparent_here =
            (m_ReparentTarget != nullptr) && (m_ReparentTarget != &node);
        if (!can_reparent_here) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Reparent Here (move target under this)")) {
            // 「Set as Reparent Target」で覚えたノードを、今右クリックされた
            // node の子に移動する。
            if (m_ReparentTarget != nullptr) {
                m_ReparentTarget->Reparent(node);
                m_ReparentTarget = nullptr;
            }
        }
        if (!can_reparent_here) ImGui::EndDisabled();

        // ----- 外部 callback (追加メニュー項目を提供する hook) -----
        // ImGui の popup は「同 popup 内で 1 回開いている間は毎フレーム再描画
        // される」ため、callback は popup が開いている間に毎フレーム呼ばれる。
        // callback 側で ImGui::MenuItem("...") 等を追加する想定 (= 外部から
        // メニュー項目を増やす機構)。Duplicate の MenuItem も同じ callback を
        // ハンドラとして呼ぶため、callback は (item_added_per_frame) と
        // (duplicate_invoked) を内部 user 状態で区別する責務がある。
        if (m_RightClickCb != nullptr) {
            ImGui::Separator();
            m_RightClickCb(m_RightClickUser, &node);
        }

        ImGui::EndPopup();
    }

    // ----- 子の再帰描画 -----
    if (open && has_child) {
        for (u32 i = 0; i < node.ChildCount(); ++i) {
            FNode2D* c = node.Child(i);
            if (c != nullptr) {
                DrawNodeRecursive(*c, depth + 1u);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

// =============================================================================
// DrawUI — メイン ImGui window
// =============================================================================
// Begin("Scene Hierarchy") 1 window で完結。レイアウト:
//   ┌──────── "Scene Hierarchy" window ─────────┐
//   │ [Expand All] [Collapse All] [Delete Sel.] │
//   │ Reparent Target: <name> [Clear]           │
//   │ ─────────────────────────────────────── │
//   │ ▼ Root (id=...)                            │
//   │   ▼ Child 0                                │
//   │     ▶ Grandchild 0                         │
//   │   ▼ Child 1                                │
//   │     ...                                    │
//   └─────────────────────────────────────────────┘
// =============================================================================
void FHierarchyPanel::DrawUI() noexcept {
    // Phase 24: FEditorPanel 継承で no-param 化。SetRootNode で事前 set された
    // m_RootNode を root に描画。null なら "(no root set)" メッセージ表示。
    if (!ImGui::Begin("Scene Hierarchy")) {
        ImGui::End();
        return;
    }
    if (m_RootNode == nullptr) {
        ImGui::TextUnformatted("(no root node attached; call SetRootNode)");
        ImGui::End();
        return;
    }
    FNode2D& root_node = *m_RootNode;

    // ----- Toolbar -----
    if (ImGui::Button("Expand All")) {
        ExpandAll();
    }
    ImGui::SameLine();
    if (ImGui::Button("Collapse All")) {
        CollapseAll();
    }
    ImGui::SameLine();
    const bool has_sel = (m_SelectedNode != nullptr);
    if (!has_sel) ImGui::BeginDisabled();
    if (ImGui::Button("Delete Selected")) {
        DeleteSelected();
    }
    if (!has_sel) ImGui::EndDisabled();

    // Reparent target 表示行
    ImGui::Text("Reparent Target: ");
    ImGui::SameLine();
    if (m_ReparentTarget != nullptr) {
        const FNodeId tid = m_ReparentTarget->Id();
        if (tid.IsValid()) {
            ImGui::Text("id=%u:%u",
                        static_cast<unsigned>(tid.Index()),
                        static_cast<unsigned>(tid.Generation()));
        } else {
            ImGui::TextUnformatted("(no-id node)");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##rep")) {
            m_ReparentTarget = nullptr;
        }
    } else {
        ImGui::TextDisabled("(none)");
    }
    ImGui::Separator();

    // ----- Tree -----
    // ImGui の子 region として scroll 可能領域を持たせる。
    if (ImGui::BeginChild("##hier_tree", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        DrawNodeRecursive(root_node, 0u);
    }
    ImGui::EndChild();

    // CollapseAll の遅延適用は今フレームの再帰描画で全ノードに反映されたので、
    // フラグを下ろす。
    m_bCollapseAllPending = false;

    ImGui::End();
}

} // namespace acs::game::inspector
