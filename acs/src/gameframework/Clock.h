// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * AScene 単位の時間トラッカ (scaled / unscaled 時間と frame count を持つ軽量値型)。
 *
 * @details
 * scaled 時間は time_scale と pause を反映する「シーンが感じる論理時間」、unscaled
 * 時間は常に進む実時間。pause / resume / per-clock time_scale を持ち、CTweenManager や
 * FSequence、カスタムタイマーが共通の時間軸として参照する。CApplication::DeltaTime()
 * の実時間とは別に、slow-mo・pause・倍速モードを反映した時間を提供する。
 */
class CSceneClock {
public:
    /** 既定状態 (time_scale=1, 非 pause, 時刻 0) で構築する。 */
    CSceneClock() noexcept = default;

    /**
     * 1 フレーム分の dt を流して時刻を進める。
     *
     * @details
     * unscaled 系は常に dt だけ進める。scaled 系は pause 中なら 0、それ以外は
     * dt * time_scale を加算する。frame count も毎回 1 増やす。
     * @param dt このフレームの実経過秒。
     */
    void Tick(f32 dt) noexcept {
        m_LastDtUnscaled = dt;
        const f32 scaled = m_Paused ? 0.0f : dt * m_TimeScale;
        m_LastDt        = scaled;
        m_Time           += scaled;
        m_TimeUnscaled  += dt;
        ++m_FrameCount;
    }

    /** クロックを一時停止する (以降 scaled 系は進まない)。 */
    void Pause()  noexcept { m_Paused = true;  }

    /** 一時停止を解除する (scaled 系の進行を再開)。 */
    void Resume() noexcept { m_Paused = false; }

    /**
     * 一時停止中かを返す。
     *
     * @return pause 中なら true。
     */
    bool IsPaused() const noexcept { return m_Paused; }

    /**
     * 直近フレームの scaled デルタ秒を返す。
     *
     * @return 直近 Tick の dt * time_scale (pause 中は 0)。
     */
    f32 Dt()           const noexcept { return m_LastDt; }

    /**
     * 直近フレームの unscaled デルタ秒を返す。
     *
     * @return 直近 Tick の実経過秒。
     */
    f32 DtUnscaled()   const noexcept { return m_LastDtUnscaled; }

    /**
     * 累積 scaled 時間を返す。
     *
     * @return time_scale / pause を反映した累積秒。
     */
    f32 Time()         const noexcept { return m_Time; }

    /**
     * 累積 unscaled 時間を返す。
     *
     * @return 実時間ベースの累積秒。
     */
    f32 TimeUnscaled() const noexcept { return m_TimeUnscaled; }

    /**
     * Tick が呼ばれた累計フレーム数を返す。
     *
     * @return 累計フレーム数。
     */
    u64 Frame()        const noexcept { return m_FrameCount; }

    /**
     * 時間倍率を設定する。
     *
     * @details 0 でフリーズ (pause と同等)、1 が通常、>1 で早送り、<1 で slow-mo。負値は 0 にクランプ。
     * @param s 設定する時間倍率。
     */
    void SetTimeScale(f32 s) noexcept { m_TimeScale = s < 0.0f ? 0.0f : s; }

    /**
     * 現在の時間倍率を返す。
     *
     * @return 設定済みの time_scale。
     */
    f32  TimeScale() const noexcept { return m_TimeScale; }

    /**
     * 時間積算と frame count をリセットする (pause / time_scale は維持)。
     *
     * @details m_Time / m_TimeUnscaled / 直近 dt / frame count を 0 に戻す。
     */
    void Reset() noexcept {
        m_Time = 0.0f;
        m_TimeUnscaled = 0.0f;
        m_LastDt = 0.0f;
        m_LastDtUnscaled = 0.0f;
        m_FrameCount = 0;
    }

private:
    /** 累積 scaled 時間 (秒)。 */
    f32 m_Time             = 0.0f;

    /** 累積 unscaled 時間 (秒)。 */
    f32 m_TimeUnscaled    = 0.0f;

    /** 直近フレームの scaled デルタ秒。 */
    f32 m_LastDt          = 0.0f;

    /** 直近フレームの unscaled デルタ秒。 */
    f32 m_LastDtUnscaled = 0.0f;

    /** 時間倍率 (1.0 が通常速度)。 */
    f32 m_TimeScale       = 1.0f;

    /** Tick の累計フレーム数。 */
    u64 m_FrameCount      = 0;

    /** 一時停止フラグ (true で scaled 系が止まる)。 */
    bool m_Paused          = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSceneClock = CSceneClock;

} // namespace acs::game
