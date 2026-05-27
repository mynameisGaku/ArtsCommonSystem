// SPDX-License-Identifier: Apache-2.0
// 時間計測（高精度タイマ + フレーム時間管理）
//
// 使い方:
//   FrameTimer ft;
//   while (running) {
//       f32 dt = ft.Tick();   // 経過秒（前フレームから今フレームまで）
//       Update(dt);
//       Render();
//   }
#pragma once

#include "foundation/Types.h"

namespace acs {

// 時刻ユーティリティ
class Clock {
public:
    // 起動からの経過秒（高精度）
    static f64 SecondsSinceStartup() noexcept;
    // 起動からの経過ミリ秒
    static u64 MillisSinceStartup() noexcept;
    // 経過 100ns ティック（QPC ベース）
    static u64 Ticks() noexcept;
    // 1 秒あたりのティック数
    static u64 TicksPerSecond() noexcept;
};

// フレーム時間計測（毎フレーム Tick を呼ぶ）
class FrameTimer {
public:
    FrameTimer() noexcept;

    // 経過秒を返し、内部時刻を更新する。最大 dt は 0.25 秒に制限（停止後の暴走防止）
    f32 Tick() noexcept;

    // 過去 N フレームの平均 dt
    f32 SmoothedDelta() const noexcept { return m_Smoothed; }
    // 過去 N フレームの平均 FPS
    f32 SmoothedFPS()   const noexcept { return m_Smoothed > 0 ? 1.0f / m_Smoothed : 0; }
    // 累積フレーム数
    u64 FrameCount()    const noexcept { return m_Frames; }
    // 起動からの累積秒
    f64 TotalSeconds()  const noexcept { return m_Total; }

private:
    u64 m_LastTicks = 0;     // 前回 Tick の時刻
    f32 m_Smoothed   = 0.0f;  // 平滑化された dt
    f64 m_Total      = 0.0;
    u64 m_Frames     = 0;
};

} // namespace acs
