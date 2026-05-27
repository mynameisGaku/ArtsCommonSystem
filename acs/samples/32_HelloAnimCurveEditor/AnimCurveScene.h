// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — Scene。Workspace + 1 panel + 1 FAnimationCurve
// (Hermite 3 key)。panel に curve を bind し、ImGui 上でキー編集を行える。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/AnimationCurve.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/EditorTheme.h"
#include "gameframework/tools/animcurve/AnimCurveEditorPanel.h"

namespace helloac {

class AnimCurveScene : public acs::game::Scene {
public:
    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    // File menu の Save/Load 用ファイルパス (実シリアライザは未実装、stub)。
    static constexpr const char* kCurvePath = "preset.acscurve";

    // MainMenuBar の File メニューを描画する。OnRender 内で Workspace の
    // DrawMenuBar より前に push する必要がある (= ImGui は同一フレーム内で
    // BeginMainMenuBar を複数回呼ぶと 1 個の bar にマージするため、本 sample
    // 固有の File メニューと Workspace の FWindow/Layout メニューを並べられる)。
    void _draw_file_menu() noexcept;

    acs::game::editor_core::FEditorWorkspace _workspace;
    acs::game::editor_core::FEditorTheme     _theme;
    acs::game::animcurve::FAnimCurveEditorPanel _curve_panel;

    // 編集対象の FAnimationCurve。Scene が所有し、panel は raw 参照を保持する。
    acs::game::FAnimationCurve _example_curve;
};

} // namespace helloac
