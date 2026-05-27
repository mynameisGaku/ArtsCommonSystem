// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — Particle 編集対象 Scene。
//
// emitter 1 個 + FParticleEditorPanel + FParticleEditorPreview を保持する 1
// シーン。WantedServices() は None なので、自前で m_ParticleSystem.Tick /
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
    void m_DrawFileMenu() noexcept;
    void m_SavePreset()    noexcept;
    void m_LoadPreset()    noexcept;

    acs::game::FParticleEffectSystem          m_ParticleSystem;
    acs::game::fxedit::FParticleEditorPanel   m_EditorPanel;
    acs::game::fxedit::FParticleEditorPreview m_EditorPreview;
    acs::game::FEmitterHandle                 m_DefaultEmitter;
};

} // namespace helloparticleed
