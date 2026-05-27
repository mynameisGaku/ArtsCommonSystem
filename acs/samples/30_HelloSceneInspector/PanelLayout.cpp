// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — PanelLayout 実装。
#include "PanelLayout.h"

#include "gameframework/GameFramework.h"

namespace helloscene {

void PanelLayout::Init() noexcept {
    _seam.Init();
    _selection.Init();
    _hierarchy_panel.Init();
    _hierarchy_panel.SetSelectionService(&_selection);
    _inspector_panel.Init();
    _inspector_panel.SetSelectionService(&_selection);
    // FEditorToolbar の最小契約: Init() で内部状態を default に戻す
    // (Play/Pause/Step トグルは内部で持つ)。
    _toolbar.Init();
}

void PanelLayout::Shutdown() noexcept {
    // Provider 本体は Scene 側 (PlayerNode) が握っているので seam は
    // 登録解除のみ。panel は逆順に shutdown。
    _toolbar.Shutdown();
    _inspector_panel.Shutdown();
    _hierarchy_panel.Shutdown();
    _selection.ClearAll();
    _seam.ClearAll();
}

void PanelLayout::RegisterProvider(acs::game::IInspectableProvider* provider) noexcept {
    _seam.RegisterProvider(provider);
}

void PanelLayout::SelectInitial(acs::game::FNodeId id) noexcept {
    _selection.SelectNode(id);
}

void PanelLayout::DrawUI(acs::game::FGame& game, acs::game::FNode2D& root) noexcept {
    // FEditorPanel 基底に乗っているため、フレーム毎に依存を bind してから no-arg DrawUI。
    _toolbar.SetGame(&game);
    _toolbar.DrawUI();

    _hierarchy_panel.SetRootNode(&root);
    _hierarchy_panel.DrawUI();

    // selection は Hierarchy 側が FSelectionService に書き、Inspector が読む。
    _inspector_panel.SetInspectorSeam(&_seam);
    _inspector_panel.DrawUI();
}

} // namespace helloscene
