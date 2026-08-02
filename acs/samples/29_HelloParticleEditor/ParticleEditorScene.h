// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — Particle 編集対象 Scene。
//
// emitter 1 個 + AParticleEditorPanel + CParticleEditorPreview を保持する 1
// シーン。WantedServices() は None なので、自前で m_ParticleSystem.Tick /
// Preview.Tick を呼ぶ。main menu bar "File > Save .fxedit / Load .fxedit" で
// `CFxeditSerializer` 経由の永続化を行う。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/ParticleEffectSystem.h"
#include "gameframework/tools/fxedit/ParticleEditorPanel.h"
#include "gameframework/tools/fxedit/ParticleEditorPreview.h"

namespace helloparticleed {

class AParticleEditorScene : public acs::game::AScene {
public:
    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

private:
    // File メニューで Save/Load を選んだときのファイルパス (固定)。
    // narrow はログ用 (%s)、wide は CFxeditSerializer (Win32 wide path) 用。
    static constexpr const char*    kPresetPath  = "preset.fxedit";
    static constexpr const wchar_t* kPresetPathW = L"preset.fxedit";

    // OnRender の File メニュー本体を二分割して、メニュー UI と
    // 永続化詳細 (CFxeditSerializer 呼出) を切り離す。
    void m_DrawFileMenu() noexcept;
    void m_SavePreset()    noexcept;
    void m_LoadPreset()    noexcept;

    acs::game::CParticleEffectSystem          m_ParticleSystem;
    acs::game::fxedit::AParticleEditorPanel   m_EditorPanel;
    acs::game::fxedit::CParticleEditorPreview m_EditorPreview;
    acs::game::FEmitterHandle                 m_DefaultEmitter;
};

} // namespace helloparticleed
