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

/** 基準クロック型 (portable な単調増加クロック)。 */
using SteadyClock = std::chrono::steady_clock;

/** 起動基準点を保持する状態。 */
struct FTimeState {
    /** 起点となる時刻 (構築時の now)。 */
    SteadyClock::time_point start;

    /** 現在時刻を起点として初期化する。 */
    FTimeState() noexcept : start(SteadyClock::now()) {}
};

/**
 * 唯一の TimeState を遅延構築して返す。
 *
 * @details 初回呼び出し時の now が以降すべての経過時間計測の起点になる。
 * @return プロセス唯一の TimeState への const 参照。
 */
const FTimeState& GetTimeState() noexcept { static FTimeState s; return s; }

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
    m_LastTicks = FClock::Ticks();
}

f32 FFrameTimer::Tick() noexcept {
    const u64 now = FClock::Ticks();
    const u64 freq = FClock::TicksPerSecond();
    f32 dt = static_cast<f32>(static_cast<f64>(now - m_LastTicks) / static_cast<f64>(freq));
    m_LastTicks = now;

    // ブレークポイント等で長時間止まった後の暴走を防ぐためクランプ
    if (dt > 0.25f) dt = 0.25f;
    if (dt < 0.0f)  dt = 0.0f;

    // 指数移動平均で平滑化（FPS 表示などのチラつき防止、α=0.1）
    if (m_Smoothed == 0.0f) m_Smoothed = dt;
    else                    m_Smoothed = m_Smoothed * 0.9f + dt * 0.1f;

    m_Total += dt;
    ++m_Frames;
    return dt;
}

} // namespace acs
