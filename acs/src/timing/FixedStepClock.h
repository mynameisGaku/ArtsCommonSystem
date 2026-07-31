// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "timing/FixedStepAdvanceResult.h"
#include "timing/FixedStepClockSnapshot.h"

namespace acs::timing {

/** 固定更新に指定できる最小の刻み秒。 */
inline constexpr f64 kMinimumFixedStepSeconds = 1.0e-9;

/** 固定更新に指定できる最大の刻み秒。 */
inline constexpr f64 kMaximumFixedStepSeconds = 3600.0;

/** 一回の入力で実行できる固定更新の最大回数。 */
inline constexpr u32 kMaximumFixedStepsPerAdvance = 1000000u;

/** 一回の入力で蓄積できる最大秒数。 */
inline constexpr f64 kMaximumFixedStepAccumulatedSeconds = 86400.0;

/** 一括処理で受け付ける最大要素数。 */
inline constexpr u32 kMaximumFixedStepBatchCount = 4096u;

/**
 * 可変な経過秒を有界な固定更新回数へ変換する値。
 *
 * 呼び出し側が値として所有し、OS時刻、ゲームループ、共有サービスの寿命を持たない。
 */
class FFixedStepClock final {
public:
    /**
     * 設定を検証して適用し、補間位置と累積統計を初期化する。
     *
     * 範囲外または有限でない設定は拒否し、現在状態を変更しない。
     */
    bool Configure(FFixedStepOptions options) noexcept;

    /**
     * 設定を検証して適用し、補間率と累積統計を維持する。
     *
     * 範囲外の設定または現在状態の検証失敗では状態を変更しない。
     */
    bool TryReconfigurePreservingProgress(FFixedStepOptions options) noexcept;

    /** 現在適用されている設定値を返す。 */
    FFixedStepOptions Options() const noexcept;

    /**
     * 経過秒を受け付け、今回実行する固定更新回数と補間率を返す。
     *
     * 負値または有限でない入力は拒否し、現在状態を変更しない。
     */
    FFixedStepAdvanceResult Advance(f64 delta_seconds) noexcept;

    /**
     * 経過秒配列を単発処理と同じ順序でまとめて進める。
     *
     * 入力、容量、整列、領域重複を先に検証し、失敗時は時計、結果配列、件数を変更しない。
     */
    bool TryAdvanceBatch(const f64* delta_seconds, u32 count, FFixedStepAdvanceResult* results, u32 result_capacity, u32& result_count) noexcept;

    /**
     * 現在状態を指定先へ保存する。
     *
     * null、整列違反、時計との領域重複、現在状態の不正では保存先を変更しない。
     */
    bool TryCaptureSnapshot(FFixedStepClockSnapshot* snapshot) const noexcept;

    /** 現在状態を参照で指定した保存先へ保存する。 */
    bool TryCaptureSnapshot(FFixedStepClockSnapshot& snapshot) const noexcept { return TryCaptureSnapshot(&snapshot); }

    /**
     * 検証済みの保存値を現在状態へ復元する。
     *
     * null、整列違反、時計との領域重複、不正内容では現在状態を変更しない。
     */
    bool TryRestoreSnapshot(const FFixedStepClockSnapshot* snapshot) noexcept;

    /** 参照で指定した保存値を現在状態へ復元する。 */
    bool TryRestoreSnapshot(const FFixedStepClockSnapshot& snapshot) noexcept { return TryRestoreSnapshot(&snapshot); }

    /** 設定を維持し、補間位置と累積統計を初期化する。 */
    void Reset() noexcept;

    /** 次の固定更新へ持ち越している経過秒を返す。 */
    f64 AccumulatedSeconds() const noexcept;

    /** 次の固定更新までの進み具合を0以上1未満で返す。 */
    f64 InterpolationAlpha() const noexcept;

    /** 時計が確定した固定更新の累積回数を返す。 */
    u64 TotalStepCount() const noexcept;

    /** 上限によって破棄した累積秒数を返す。 */
    f64 TotalDroppedSeconds() const noexcept;

private:
    /** 現在適用されている固定更新設定。 */
    FFixedStepOptions m_Options{};

    /** 次の固定更新へ持ち越す経過秒。 */
    f64 m_AccumulatedSeconds = 0.0;

    /** 上限によって破棄した累積秒数。 */
    f64 m_TotalDroppedSeconds = 0.0;

    /** 時計が確定した固定更新の累積回数。 */
    u64 m_TotalStepCount = 0u;
};

} // namespace acs::timing
