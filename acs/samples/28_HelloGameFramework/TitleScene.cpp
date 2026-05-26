// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — TitleScene 実装。
#include "TitleScene.h"
#include "GameTypes.h"
#include "GameplayScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellogf {

void TitleScene::OnEnter() noexcept {
    _fsm.Configure(Idle,  { &TitleScene::EnterIdle,  &TitleScene::UpdateIdle,  nullptr });
    _fsm.Configure(Blink, { &TitleScene::EnterBlink, &TitleScene::UpdateBlink, nullptr });
    _fsm.Start(Idle, *this);
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
    _clock.Tick(dt);
    _fsm.Update(*this, _clock.Dt());
}

// FSM 状態 (= Phase 4 デモ): 2 秒間 Idle (dark blue) → 0.3 秒 Blink (明るい青) を繰返
void TitleScene::EnterIdle(TitleScene& s) noexcept {
    s.GetGame().SetClearColor(0.10f, 0.12f, 0.25f);
    s._state_secs = 0.0f;
}
void TitleScene::UpdateIdle(TitleScene& s, f32 dt) noexcept {
    s._state_secs += dt;
    if (s._state_secs > 2.0f) s._fsm.ChangeState(Blink, s);
}
void TitleScene::EnterBlink(TitleScene& s) noexcept {
    s.GetGame().SetClearColor(0.20f, 0.25f, 0.50f);
    s._state_secs = 0.0f;
}
void TitleScene::UpdateBlink(TitleScene& s, f32 dt) noexcept {
    s._state_secs += dt;
    if (s._state_secs > 0.3f) s._fsm.ChangeState(Idle, s);
}

} // namespace hellogf
