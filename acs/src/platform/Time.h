// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * 高精度な時刻取得ユーティリティ (全メソッド static)。
 *
 * @details
 * 初回参照時を起点とする単調増加時刻を返し、フレーム間の経過時間計測に使う。
 */
class CClock {
public:
    /**
     * 起動 (初回参照) からの経過秒を返す。
     *
     * @return 経過秒 (高精度、単調増加)。
     */
    static f64 SecondsSinceStartup() noexcept;

    /**
     * 起動 (初回参照) からの経過ミリ秒を返す。
     *
     * @return 経過ミリ秒。
     */
    static u64 MillisSinceStartup() noexcept;

    /**
     * 内部クロックの生 tick 値を返す。
     *
     * @details 1 tick の長さは steady_clock の period 依存 (ナノ秒など)。TicksPerSecond と
     * 組み合わせて秒へ換算する。
     * @return time_since_epoch の生 tick カウント。
     */
    static u64 Ticks() noexcept;

    /**
     * 1 秒あたりの tick 数を返す。
     *
     * @return steady_clock の period から求めた 1 秒あたりの tick 数。
     */
    static u64 TicksPerSecond() noexcept;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FClock = CClock;

/**
 * フレーム時間を計測するタイマ (毎フレーム Tick を呼ぶ)。
 *
 * @details 前回 Tick からの経過秒を返しつつ、平滑化 dt・累積秒・累積フレーム数を更新する。
 */
class FFrameTimer {
public:
    /** 現在時刻を基準点にしてタイマを初期化する。 */
    FFrameTimer() noexcept;

    /**
     * 前回呼び出しからの経過秒を返し、内部状態を更新する。
     *
     * @details
     * dt は [0.0, 0.25] 秒にクランプする (ブレークポイント等で長時間停止した後の暴走防止)。
     * 併せて指数移動平均で平滑化 dt を更新し、累積秒とフレーム数を進める。
     * @return クランプ後の経過秒。
     */
    f32 Tick() noexcept;

    /**
     * 平滑化された 1 フレームあたりの dt を返す。
     *
     * @return 指数移動平均で平滑化した dt (秒)。
     */
    f32 SmoothedDelta() const noexcept { return m_Smoothed; }

    /**
     * 平滑化 dt から求めた平均 FPS を返す。
     *
     * @return 平均 FPS (平滑化 dt が 0 のときは 0)。
     */
    f32 SmoothedFPS()   const noexcept { return m_Smoothed > 0 ? 1.0f / m_Smoothed : 0; }

    /**
     * これまでに計測した累積フレーム数を返す。
     *
     * @return Tick を呼んだ累積回数。
     */
    u64 FrameCount()    const noexcept { return m_Frames; }

    /**
     * クランプ後 dt を積算した累積秒を返す。
     *
     * @return 累積経過秒。
     */
    f64 TotalSeconds()  const noexcept { return m_Total; }

private:
    /** 前回 Tick 時の生 tick 値。 */
    u64 m_LastTicks = 0;

    /** 平滑化された dt (秒)。 */
    f32 m_Smoothed   = 0.0f;

    /** クランプ後 dt の累積秒。 */
    f64 m_Total      = 0.0;

    /** Tick を呼んだ累積フレーム数。 */
    u64 m_Frames     = 0;
};

} // namespace acs
