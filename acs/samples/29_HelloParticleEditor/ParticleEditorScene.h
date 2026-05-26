// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — Particle 編集対象 Scene。
//
// emitter 1 個 + ParticleEditorPanel + ParticleEditorPreview を保持する 1
// シーン。WantedServices() は None (= 自前で _particle_system.Tick / Preview.Tick
// を呼ぶ)。main menu bar "File > Save .fxedit / Load .fxedit" で
// `FxeditSerializer` 経由の永続化を行う。
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

    acs::game::ParticleEffectSystem        _particle_system;
    acs::game::fxedit::ParticleEditorPanel _editor_panel;
    acs::game::fxedit::ParticleEditorPreview _editor_preview;
    acs::game::EmitterHandle               _default_emitter;     // OnEnter で作る emitter
};

} // namespace helloparticleed
