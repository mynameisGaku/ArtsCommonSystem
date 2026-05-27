// SPDX-License-Identifier: Apache-2.0
// HelloLevelEditor — Scene。Workspace + FLevelEditorPanel + FTilemap (32x32, 2 layer)。
//
// 初期 tilemap パターン:
//   - layer 0 (床) : 全面 tile id=1 で埋める (= 緑系の床色)
//   - layer 1 (壁) : 外周 (x=0 / x=31 / y=0 / y=31) を tile id=10 で囲む、
//                    中央 (8..23, 8..23) の枠を tile id=20 で囲む
#pragma once

#include "gameframework/GameFramework.h"

// editor_core: ImGui ベースのエディタ基盤 (Workspace + Theme)。
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/EditorTheme.h"

// leveledit: FTilemap 編集用の ImGui Panel。
#include "gameframework/tools/leveledit/LevelEditorPanel.h"

// FTilemap: 2D グリッド状の tile データコンテナ (Pillar Q)。
#include "gameframework/Tilemap.h"

namespace hellole {

class LevelEditorScene : public acs::game::Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    // OnEnter から呼ばれる、tilemap の初期パターン書き込み (床 + 壁)。
    // 「データ生成」と「panel/workspace 配線」を分けるための private helper。
    void _seed_initial_tilemap_pattern() noexcept;

    // OnRender 内で main menu bar に "File" / "Brush" メニューを push する
    // private helper。各メニューを 1 関数に分けて OnRender 本体を短く保つ。
    void _draw_file_menu() noexcept;
    void _draw_brush_menu() noexcept;

    // File メニューの Save/Load stub が示すパス (実シリアライザは未配線)。
    static constexpr const char* kSavePath = "tilemap.acstilemap";

    // ImGui ベースのエディタ基盤 (Workspace + Theme)。
    acs::game::editor_core::FEditorWorkspace _workspace;
    acs::game::editor_core::FEditorTheme     _theme;

    // FTilemap を編集するための ImGui Panel。
    acs::game::leveledit::FLevelEditorPanel  _level_panel;

    // 編集対象 FTilemap。FLevelEditorPanel に raw 参照で渡す (所有は本 Scene 側)。
    acs::game::FTilemap                      _tilemap;
};

} // namespace hellole
