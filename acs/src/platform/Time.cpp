// SPDX-License-Identifier: Apache-2.0
// 時間計測実装 — std::chrono::steady_clock ベース (完全 portable)
//
// QueryPerformanceCounter を直接使うのを止めて、std::chrono に統一。
// 精度は同等 (Windows では steady_clock の実装が QPC ベース)、
// Linux/macOS では clock_gettime(CLOCK_MONOTONIC) ベース。
#include "platform/Time.h"
#include <chrono>

namespace acs {

namespace {
using SteadyClock = std::chrono::steady_clock;
struct TimeState {
    SteadyClock::time_point start;
    TimeState() noexcept : start(SteadyClock::now()) {}
};
const TimeState& GetTimeState() noexcept { static TimeState s; return s; }
} // namespace

f64 FClock::SecondsSinceStartup() noexcept {
    using namespace std::chrono;
    auto dt = SteadyClock::now() - GetTimeState().start;
    return duration<f64>(dt).count();
}

u64 FClock::MillisSinceStartup() noexcept {
    using namespace std::chrono;
    auto dt = SteadyClock::now() - GetTimeState().start;
    return static_cast<u64>(duration_cast<milliseconds>(dt).count());
}

u64 FClock::Ticks() noexcept {
    // 内部表現の生 tick (ナノ秒 or プラットフォーム依存)
    return static_cast<u64>(SteadyClock::now().time_since_epoch().count());
}

u64 FClock::TicksPerSecond() noexcept {
    using Period = SteadyClock::period;   // ratio<num, den>
    // ticks/sec = den / num
    return static_cast<u64>(Period::den / Period::num);
}

FFrameTimer::FFrameTimer() noexcept {
    _last_ticks = FClock::Ticks();
}

f32 FFrameTimer::Tick() noexcept {
    u64 now = FClock::Ticks();
    u64 freq = FClock::TicksPerSecond();
    f32 dt = static_cast<f32>(static_cast<f64>(now - _last_ticks) / static_cast<f64>(freq));
    _last_ticks = now;

    // ブレークポイント等で長時間止まった後の暴走を防ぐためクランプ
    if (dt > 0.25f) dt = 0.25f;
    if (dt < 0.0f)  dt = 0.0f;

    // 指数移動平均で平滑化（FPS 表示などのチラつき防止、α=0.1）
    if (_smoothed == 0.0f) _smoothed = dt;
    else                    _smoothed = _smoothed * 0.9f + dt * 0.1f;

    _total += dt;
    ++_frames;
    return dt;
}

} // namespace acs
