// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — Particle 編集対象 Scene。
//
// emitter 1 個 + FParticleEditorPanel + FParticleEditorPreview を保持する 1
// シーン。WantedServices() は None なので、自前で _particle_system.Tick /
// Preview.Tick を呼ぶ。main menu bar "File > Save .fxedit / Load .fxedit" で
// `FFxeditSerializer` 経由の永続化を行う。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/ParticleEffectSystem.h"
#include "gameframework/tools/fxedit/ParticleEditorPanel.h"
#include "gameframework/tools/fxedit/ParticleEditorPreview.h"

namespace helloparticleed {

class ParticleEditorScene : public acs::game::Scene {
public:
    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    // File メニューで Save/Load を選んだときのファイルパス (固定)。
    static constexpr const char* kPresetPath = "preset.fxedit";

    // OnRender の File メニュー本体を二分割して、メニュー UI と
    // 永続化詳細 (FFxeditSerializer 呼出) を切り離す。
    void _draw_file_menu() noexcept;
    void _save_preset()    noexcept;
    void _load_preset()    noexcept;

    acs::game::FParticleEffectSystem          _particle_system;
    acs::game::fxedit::FParticleEditorPanel   _editor_panel;
    acs::game::fxedit::FParticleEditorPreview _editor_preview;
    acs::game::FEmitterHandle                 _default_emitter;
};

} // namespace helloparticleed
