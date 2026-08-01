// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "gameframework/SceneTimerHandle.h"

namespace acs::game {

/** タイマー発火時に呼ぶ C 関数ポインタ型 (user pointer を 1 つ受け取る)。 */
using TimerCallback = void(*)(void* user) noexcept;

/**
 * 呼び出し側が所有し、シーン寿命で遅延コールバックを管理する。
 *
 * @details 所有するシーンが Tick を呼び、破棄時は登録済みタイマーも破棄する。
 */
class CSceneTimer {
public:
    /** 空のタイマー管理を構築する。 */
    CSceneTimer() noexcept = default;

    /** 破棄する (active timer は呼ばずに捨てる)。 */
    ~CSceneTimer() noexcept = default;

    /** コピー禁止 (発火中の self 参照との競合を防ぐため)。 */
    CSceneTimer(const CSceneTimer&)            = delete;

    /** コピー代入も禁止。 */
    CSceneTimer& operator=(const CSceneTimer&) = delete;

    /** ムーブ禁止。 */
    CSceneTimer(CSceneTimer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSceneTimer& operator=(CSceneTimer&&)      = delete;

    /**
     * delay_sec 後に cb(user) を 1 回だけ実行するタイマーを登録する。
     *
     * @param delay_sec 発火までの遅延秒。0 以下は invalid handle を返す (即時実行しない)。
     * @param cb 発火時に呼ぶコールバック。nullptr なら invalid handle を返す。
     * @param user cb に渡すコンテキスト (この管理は所有しない)。
     * @return 登録したタイマーの handle (不正引数なら invalid)。
     */
    FSceneTimerHandle SetTimeout(f32 delay_sec, TimerCallback cb, void* user) noexcept;

    /**
     * period_sec ごとに cb(user) を繰り返し実行するタイマーを登録する。
     *
     * @details Cancel するまで永続する。
     * @param period_sec 発火周期の秒。0 以下は invalid handle を返す。
     * @param cb 発火時に呼ぶコールバック。nullptr なら invalid handle を返す。
     * @param user cb に渡すコンテキスト (この管理は所有しない)。
     * @return 登録したタイマーの handle (不正引数なら invalid)。
     */
    FSceneTimerHandle SetInterval(f32 period_sec, TimerCallback cb, void* user) noexcept;

    /**
     * 指定 handle のタイマーを停止する。
     *
     * @param h 停止するタイマーの handle。
     * @return active を停止できたら true。stale または既に完了済みなら false。
     */
    bool Cancel(FSceneTimerHandle h) noexcept;

    /** 全 active timer を停止する (コールバックは呼ばない)。 */
    void CancelAll() noexcept;

    /**
     * 指定 handle のタイマーが現在 active かを返す。
     *
     * @param h 調べるタイマーの handle。
     * @return active なら true (stale / 完了済みなら false)。
     */
    bool IsActive(FSceneTimerHandle h) const noexcept;

    /**
     * 現在 active なタイマーの数を返す。
     *
     * @return active timer 数。
     */
    u32  ActiveCount() const noexcept { return m_ActiveCount; }

    /**
     * 毎フレーム呼んで経過時間を進め、満了したタイマーを発火する。
     *
     * @details
     * dt < 0 は無視 (0 も何もしない)。大 dt のとき Interval は elapsed - period を
     * carry して同 Tick 内で複数回発火し得る。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

private:
    /** 1 タイマーぶんの内部状態。 */
    struct FTimerEntry {
        /** 発火時に呼ぶコールバック。 */
        TimerCallback cb        = nullptr;

        /** cb に渡すコンテキスト。 */
        void*         user      = nullptr;

        /** 前回発火 (または登録) からの経過秒。 */
        f32           elapsed   = 0.0f;

        /** 発火までの遅延 / 発火周期の秒。 */
        f32           period    = 0.0f;

        /** Interval なら true、Timeout (一度きり) なら false。 */
        bool          repeating = false;

        /** スロットが使用中なら true。 */
        bool          active    = false;

        /** スロットの generation (0 = 未使用、Acquire で必ず 1 以上にする)。 */
        u8            gen       = 0u;
    };

    /**
     * 空きスロットを確保 (なければ末尾に追加) して index を返す。
     *
     * @return 確保したスロットの index。
     */
    u32         AcquireSlot() noexcept;

    /**
     * index と generation から handle を組み立てる。
     *
     * @param index スロット index。
     * @param gen スロットの generation。
     * @return 対応する FSceneTimerHandle。
     */
    FSceneTimerHandle MakeHandle(u32 index, u8 gen) const noexcept;

    /** タイマースロット配列 (active/inactive 混在)。 */
    TArray<FTimerEntry> m_Entries;

    /** 現在 active なタイマーの数。 */
    u32               m_ActiveCount = 0u;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSceneTimer = CSceneTimer;

} // namespace acs::game
