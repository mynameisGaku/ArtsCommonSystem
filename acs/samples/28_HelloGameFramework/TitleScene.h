// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Title scene。
//
// `TStateMachine<ATitleScene>` の 2 状態 (Idle / Blink) FSM で 2s 毎に色を切り替える。
// 状態関数は static で書き、Owner& 経由で self に touch する流儀。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class ATitleScene : public acs::game::AScene {
public:
    enum EStates : acs::u32 { Idle = 0, Blink };

    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;

    // FSM 状態関数 (静的、Owner& 経由で self 参照)
    static void EnterIdle (ATitleScene& s)              noexcept;
    static void UpdateIdle(ATitleScene& s, acs::f32 dt) noexcept;
    static void EnterBlink (ATitleScene& s)             noexcept;
    static void UpdateBlink(ATitleScene& s, acs::f32 dt) noexcept;

private:
    acs::game::TStateMachine<ATitleScene> m_Fsm;
    acs::game::CSceneClock m_Clock;
    acs::f32 _state_secs = 0.0f;
};

} // namespace hellogf
