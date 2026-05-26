// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Pause scene。Gameplay の上に push される overlay。
//
// Phase 4: Sequence + SequenceRunner で Wait → Call → Wait → Call Loop(0)
// (= 無限) を仕込み、Pause を抜けるまで「still paused...」をログに出し続ける。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class PauseScene : public acs::game::Scene {
public:
    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;

    // Sequence Call action 用 (関数ポインタ)
    static void LogStillPaused1(void* user) noexcept;
    static void LogStillPaused2(void* user) noexcept;

private:
    acs::game::SequenceRunner _seqs;
    acs::game::SceneClock     _clock;
};

} // namespace hellogf
