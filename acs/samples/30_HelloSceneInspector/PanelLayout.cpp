// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — FPanelLayout 実装。
#include "PanelLayout.h"

#include "gameframework/GameFramework.h"

namespace helloscene {

void FPanelLayout::Init() noexcept {
    m_Seam.Init();
    m_Selection.Init();
    m_HierarchyPanel.Init();
    m_HierarchyPanel.SetSelectionService(&m_Selection);
    m_InspectorPanel.Init();
    m_InspectorPanel.SetSelectionService(&m_Selection);
    // FEditorToolbar の最小契約: Init() で内部状態を default に戻す
    // (Play/Pause/Step トグルは内部で持つ)。
    m_Toolbar.Init();
}

void FPanelLayout::Shutdown() noexcept {
    // Provider 本体は Scene 側 (APlayerNode) が握っているので seam は
    // 登録解除のみ。panel は逆順に shutdown。
    m_Toolbar.Shutdown();
    m_InspectorPanel.Shutdown();
    m_HierarchyPanel.Shutdown();
    m_Selection.ClearAll();
    m_Seam.ClearAll();
}

void FPanelLayout::RegisterProvider(acs::game::FNodeId node_id,
                                   acs::game::IInspectableProvider* provider) noexcept {
    // node_id をキーに登録 (= Inspector が選択 FNodeId から逆引きできるように)。
    m_Seam.RegisterProviderForNode(node_id, provider);
}

void FPanelLayout::SelectInitial(acs::game::FNodeId id) noexcept {
    m_Selection.SelectNode(id);
}

void FPanelLayout::DrawUI(acs::game::FGame& game, acs::game::ANode& root) noexcept {
    // FEditorPanel 基底に乗っているため、フレーム毎に依存を bind してから no-arg DrawUI。
    m_Toolbar.SetGame(&game);
    m_Toolbar.DrawUI();

    m_HierarchyPanel.SetRootNode(&root);
    m_HierarchyPanel.DrawUI();

    // selection は Hierarchy 側が FSelectionService に書き、Inspector が読む。
    m_InspectorPanel.SetInspectorSeam(&m_Seam);
    m_InspectorPanel.DrawUI();
}

} // namespace helloscene
