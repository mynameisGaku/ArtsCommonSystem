// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "event/SimpleDelegate.h"
#include "event/TimerDiagnostics.h"
#include "event/TimerHandle.h"
#include "event/TimerSchedulePolicy.h"
#include "foundation/Types.h"

#include <type_traits>

namespace acs {

/**
 * タイマー発火時に呼び出す関数。
 * @param user 登録時に指定した値。
 */
using TimerCallback = void (*)(void* user);

/**
 * フレーム単位の一回または周期タイマーを同じスレッド内で管理する。
 * Tickが戻るまでは、この管理器と呼出し対象の寿命を呼出し側で保つ必要がある。
 */
class CTimerManager {
public:
    /** 空のタイマー管理器を作る。 */
    CTimerManager() noexcept = default;

    /** タイマー管理器を破棄する。 */
    ~CTimerManager() noexcept = default;

    /** タイマーの重複所有を防ぐためコピー構築を禁止する。 */
    CTimerManager(const CTimerManager&) = delete;

    /** タイマーの重複所有を防ぐためコピー代入を禁止する。 */
    CTimerManager& operator=(const CTimerManager&) = delete;

    /**
     * 指定時間後に一度だけ関数を呼び出す。
     * 関数が空、秒数が負または有限値でない、保持領域を確保できない、または世代番号を使い切った場合は無効なハンドルを返す。
     * @param delay_seconds 呼び出すまでの秒数。
     * @param cb 呼び出す関数。
     * @param user 呼び出す関数へ渡す値。
     */
    FTimerHandle SetTimeout(f32 delay_seconds, TimerCallback cb, void* user) noexcept;

    /**
     * 指定時間後に一度だけデリゲートを呼び出す。
     * デリゲートが未設定の場合は無効なハンドルを返す。
     * @param delay_seconds 呼び出すまでの秒数。
     * @param delegate 呼び出すデリゲート。
     */
    FTimerHandle SetTimeout(f32 delay_seconds, FSimpleDelegate delegate) noexcept;

    /**
     * 指定周期で関数を繰り返し呼び出す。
     * 関数が空、秒数が0以下または有限値でない、保持領域を確保できない、または世代番号を使い切った場合は無効なハンドルを返す。
     * @param period_seconds 呼び出す周期の秒数。
     * @param cb 呼び出す関数。
     * @param user 呼び出す関数へ渡す値。
     */
    FTimerHandle SetInterval(f32 period_seconds, TimerCallback cb, void* user) noexcept;

    /**
     * 指定周期でデリゲートを繰り返し呼び出す。
     * デリゲートが未設定の場合は無効なハンドルを返す。
     * @param period_seconds 呼び出す周期の秒数。
     * @param delegate 呼び出すデリゲート。
     */
    FTimerHandle SetInterval(f32 period_seconds, FSimpleDelegate delegate) noexcept;

    /**
     * 発火方針と型付きコールバックをコンパイル時に確定して登録する。
     * @tparam Policy 一回または周期発火の方針。
     * @tparam Callback コンパイル時に確定する型付きコールバック。
     * @tparam User コールバックへ渡す値の型。
     * @param seconds 遅延または周期秒。
     * @param user コールバックへ渡す値。
     */
    template<ETimerSchedulePolicy Policy, auto Callback, typename User>
    FTimerHandle Schedule(f32 seconds, User* user) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<void, decltype(Callback), User*>, "型付きタイマーコールバックは void(User*) noexcept として呼べる必要があります");
        if constexpr (Policy == ETimerSchedulePolicy::Once) {
            return SetTimeout(seconds, &TypedTimerThunk<User, Callback>, user);
        } else {
            static_assert(Policy == ETimerSchedulePolicy::Repeating, "未対応のタイマー発火方針です");
            return SetInterval(seconds, &TypedTimerThunk<User, Callback>, user);
        }
    }

    /**
     * 指定したタイマーを取り消す。
     * @param handle 取り消すタイマーのハンドル。
     */
    bool Cancel(FTimerHandle handle) noexcept;

    /** 登録中のタイマーをすべて取り消す。 */
    void CancelAll() noexcept;

    /** 全タイマーを無効にして保持領域を空にする。 */
    void Clear() noexcept;

    /**
     * 指定したタイマーが現在も登録中かを返す。
     * @param handle 調べるタイマーのハンドル。
     */
    bool IsActive(FTimerHandle handle) const noexcept;

    /**
     * 時間を進めて発火条件を満たしたタイマーを呼び出す。
     * 秒数が負または有限値でない場合と再入呼出しでは状態を変更しない。
     * 呼出し中に登録したタイマーは次回更新から進める。
     * @param dt 前回から経過した秒数。
     */
    void Tick(f32 dt) noexcept;

    /** 現在有効なタイマー数を返す。 */
    u32 ActiveCount() const noexcept;

    /** 現在の決定的な走査診断値を返す。 */
    FTimerDiagnostics Diagnostics() const noexcept { return m_Diagnostics; }

    /** 走査診断値だけを0へ戻し、タイマー状態は変更しない。 */
    void ResetDiagnostics() noexcept { m_Diagnostics = {}; }

private:
    /** 一件分のタイマー情報。 */
    struct FSlot {
        /** タイマーの識別番号。 */
        u32 id = 0;
        /** 再利用された枠を見分ける世代番号。 */
        u32 generation = 0;
        /** 現在有効かを示す。 */
        bool active = false;
        /** 周期実行するかを示す。 */
        bool repeating = false;
        /** 現在の更新中に登録され、次回更新まで待つかを示す。 */
        bool pending_until_next_tick = false;
        /** 次回発火までの秒数。 */
        f32 remaining = 0.0f;
        /** 周期実行の間隔秒数。 */
        f32 period = 0.0f;
        /** 発火時に呼び出す関数。 */
        TimerCallback cb = nullptr;
        /** 呼び出す関数へ渡す値。 */
        void* user = nullptr;
    };

    /** 型付きコールバックを既存の TimerCallback へ接続する。 */
    template<typename User, auto Callback>
    static void TypedTimerThunk(void* user) noexcept {
        Callback(static_cast<User*>(user));
    }

    /** 次の登録に使うゼロ以外の識別番号を返す。 */
    u32 AcquireId() noexcept;

    /** 次の登録に使う世代番号を返し、使い切った場合は0を返す。 */
    u32 AcquireGeneration() noexcept;

    /** 新規タイマー枠を active bitset で表せるようにする。 */
    bool EnsureActiveWord(u32 slot_index) noexcept;

    /** 登録に使う空き枠を返し、確保できなければ無効な位置を返す。 */
    u32 AcquireSlotIndex() noexcept;

    /**
     * 検証済みの時間と処理をタイマー枠へ登録する。
     * @param duration_seconds 発火までの秒数または周期。
     * @param repeating 周期実行する場合はtrue。
     * @param cb 発火時に呼ぶ関数。
     * @param user 関数へ渡す値。
     */
    FTimerHandle RegisterTimer(f32 duration_seconds, bool repeating, TimerCallback cb, void* user) noexcept;

    /** タイマー枠の active bit を立てる。 */
    void MarkActive(u32 slot_index) noexcept;

    /** タイマー枠の active bit を下ろす。 */
    void MarkInactive(u32 slot_index) noexcept;

    /** 全タイマー枠を無効にする。 */
    void InvalidateAllSlots() noexcept;

    /** 全消去後の保持領域を解放する。 */
    void ReleaseClearedStorage() noexcept;

    /** 確保済みのタイマー枠。 */
    TArray<FSlot> m_Slots;
    /** 有効なタイマー枠だけを示す64-bit bitset。 */
    TArray<u64> m_ActiveWords;
    /** 再利用できるタイマー枠の位置。 */
    TArray<u32> m_FreeIndices;
    /** 次に割り当てるタイマー番号。 */
    u32 m_NextId = 1;
    /** 次に割り当てる世代番号。0は使い切り済みを示す。 */
    u32 m_NextGeneration = 1;
    /** 現在の更新呼出し深度。 */
    u32 m_TickDepth = 0;
    /** 更新終了後に保持領域を解放するかを示す。 */
    bool m_ClearPending = false;
    /** 現在有効なタイマー数。 */
    u32 m_ActiveCount = 0;
    /** 最適化経路の決定的な診断値。 */
    FTimerDiagnostics m_Diagnostics;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FTimerManager = CTimerManager;

} // namespace acs
