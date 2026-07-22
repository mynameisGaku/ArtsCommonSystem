// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Title scene。
//
// `TStateMachine<FTitleScene>` の 2 状態 (Idle / Blink) FSM で 2s 毎に色を切り替える。
// 状態関数は static で書き、Owner& 経由で self に touch する流儀。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class FTitleScene : public acs::game::FScene {
public:
    enum EStates : acs::u32 { Idle = 0, Blink };

    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;

    // FSM 状態関数 (静的、Owner& 経由で self 参照)
    static void EnterIdle (FTitleScene& s)              noexcept;
    static void UpdateIdle(FTitleScene& s, acs::f32 dt) noexcept;
    static void EnterBlink (FTitleScene& s)             noexcept;
    static void UpdateBlink(FTitleScene& s, acs::f32 dt) noexcept;

private:
    acs::game::TStateMachine<FTitleScene> m_Fsm;
    acs::game::FSceneClock m_Clock;
    acs::f32 _state_secs = 0.0f;
};

} // namespace hellogf
