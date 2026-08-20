// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/FixedStepAdvanceResult.h"
#include "gameframework/FixedStepClockSnapshot.h"

namespace acs::game {

/** 固定 step として受理する最小秒数。 */
inline constexpr f64 kMinimumFixedStepSeconds = 1.0e-9;

/** 固定 step として受理する最大秒数。 */
inline constexpr f64 kMaximumFixedStepSeconds = 3600.0;

/** 1 回の Advance で返せる固定更新回数の絶対上限。 */
inline constexpr u32 kMaximumFixedStepsPerAdvance = 1000000u;

/** 時計へ取り込める遅延時間の絶対上限。 */
inline constexpr f64 kMaximumFixedStepAccumulatedSeconds = 86400.0;

/**
 * 可変deltaを有界な固定更新回数へ変換する、呼び出し側所有の値型。
 *
 * FGameの実行ループや実時間Clockは所有せず、任意の独立simulationで利用できる。
 * 検証に失敗した操作は現在状態を変更しない。
 */
class FFixedStepClock final {
public:
    /**
     * 固定 step と上限を検証して設定する。
     *
     * @param options 適用する固定時計設定。
     * @return 設定が有効で適用できた場合は true。不正時は現在状態を変更しない。
     */
    bool Configure(FFixedStepOptions options) noexcept;

    /** 現在の検証済み設定を値で返す。 */
    FFixedStepOptions Options() const noexcept { return m_Options; }

    /**
     * 可変 delta を今回実行する固定更新回数へ変換する。
     *
     * @param delta_seconds 取り込む 0 以上の有限秒数。
     * @return 実行回数、描画補間率、破棄時間、入力受理状態。
     */
    FFixedStepAdvanceResult Advance(f64 delta_seconds) noexcept;

    /**
     * 設定、剰余、累積統計を保存値へ複写する。
     *
     * @param snapshot 検証済み状態の出力先。
     * @return 現状態が保存条件を満たす場合は true。
     */
    bool TryCaptureSnapshot(FFixedStepClockSnapshot& snapshot) const noexcept;

    /**
     * 保存値を検証して時計全体へ復元する。
     *
     * @param snapshot 復元する設定、剰余、累積統計。
     * @return 復元できた場合は true。不正時は現在状態を変更しない。
     */
    bool TryRestoreSnapshot(const FFixedStepClockSnapshot& snapshot) noexcept;

    /** 現在設定を維持し、剰余と累積統計を初期化する。 */
    void Reset() noexcept;

    /** 次の固定 step へ繰り越す秒数を返す。 */
    f64 AccumulatedSeconds() const noexcept { return m_AccumulatedSeconds; }

    /** 次の固定 step までの描画補間率を返す。 */
    f64 InterpolationAlpha() const noexcept;

    /** 時計が返した固定更新回数の累計を返す。 */
    u64 TotalStepCount() const noexcept { return m_TotalStepCount; }

    /** 上限処理によって破棄した秒数の累計を返す。 */
    f64 TotalDroppedSeconds() const noexcept { return m_TotalDroppedSeconds; }

private:
    /** 検証済みの固定 step と上限。 */
    FFixedStepOptions m_Options{};

    /** 次の固定 step へ繰り越す 1 step 未満の秒数。 */
    f64 m_AccumulatedSeconds = 0.0;

    /** 上限処理によって破棄した秒数の累計。 */
    f64 m_TotalDroppedSeconds = 0.0;

    /** 時計が返した固定更新回数の累計。 */
    u64 m_TotalStepCount = 0u;
};

} // namespace acs::game
