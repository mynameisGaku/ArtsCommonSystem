// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — GameOver scene。VICTORY / GAME OVER 表示、R で Title 戻し。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellofg {

class GameOverScene : public acs::game::Scene {
public:
    GameOverScene(acs::u64 final_score, bool did_win) noexcept
        : m_FinalScore(final_score), m_DidWin(did_win) {}

    acs::game::ESvc WantedServices() const noexcept override {
        return acs::game::ESvc::Default2D;
    }

    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    acs::u64 m_FinalScore   = 0;
    acs::u64 m_SavedBest    = 0;
    bool     m_DidWin       = false;
    bool     m_IsNewRecord = false;
    acs::f32 _state_sec     = 0.0f;
};

} // namespace hellofg
