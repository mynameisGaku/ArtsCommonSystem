// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FSceneClock (Phase 3)
//
// Scene 単位の時間トラッカ。scaled / unscaled 時間、frame count、pause/resume、
// per-clock time_scale を持つ軽量値型 (40 byte 程度)。FTween/FSequence/カスタム
// タイマー等が共通の時間軸として参照する。
//
// 命名: `acs::Clock` (platform/Time.h、ハイレベル時間 API) との衝突を避けるため
// `FSceneClock`。役割は「シーンの感じる時間 = pause/slow-mo を反映する論理時間」。
//
// 使い方:
//   class GameplayScene : public Scene {
//   public:
//       void OnUpdate(f32 dt) noexcept override {
//           m_Clock.Tick(dt);
//           // m_Clock.Dt() を FTween 等の更新に渡す
//       }
//       void OnPause()  noexcept override { m_Clock.Pause();  }
//       void OnResume() noexcept override { m_Clock.Resume(); }
//   private:
//       acs::game::FSceneClock m_Clock;
//   };
//
// FGame の FApplication::DeltaTime() は常にリアル時間。シーンの感じる「時間」
// (slow-mo・pause・スピードランナーの倍速モード等) は FSceneClock を経由する。
#pragma once

#include "foundation/Types.h"

namespace acs::game {

class FSceneClock {
public:
    FSceneClock() noexcept = default;

    // dt を流す。pause 中は scaled 系が進まず、unscaled 系のみ進む。
    void Tick(f32 dt) noexcept {
        m_LastDtUnscaled = dt;
        const f32 scaled = m_Paused ? 0.0f : dt * m_TimeScale;
        m_LastDt        = scaled;
        m_Time           += scaled;
        m_TimeUnscaled  += dt;
        ++m_FrameCount;
    }

    void Pause()  noexcept { m_Paused = true;  }
    void Resume() noexcept { m_Paused = false; }
    bool IsPaused() const noexcept { return m_Paused; }

    f32 Dt()           const noexcept { return m_LastDt; }
    f32 DtUnscaled()   const noexcept { return m_LastDtUnscaled; }
    f32 Time()         const noexcept { return m_Time; }
    f32 TimeUnscaled() const noexcept { return m_TimeUnscaled; }
    u64 Frame()        const noexcept { return m_FrameCount; }

    // 倍率。0 でフリーズ (pause と同等)、1 が通常、>1 で早送り、<1 で slow-mo。
    void SetTimeScale(f32 s) noexcept { m_TimeScale = s < 0.0f ? 0.0f : s; }
    f32  TimeScale() const noexcept { return m_TimeScale; }

    // 時間積算と frame count のみリセット (pause / time_scale は維持)。
    void Reset() noexcept {
        m_Time = 0.0f;
        m_TimeUnscaled = 0.0f;
        m_LastDt = 0.0f;
        m_LastDtUnscaled = 0.0f;
        m_FrameCount = 0;
    }

private:
    f32 m_Time             = 0.0f;
    f32 m_TimeUnscaled    = 0.0f;
    f32 m_LastDt          = 0.0f;
    f32 m_LastDtUnscaled = 0.0f;
    f32 m_TimeScale       = 1.0f;
    u64 m_FrameCount      = 0;
    bool m_Paused          = false;
};

} // namespace acs::game
