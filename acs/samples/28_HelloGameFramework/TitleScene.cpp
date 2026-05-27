// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — TitleScene 実装。
#include "TitleScene.h"
#include "PlayerProfile.h"
#include "GameplayScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellogf {

void TitleScene::OnEnter() noexcept {
    m_Fsm.Configure(Idle,  { &TitleScene::EnterIdle,  &TitleScene::UpdateIdle,  nullptr });
    m_Fsm.Configure(Blink, { &TitleScene::EnterBlink, &TitleScene::UpdateBlink, nullptr });
    m_Fsm.Start(Idle, *this);
    auto* prof = GetGame().AppState<PlayerProfile>();
    if (prof) {
        ACS_LOG_INFO("[Title] hi_score=%u sessions=%u  (Space: start, Esc: quit) FSM Idle/Blink",
                     prof->hi_score, prof->sessions);
    } else {
        ACS_LOG_INFO("[Title] (Space: start, Esc: quit) FSM Idle/Blink");
    }
}

void TitleScene::OnExit() noexcept { ACS_LOG_INFO("[Title] exit"); }

void TitleScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) GetGame().Quit();
    if (Input::IsKeyPressed(EKey::Space)) {
        Scenes().ChangeScene(MakeUnique<GameplayScene>());
        return;
    }
    m_Clock.Tick(dt);
    m_Fsm.Update(*this, m_Clock.Dt());
}

// 2 秒間 Idle (dark blue) → 0.3 秒 Blink (明るい青) を交互に繰返す FSM デモ。
void TitleScene::EnterIdle(TitleScene& s) noexcept {
    s.GetGame().SetClearColor(0.10f, 0.12f, 0.25f);
    s._state_secs = 0.0f;
}
void TitleScene::UpdateIdle(TitleScene& s, f32 dt) noexcept {
    s._state_secs += dt;
    if (s._state_secs > 2.0f) s.m_Fsm.ChangeState(Blink, s);
}
void TitleScene::EnterBlink(TitleScene& s) noexcept {
    s.GetGame().SetClearColor(0.20f, 0.25f, 0.50f);
    s._state_secs = 0.0f;
}
void TitleScene::UpdateBlink(TitleScene& s, f32 dt) noexcept {
    s._state_secs += dt;
    if (s._state_secs > 0.3f) s.m_Fsm.ChangeState(Idle, s);
}

} // namespace hellogf
