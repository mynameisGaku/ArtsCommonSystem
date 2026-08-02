// SPDX-License-Identifier: Apache-2.0
// エンジン基盤の決定的な作業量と補助タイミングを JSON で報告する。
#include "foundation/Log.h"
#include "platform/GamepadPollScheduler.h"

#include <chrono>
#include <cstdio>

namespace {

using namespace acs;

/**
 * 32ビット値に含まれる1ビットの数を返す。
 *
 * @param value 数えるビット列。
 * @return 立っているビットの数。
 */
constexpr u32 CountBits(u32 value) noexcept
{
    /** これまでに見つけた1ビットの数。 */
    u32 count = 0;
    while (value != 0) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

/** 全ポート未接続時の順番と再確認上限を検証する。 */
constexpr bool ValidateDisconnectedRoundRobin() noexcept
{
    /** 直前フレームに接続していたポート。 */
    bool connected[4]{};
    /** 検証対象のポーリング順序制御。 */
    detail::TGamepadPollScheduler<4> scheduler;
    return scheduler.BuildPollMask(connected) == 0x1u &&
           scheduler.BuildPollMask(connected) == 0x2u &&
           scheduler.BuildPollMask(connected) == 0x4u &&
           scheduler.BuildPollMask(connected) == 0x8u &&
           scheduler.BuildPollMask(connected) == 0x1u;
}

/** 接続中ポートが毎フレーム選ばれることを検証する。 */
constexpr bool ValidateConnectedFullRate() noexcept
{
    /** 直前フレームに接続していたポート。 */
    bool connected[4]{true, false, true, false};
    /** 検証対象のポーリング順序制御。 */
    detail::TGamepadPollScheduler<4> scheduler;
    /** 1フレーム目に取得するポート集合。 */
    const u32 first = scheduler.BuildPollMask(connected);
    /** 2フレーム目に取得するポート集合。 */
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
static_assert(detail::ClassifyLogDispatch("literal message") == detail::ELogDispatchKind::Literal);
static_assert(detail::ClassifyLogDispatch("value=%u") == detail::ELogDispatchKind::Formatted);
static_assert(CLogger::CompiledEnabled<ELogSeverity::Fatal>());

} // 無名名前空間

/** エンジン基盤の決定的な削減量をJSONで出力する。 */
int main()
{
    using namespace acs;
    /** 入力ポーリングの計測フレーム数。 */
    constexpr u64 kFrames = 1'000'000;
    /** 起床通知を集計する連続ログ件数。 */
    constexpr u64 kLoggerBurst = 4096;

    /** 全ポートを未接続にした基準状態。 */
    bool connected[4]{};
    /** 計測対象のポーリング順序制御。 */
    detail::TGamepadPollScheduler<4> scheduler;
    /** 最適化後に実行されるXInput相当呼び出し数。 */
    u64 optimized_poll_calls = 0;
    /** 計測中に一度でも確認したポート集合。 */
    u32 observed_ports = 0;

    /** ポーリング計測の開始時刻。 */
    const auto begin = std::chrono::steady_clock::now();
    /** ポーリングを進めるフレーム番号。 */
    for (u64 frame = 0; frame < kFrames; ++frame) {
        /** 今フレームに取得するポート集合。 */
        const u32 mask = scheduler.BuildPollMask(connected);
        optimized_poll_calls += CountBits(mask);
        observed_ports |= mask;
    }
    /** ポーリング計測の終了時刻。 */
    const auto end = std::chrono::steady_clock::now();

    /** 最適化後に送られる起床通知数。 */
    u64 optimized_wake_signals = 0;
    /** 公開するログレコード位置。 */
    for (u64 position = 0; position < kLoggerBurst; ++position) {
        if (detail::ShouldSignalLogConsumer(position, 0)) {
            ++optimized_wake_signals;
        }
    }

    /** 従来方式で実行されるXInput相当呼び出し数。 */
    const u64 baseline_poll_calls = kFrames * 4u;
    /** 従来方式で送られる起床通知数。 */
    const u64 baseline_wake_signals = kLoggerBurst;
    /** ポーリング処理の補助計測時間。合否には使わない。 */
    const u64 scheduler_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    /** 呼び出し数と全ポート巡回が契約どおりなら true。 */
    const bool polling_ok = optimized_poll_calls == kFrames && observed_ports == 0xFu;
    /** 起床通知数が契約どおりなら true。 */
    const bool logging_ok = optimized_wake_signals == 1u;
    /** 全ての決定的な契約が成立した場合は true。 */
    const bool ok = polling_ok && logging_ok;

    std::printf("{\n");
    std::printf("  \"schema\": 1,\n");
    std::printf("  \"status\": \"%s\",\n", ok ? "pass" : "fail");
    std::printf("  \"xinput\": {\"frames\": %llu, \"baseline_calls\": %llu, ", static_cast<unsigned long long>(kFrames), static_cast<unsigned long long>(baseline_poll_calls));
    std::printf("\"optimized_calls\": %llu, \"reduction_percent\": 75, ", static_cast<unsigned long long>(optimized_poll_calls));
    std::printf("\"reconnect_bound_frames\": 4},\n");
    std::printf("  \"logger\": {\"burst_records\": %llu, \"baseline_wake_signals\": %llu, ", static_cast<unsigned long long>(kLoggerBurst), static_cast<unsigned long long>(baseline_wake_signals));
    std::printf("\"optimized_wake_signals\": %llu},\n", static_cast<unsigned long long>(optimized_wake_signals));
    std::printf("  \"timing_diagnostic\": {\"scheduler_total_ns\": %llu, ", static_cast<unsigned long long>(scheduler_ns));
    std::printf("\"scheduler_ns_per_frame\": %.3f},\n", static_cast<double>(scheduler_ns) / static_cast<double>(kFrames));
    std::printf("  \"compiled_log_min_severity\": %d\n", ACS_COMPILED_LOG_MIN_SEVERITY);
    std::printf("}\n");
    return ok ? 0 : 1;
}
