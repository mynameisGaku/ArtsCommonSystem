// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — APauseScene 実装。
#include "PauseScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellogf {

void APauseScene::OnEnter() noexcept {
    GetGame().SetClearColor(0.18f, 0.18f, 0.20f);   // grey
    // Wait → Call → Wait → Call を Loop(0)=無限。Pause を抜けるまで定期出力する。
    FSequence s;
    s.Wait(0.5f)
     .Call(&APauseScene::LogStillPaused1, this)
     .Wait(1.5f)
     .Call(&APauseScene::LogStillPaused2, this)
     .Loop(0);
    m_Seqs.Start(Move(s));
    ACS_LOG_INFO("[Pause] (P: resume, Esc: quit) FSequence loop start");
}

void APauseScene::OnExit() noexcept {
    m_Seqs.CancelAll();
    ACS_LOG_INFO("[Pause] exit");
}

void APauseScene::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) GetGame().Quit();
    if (CInput::IsKeyPressed(EKey::P)) {
        Scenes().PopScene();
        return;
    }
    m_Clock.Tick(dt);
    m_Seqs.Tick(m_Clock.Dt());
}

void APauseScene::LogStillPaused1(void* /*user*/) noexcept {
    ACS_LOG_INFO("[Pause] still paused...");
}

void APauseScene::LogStillPaused2(void* /*user*/) noexcept {
    ACS_LOG_INFO("[Pause] still paused longer...");
}

} // namespace hellogf
