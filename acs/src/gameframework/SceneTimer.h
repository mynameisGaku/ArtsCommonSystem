// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FSceneTimer (scene-scoped 遅延コールバック)
//
// シーンスコープの SetTimeout / SetInterval。FScene 死亡で自動破棄される点が
// 既存 `acs::FTimerManager` (event/Timer.h、グローバル寿命) との違い。各 FScene が
// 自分の FSceneTimer をメンバ保持し OnUpdate から Tick(dt) を呼ぶ想定。
//
// 使い方:
//   class FGameplayScene : public FScene {
//       acs::game::FSceneTimer m_Timers;
//       acs::game::FTimerHandle m_SpawnTimer;
//
//       void OnEnter() noexcept override {
//           m_SpawnTimer = m_Timers.SetInterval(2.0f, &FGameplayScene::SpawnEnemy, this);
//           m_Timers.SetTimeout(10.0f, &FGameplayScene::EndWave, this);
//       }
//       void OnUpdate(f32 dt) noexcept override { m_Timers.Tick(dt); }
//       void OnExit() noexcept override { m_Timers.CancelAll(); }
//
//       static void SpawnEnemy(void* self) noexcept { /* ... */ }
//       static void EndWave(void* self) noexcept    { /* ... */ }
//   };
//
// 設計:
//   ・コールバックは `void(*)(void*) noexcept` 関数ポインタ (std::function 不使用)。
//   ・Handle は 24bit index + 8bit gen の packed u32。stale 参照を検出可能。
//   ・delay/period <= 0 は invalid handle 返却 (即時実行はしない)。
//   ・cb == nullptr は invalid handle 返却。
//   ・Tick 内のコールバック発火順は登録順。1 Tick で複数回 fire する Interval は、
//     `elapsed - period` を carry して同 Tick 内で連続発火 (大 dt 対策)。
//   ・非コピー・非ムーブ。Tick 中の新規 SetTimeout/SetInterval は次 Tick から有効。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * タイマーを識別する packed handle (24bit index + 8bit generation)。
 *
 * @details
 * m_Packed == 0 を invalid と定義する (generation は active 化時に常に 1 以上に
 * なる)。slot 再利用後の stale 参照は generation 不一致で検出できる。
 */
struct FTimerHandle {
    /** index と generation を詰めた 32bit 値 (0 = invalid)。 */
    u32 m_Packed = 0u;

    /**
     * handle が有効値かを返す。
     *
     * @return m_Packed が非 0 なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0u; }

    /** index 部に割り当てるビット数。 */
    static constexpr u32 kIndexBits = 24u;

    /** index 部を取り出すマスク (0x00FFFFFF)。 */
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u; // 0x00FFFFFF

    /** index として表現できる最大値 (= 16777215 スロット)。 */
    static constexpr u32 kMaxIndex  = kIndexMask;              // 16777215 個

    /**
     * index と generation から handle を組み立てる。
     *
     * @param index スロット index (下位 24bit に格納、上位は切り捨て)。
     * @param gen generation (上位 8bit に格納)。
     * @return 詰めた FTimerHandle。
     */
    static FTimerHandle Pack(u32 index, u8 gen) noexcept {
        FTimerHandle h;
        h.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }

    /**
     * handle から index 部を取り出す。
     *
     * @return スロット index (下位 24bit)。
     */
    u32 Index() const noexcept { return m_Packed & kIndexMask; }

    /**
     * handle から generation 部を取り出す。
     *
     * @return generation (上位 8bit)。
     */
    u8  Gen()   const noexcept { return static_cast<u8>(m_Packed >> kIndexBits); }
};

/** タイマー発火時に呼ぶ C 関数ポインタ型 (user pointer を 1 つ受け取る)。 */
using TimerCallback = void(*)(void* user) noexcept;

/**
 * シーンスコープの SetTimeout / SetInterval を提供する遅延コールバック管理。
 *
 * @details
 * 各 FScene がメンバとして保持し OnUpdate から Tick(dt) を呼ぶ想定で、FScene 死亡で
 * 自動破棄される (グローバル寿命の FTimerManager との違い)。コールバックは
 * void(*)(void*) noexcept 関数ポインタで保持し、handle は 24bit index + 8bit gen
 * の packed u32 で stale 参照を検出可能。発火中の self 参照との競合を避けるため
 * 非コピー・非ムーブ。
 */
class FSceneTimer {
public:
    /** 空のタイマー管理を構築する。 */
    FSceneTimer() noexcept = default;

    /** 破棄する (active timer は呼ばずに捨てる)。 */
    ~FSceneTimer() noexcept = default;

    /** コピー禁止 (発火中の self 参照との競合を防ぐため)。 */
    FSceneTimer(const FSceneTimer&)            = delete;

    /** コピー代入も禁止。 */
    FSceneTimer& operator=(const FSceneTimer&) = delete;

    /** ムーブ禁止。 */
    FSceneTimer(FSceneTimer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FSceneTimer& operator=(FSceneTimer&&)      = delete;

    /**
     * delay_sec 後に cb(user) を 1 回だけ実行するタイマーを登録する。
     *
     * @param delay_sec 発火までの遅延秒。0 以下は invalid handle を返す (即時実行しない)。
     * @param cb 発火時に呼ぶコールバック。nullptr なら invalid handle を返す。
     * @param user cb に渡すコンテキスト (この管理は所有しない)。
     * @return 登録したタイマーの handle (不正引数なら invalid)。
     */
    FTimerHandle SetTimeout(f32 delay_sec, TimerCallback cb, void* user) noexcept;

    /**
     * period_sec ごとに cb(user) を繰り返し実行するタイマーを登録する。
     *
     * @details Cancel するまで永続する。
     * @param period_sec 発火周期の秒。0 以下は invalid handle を返す。
     * @param cb 発火時に呼ぶコールバック。nullptr なら invalid handle を返す。
     * @param user cb に渡すコンテキスト (この管理は所有しない)。
     * @return 登録したタイマーの handle (不正引数なら invalid)。
     */
    FTimerHandle SetInterval(f32 period_sec, TimerCallback cb, void* user) noexcept;

    /**
     * 指定 handle のタイマーを停止する。
     *
     * @param h 停止するタイマーの handle。
     * @return active を停止できたら true。stale または既に完了済みなら false。
     */
    bool Cancel(FTimerHandle h) noexcept;

    /** 全 active timer を停止する (コールバックは呼ばない)。 */
    void CancelAll() noexcept;

    /**
     * 指定 handle のタイマーが現在 active かを返す。
     *
     * @param h 調べるタイマーの handle。
     * @return active なら true (stale / 完了済みなら false)。
     */
    bool IsActive(FTimerHandle h) const noexcept;

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
     * @return 対応する FTimerHandle。
     */
    FTimerHandle MakeHandle(u32 index, u8 gen) const noexcept;

    /** タイマースロット配列 (active/inactive 混在)。 */
    TArray<FTimerEntry> m_Entries;

    /** 現在 active なタイマーの数。 */
    u32               m_ActiveCount = 0u;
};

} // namespace acs::game
