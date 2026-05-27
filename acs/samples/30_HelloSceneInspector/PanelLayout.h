// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — Inspector 系 4 panel + Selection + Seam の束ね役。
//
// Scene 本体から panel の配線責任を切り出すための薄いコンテナ。
// 1) Init で 4 panel + FSelectionService + FInspectorSeam を起動 + 配線
// 2) Shutdown で逆順 teardown
// 3) DrawUI で 1 フレーム分の描画 (root / game / seam は呼び出し側が供給)
//
// 4 panel 間の選択共有は FSelectionService が source of truth。Hierarchy が
// 選択を更新し、Inspector が読む。
#pragma once

#include "gameframework/InspectorSeam.h"
#include "gameframework/tools/inspector/HierarchyPanel.h"
#include "gameframework/tools/inspector/InspectorPanel.h"
#include "gameframework/tools/inspector/EditorToolbar.h"
#include "gameframework/tools/inspector/SelectionService.h"

namespace acs::game { class FGame; class FNode2D; }

namespace helloscene {

class PanelLayout {
public:
    PanelLayout() noexcept = default;

    void Init()     noexcept;
    void Shutdown() noexcept;

    // Inspector 編集対象になる Provider を 1 つ登録 (典型: PlayerNode)。
    // 複数登録したい場合は呼び出し側で繰り返す。
    void RegisterProvider(acs::game::IInspectableProvider* provider) noexcept;

    // 起動時に何を選択状態にしておくか。Inspector に最初から値が見える UX 用。
    void SelectInitial(acs::game::FNodeId id) noexcept;

    // 1 フレーム分の panel 描画。`game` は Toolbar が GetGame に渡す対象、
    // `root` は Hierarchy が辿るツリーのルート。
    void DrawUI(acs::game::FGame& game, acs::game::FNode2D& root) noexcept;

private:
    acs::game::inspector::FHierarchyPanel   _hierarchy_panel;
    acs::game::inspector::FInspectorPanel   _inspector_panel;
    acs::game::inspector::FEditorToolbar    _toolbar;
    acs::game::inspector::FSelectionService _selection;
    acs::game::FInspectorSeam               _seam;
};

} // namespace helloscene
