// SPDX-License-Identifier: Apache-2.0
// エンジン基盤の決定的な作業量と補助タイミングを JSON で報告する。
#include "foundation/Log.h"
#include "platform/Input.h"

#include <chrono>
#include <cstdio>

namespace {

using namespace acs;

constexpr u32 CountBits(u32 value) noexcept
{
    u32 count = 0;
    while (value != 0) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

constexpr bool ValidateDisconnectedRoundRobin() noexcept
{
    bool connected[4]{};
    detail::TGamepadPollScheduler<4> scheduler;
    return scheduler.BuildPollMask(connected) == 0x1u &&
           scheduler.BuildPollMask(connected) == 0x2u &&
           scheduler.BuildPollMask(connected) == 0x4u &&
           scheduler.BuildPollMask(connected) == 0x8u &&
           scheduler.BuildPollMask(connected) == 0x1u;
}

constexpr bool ValidateConnectedFullRate() noexcept
{
    bool connected[4]{true, false, true, false};
    detail::TGamepadPollScheduler<4> scheduler;
    const u32 first = scheduler.BuildPollMask(connected);
    const u32 second = scheduler.BuildPollMask(connected);
    return (first & 0x5u) == 0x5u &&
           (second & 0x5u) == 0x5u &&
           CountBits(first) == 3u &&
           CountBits(second) == 3u &&
           (first | second) == 0xFu;
}

static_assert(ValidateDisconnectedRoundRobin());
static_assert(ValidateConnectedFullRate());
static_assert(!detail::ContainsLogFormatMarker("literal message"));
static_assert(detail::ContainsLogFormatMarker("value=%u"));
static_assert(detail::TLogLiteralInfo<const char[16]>::IsLiteral);
static_assert(!detail::TLogLiteralInfo<const char*>::IsLiteral);
static_assert(
    detail::ClassifyLogDispatch("literal message") ==
    detail::ELogDispatchKind::Literal);
static_assert(
    detail::ClassifyLogDispatch("value=%u") ==
    detail::ELogDispatchKind::Formatted);
static_assert(FLogger::CompiledEnabled<ELogSeverity::Fatal>());

} // namespace

int main()
{
    using namespace acs;
    constexpr u64 kFrames = 1'000'000;
    constexpr u64 kLoggerBurst = 4096;

    bool connected[4]{};
    detail::TGamepadPollScheduler<4> scheduler;
    u64 optimized_poll_calls = 0;
    u32 observed_ports = 0;

    const auto begin = std::chrono::steady_clock::now();
    for (u64 frame = 0; frame < kFrames; ++frame) {
        const u32 mask = scheduler.BuildPollMask(connected);
        optimized_poll_calls += CountBits(mask);
        observed_ports |= mask;
    }
    const auto end = std::chrono::steady_clock::now();

    u64 optimized_wake_signals = 0;
    for (u64 position = 0; position < kLoggerBurst; ++position) {
        if (detail::ShouldSignalLogConsumer(position, 0)) {
            ++optimized_wake_signals;
        }
    }

    const u64 baseline_poll_calls = kFrames * 4u;
    const u64 baseline_wake_signals = kLoggerBurst;
    const u64 scheduler_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    const bool polling_ok =
        optimized_poll_calls == kFrames && observed_ports == 0xFu;
    const bool logging_ok = optimized_wake_signals == 1u;
    const bool ok = polling_ok && logging_ok;

    std::printf(
        "{\n"
        "  \"schema\": 1,\n"
        "  \"status\": \"%s\",\n"
        "  \"xinput\": {\"frames\": %llu, \"baseline_calls\": %llu, "
        "\"optimized_calls\": %llu, \"reduction_percent\": 75, "
        "\"reconnect_bound_frames\": 4},\n"
        "  \"logger\": {\"burst_records\": %llu, \"baseline_wake_signals\": %llu, "
        "\"optimized_wake_signals\": %llu},\n"
        "  \"timing_diagnostic\": {\"scheduler_total_ns\": %llu, "
        "\"scheduler_ns_per_frame\": %.3f},\n"
        "  \"compiled_log_min_severity\": %d\n"
        "}\n",
        ok ? "pass" : "fail",
        static_cast<unsigned long long>(kFrames),
        static_cast<unsigned long long>(baseline_poll_calls),
        static_cast<unsigned long long>(optimized_poll_calls),
        static_cast<unsigned long long>(kLoggerBurst),
        static_cast<unsigned long long>(baseline_wake_signals),
        static_cast<unsigned long long>(optimized_wake_signals),
        static_cast<unsigned long long>(scheduler_ns),
        static_cast<double>(scheduler_ns) / static_cast<double>(kFrames),
        ACS_COMPILED_LOG_MIN_SEVERITY);
    return ok ? 0 : 1;
}
