// 時間計測実装
#include "platform/Time.h"
#include "foundation/Platform.h"

namespace acs {

namespace {
struct TimeState {
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    TimeState() noexcept {
        ::QueryPerformanceFrequency(&freq);
        ::QueryPerformanceCounter(&start);
    }
};
const TimeState& GetTimeState() noexcept { static TimeState s; return s; }
} // namespace

f64 Clock::SecondsSinceStartup() noexcept {
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    const auto& s = GetTimeState();
    return static_cast<f64>(now.QuadPart - s.start.QuadPart) / static_cast<f64>(s.freq.QuadPart);
}

u64 Clock::MillisSinceStartup() noexcept {
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    const auto& s = GetTimeState();
    return static_cast<u64>((now.QuadPart - s.start.QuadPart) * 1000 / s.freq.QuadPart);
}

u64 Clock::Ticks() noexcept {
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    return static_cast<u64>(now.QuadPart);
}

u64 Clock::TicksPerSecond() noexcept {
    return static_cast<u64>(GetTimeState().freq.QuadPart);
}

FrameTimer::FrameTimer() noexcept {
    _last_ticks = Clock::Ticks();
}

// dt を計算して内部状態を進める
f32 FrameTimer::Tick() noexcept {
    u64 now = Clock::Ticks();
    u64 freq = Clock::TicksPerSecond();
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
