// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — ATitleScene 実装。
#include "TitleScene.h"
#include "PlayerProfile.h"
#include "GameplayScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellogf {

void ATitleScene::OnEnter() noexcept {
    m_Fsm.Configure(Idle,  { &ATitleScene::EnterIdle,  &ATitleScene::UpdateIdle,  nullptr });
    m_Fsm.Configure(Blink, { &ATitleScene::EnterBlink, &ATitleScene::UpdateBlink, nullptr });
    m_Fsm.Start(Idle, *this);
    auto* prof = GetGame().AppState<FPlayerProfile>();
    if (prof) {
        ACS_LOG_INFO("[Title] hi_score=%u sessions=%u  (Space: start, Esc: quit) FSM Idle/Blink",
                     prof->hi_score, prof->sessions);
    } else {
        ACS_LOG_INFO("[Title] (Space: start, Esc: quit) FSM Idle/Blink");
    }
}

void ATitleScene::OnExit() noexcept { ACS_LOG_INFO("[Title] exit"); }

void ATitleScene::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) GetGame().Quit();
    if (CInput::IsKeyPressed(EKey::Space)) {
        Scenes().ChangeScene(MakeUnique<AGameplayScene>());
        return;
    }
    m_Clock.Tick(dt);
    m_Fsm.Update(*this, m_Clock.Dt());
}

// 2 秒間 Idle (dark blue) → 0.3 秒 Blink (明るい青) を交互に繰返す FSM デモ。
void ATitleScene::EnterIdle(ATitleScene& s) noexcept {
    s.GetGame().SetClearColor(0.10f, 0.12f, 0.25f);
    s._state_secs = 0.0f;
}
void ATitleScene::UpdateIdle(ATitleScene& s, f32 dt) noexcept {
    s._state_secs += dt;
    if (s._state_secs > 2.0f) s.m_Fsm.ChangeState(Blink, s);
}
void ATitleScene::EnterBlink(ATitleScene& s) noexcept {
    s.GetGame().SetClearColor(0.20f, 0.25f, 0.50f);
    s._state_secs = 0.0f;
}
void ATitleScene::UpdateBlink(ATitleScene& s, f32 dt) noexcept {
    s._state_secs += dt;
    if (s._state_secs > 0.3f) s.m_Fsm.ChangeState(Idle, s);
}

} // namespace hellogf
