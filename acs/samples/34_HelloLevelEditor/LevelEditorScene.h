// SPDX-License-Identifier: Apache-2.0
// HelloLevelEditor — Scene。Workspace + LevelEditorPanel + Tilemap (32x32, 2 layer)。
//
// 初期 tilemap パターン:
//   - layer 0 (床) : 全面 tile id=1 で埋める (= 緑系の床色)
//   - layer 1 (壁) : 外周 (x=0 / x=31 / y=0 / y=31) を tile id=10 で囲む、
//                    中央 (8..23, 8..23) の枠を tile id=20 で囲む
#pragma once

#include "gameframework/GameFramework.h"

// ----- editor_core (Phase 21a) -----
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/EditorTheme.h"

// ----- leveledit (Phase 22) -----
#include "gameframework/tools/leveledit/LevelEditorPanel.h"

// ----- Tilemap (Pillar Q) -----
#include "gameframework/Tilemap.h"

namespace hellole {

class LevelEditorScene : public acs::game::Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    // File menu stub (実 serializer は Phase 23+ で配線)
    static constexpr const char* kSavePath = "tilemap.acstilemap";

    // ---- editor_core (Phase 21a) ----
    acs::game::editor_core::EditorWorkspace _workspace;
    acs::game::editor_core::EditorTheme     _theme;

    // ---- leveledit (Phase 22) ----
    acs::game::leveledit::LevelEditorPanel  _level_panel;

    // ---- 編集対象 Tilemap ----
    acs::game::Tilemap                      _tilemap;
};

} // namespace hellole
