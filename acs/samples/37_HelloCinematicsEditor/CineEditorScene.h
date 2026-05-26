// SPDX-License-Identifier: Apache-2.0
// HelloCinematicsEditor — CineEditorScene。
// editor_core の FEditorWorkspace + FEditorTheme と
// cinetimeline::FCinematicsTimelineEditorPanel を 1 個の Workspace に集約し、
// FCinematicsDirector を bind して 3 個の初期 keyframe を持つ FScene。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/CinematicsDirector.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/EditorTheme.h"
#include "gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.h"

#include "math/Vec.h"

namespace hellocine {

class CineEditorScene : public acs::game::FScene {
public:
    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

private:
    // ---- File menu stub (実 dialog / serializer は将来予定) ----
    static constexpr const char* kCinePath = "preset.acscinetimeline";

    // ---- editor_core ----
    acs::game::editor_core::FEditorWorkspace                  _workspace;
    acs::game::editor_core::FEditorTheme                      _theme;

    // ---- cinetimeline ----
    acs::game::cinetimeline::FCinematicsTimelineEditorPanel   _cine_panel;

    // ---- 編集対象の FCinematicsDirector (= FScene が所有、panel は raw 参照) ----
    acs::game::FCinematicsDirector                            _director;

    // ---- runtime callback (= keyframe 発火可視化用、ACS_LOG_INFO に出力) ----
    static void OnCamera(void* /*user*/, acs::FVec2 target, acs::f32 zoom, acs::f32 dur) noexcept;
    static void OnEvent (void* /*user*/, acs::u32 event_id) noexcept;
};

} // namespace hellocine
