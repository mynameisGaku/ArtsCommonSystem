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
//           _clock.Tick(dt);
//           // _clock.Dt() を FTween 等の更新に渡す
//       }
//       void OnPause()  noexcept override { _clock.Pause();  }
//       void OnResume() noexcept override { _clock.Resume(); }
//   private:
//       acs::game::FSceneClock _clock;
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
        _last_dt_unscaled = dt;
        const f32 scaled = _paused ? 0.0f : dt * _time_scale;
        _last_dt        = scaled;
        _time           += scaled;
        _time_unscaled  += dt;
        ++_frame_count;
    }

    void Pause()  noexcept { _paused = true;  }
    void Resume() noexcept { _paused = false; }
    bool IsPaused() const noexcept { return _paused; }

    f32 Dt()           const noexcept { return _last_dt; }
    f32 DtUnscaled()   const noexcept { return _last_dt_unscaled; }
    f32 Time()         const noexcept { return _time; }
    f32 TimeUnscaled() const noexcept { return _time_unscaled; }
    u64 Frame()        const noexcept { return _frame_count; }

    // 倍率。0 でフリーズ (pause と同等)、1 が通常、>1 で早送り、<1 で slow-mo。
    void SetTimeScale(f32 s) noexcept { _time_scale = s < 0.0f ? 0.0f : s; }
    f32  TimeScale() const noexcept { return _time_scale; }

    // 時間積算と frame count のみリセット (pause / time_scale は維持)。
    void Reset() noexcept {
        _time = 0.0f;
        _time_unscaled = 0.0f;
        _last_dt = 0.0f;
        _last_dt_unscaled = 0.0f;
        _frame_count = 0;
    }

private:
    f32 _time             = 0.0f;
    f32 _time_unscaled    = 0.0f;
    f32 _last_dt          = 0.0f;
    f32 _last_dt_unscaled = 0.0f;
    f32 _time_scale       = 1.0f;
    u64 _frame_count      = 0;
    bool _paused          = false;
};

} // namespace acs::game
