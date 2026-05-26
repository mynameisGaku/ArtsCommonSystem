// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — Scene。Workspace + 1 panel + 1 AnimationCurve
// (Hermite 3 key)。panel に curve を bind し、ImGui 上でキー編集を行える。
#pragma once

#include "gameframework/GameFramework.h"

// ----- AnimationCurve (編集対象) -----
#include "gameframework/AnimationCurve.h"

// ----- editor_core (Phase 21a) -----
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/EditorTheme.h"

// ----- animcurve (Phase 22) -----
#include "gameframework/tools/animcurve/AnimCurveEditorPanel.h"

namespace helloac {

class AnimCurveScene : public acs::game::Scene {
public:
    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    // ---- File menu stub (実 dialog / serializer は将来 Phase) ----
    static constexpr const char* kCurvePath = "preset.acscurve";

    // ---- editor_core (Phase 21a) ----
    acs::game::editor_core::EditorWorkspace _workspace;
    acs::game::editor_core::EditorTheme     _theme;

    // ---- animcurve (Phase 22) ----
    acs::game::animcurve::AnimCurveEditorPanel _curve_panel;

    // ---- 編集対象の AnimationCurve (= Scene が所有、panel は raw 参照) ----
    acs::game::AnimationCurve _example_curve;
};

} // namespace helloac
