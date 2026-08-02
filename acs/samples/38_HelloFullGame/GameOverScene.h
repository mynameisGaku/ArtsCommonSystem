// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — GameOver scene。VICTORY / GAME OVER 表示、R で Title 戻し。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellofg {

class AGameOverScene : public acs::game::AScene {
public:
    AGameOverScene(acs::u64 final_score, bool did_win) noexcept
        : m_FinalScore(final_score), m_bDidWin(did_win) {}

    acs::game::ESvc WantedServices() const noexcept override {
        return acs::game::ESvc::Default2D;
    }

    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

private:
    acs::u64 m_FinalScore   = 0;
    acs::u64 m_SavedBest    = 0;
    bool     m_bDidWin       = false;
    bool     m_bIsNewRecord = false;
    acs::f32 _state_sec     = 0.0f;
};

} // namespace hellofg
