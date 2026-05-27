// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Pause scene。Gameplay の上に push される overlay。
//
// FSequence + FSequenceRunner で Wait → Call → Wait → Call の Loop(0)=無限 を組み、
// Pause を抜けるまで「still paused...」を周期的にログ出力するデモ。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class PauseScene : public acs::game::Scene {
public:
    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;

    // FSequence Call action 用 (関数ポインタ)
    static void LogStillPaused1(void* user) noexcept;
    static void LogStillPaused2(void* user) noexcept;

private:
    acs::game::FSequenceRunner _seqs;
    acs::game::FSceneClock     _clock;
};

} // namespace hellogf
