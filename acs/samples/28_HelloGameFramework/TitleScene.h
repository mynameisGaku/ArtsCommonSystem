// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Title scene。
//
// `FStateMachine<TitleScene>` の 2 状態 (Idle / Blink) FSM で 2s 毎に色を切り替える。
// 状態関数は static で書き、Owner& 経由で self に touch する流儀。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class TitleScene : public acs::game::Scene {
public:
    enum States : acs::u32 { Idle = 0, Blink };

    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;

    // FSM 状態関数 (静的、Owner& 経由で self 参照)
    static void EnterIdle (TitleScene& s)              noexcept;
    static void UpdateIdle(TitleScene& s, acs::f32 dt) noexcept;
    static void EnterBlink (TitleScene& s)             noexcept;
    static void UpdateBlink(TitleScene& s, acs::f32 dt) noexcept;

private:
    acs::game::FStateMachine<TitleScene> _fsm;
    acs::game::FSceneClock _clock;
    acs::f32 _state_secs = 0.0f;
};

} // namespace hellogf
