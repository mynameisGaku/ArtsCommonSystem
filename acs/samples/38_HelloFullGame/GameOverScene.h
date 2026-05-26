// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — GameOver scene。VICTORY / GAME OVER 表示、R で Title 戻し。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellofg {

class GameOverScene : public acs::game::Scene {
public:
    GameOverScene(acs::u64 final_score, bool did_win) noexcept
        : _final_score(final_score), _did_win(did_win) {}

    acs::game::ESvc WantedServices() const noexcept override {
        return acs::game::ESvc::Default2D;
    }

    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    acs::u64 _final_score   = 0;
    acs::u64 _saved_best    = 0;
    bool     _did_win       = false;
    bool     _is_new_record = false;
    acs::f32 _state_sec     = 0.0f;
};

} // namespace hellofg
