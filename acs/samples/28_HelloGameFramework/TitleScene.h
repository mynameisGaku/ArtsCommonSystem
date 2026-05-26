// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Title scene。FSM (Idle/Blink) で 2s 毎に明滅。
//
// Phase 4: `StateMachine<TitleScene>` で 2 状態 FSM。Owner& 経由で self
// 参照する static 関数として状態関数を実装。
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
    acs::game::StateMachine<TitleScene> _fsm;
    acs::game::SceneClock _clock;
    acs::f32 _state_secs = 0.0f;
};

} // namespace hellogf
