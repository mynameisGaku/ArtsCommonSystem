// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Title scene。"Press Space to Start" 待ち + 背景色 ping-pong。
#pragma once

#include "gameframework/GameFramework.h"
#include "math/Vec.h"

namespace hellofg {

class TitleScene : public acs::game::Scene {
public:
    acs::game::ESvc WantedServices() const noexcept override {
        return acs::game::ESvc::Default2D;   // Clock | Tweens | Sequences | Input
    }

    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    acs::FVec3                  m_BgColor  {0.06f, 0.08f, 0.16f};
    acs::game::FTweenHandle     m_BgTween  {};
    bool                       m_ToBright = true;
    acs::f32                   m_PulseSec = 0.0f;   // "Press Space" の点滅位相

    static constexpr acs::FVec3 kBgDark   {0.06f, 0.08f, 0.16f};
    static constexpr acs::FVec3 kBgBright {0.16f, 0.20f, 0.35f};
};

} // namespace hellofg
