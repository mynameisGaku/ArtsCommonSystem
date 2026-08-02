// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Pause scene。Gameplay の上に push される overlay。
//
// FSequence + CSequenceRunner で Wait → Call → Wait → Call の Loop(0)=無限 を組み、
// Pause を抜けるまで「still paused...」を周期的にログ出力するデモ。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class APauseScene : public acs::game::AScene {
public:
    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;

    // FSequence Call action 用 (関数ポインタ)
    static void LogStillPaused1(void* user) noexcept;
    static void LogStillPaused2(void* user) noexcept;

private:
    acs::game::CSequenceRunner m_Seqs;
    acs::game::CSceneClock     m_Clock;
};

} // namespace hellogf
