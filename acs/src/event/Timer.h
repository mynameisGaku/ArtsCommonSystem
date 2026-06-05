// SPDX-License-Identifier: Apache-2.0
// FTimerManager — フレーム単位の遅延 / 周期タイマー
//
// 使い方:
//   struct MyState { World* w; EntityId e; };
//   static void OnFire(void* user) {
//       auto* s = static_cast<MyState*>(user);
//       // ...
//   }
//
//   FTimerManager timers;
//   MyState st { &world, enemy };
//   FTimerHandle h = timers.SetTimeout(2.5f, &OnFire, &st);
//
//   // フレームループで:
//   timers.Tick(dt);
//
//   // 任意のキャンセル
//   timers.Cancel(h);
//
// 設計:
//   ・コールバックは関数ポインタ + void* user (STL 不依存方針に合わせる)
//   ・Handle は世代付き (Cancel 後に再利用された ID で誤発火しない)
//   ・SetInterval は self-rearm 方式 (発火後に次の period_seconds まで再カウント)
//   ・全タイマは線形配列で持つ (1000 個程度なら Tick の O(N) は問題ない)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs {

/**
 * タイマを一意に指すハンドル (世代付き slot id)。
 *
 * @details
 * id は 1-based で 0 は無効。slot 再利用を generation で見分けるため、Cancel または発火で
 * 解放済みのハンドルで誤って再利用後の別タイマを操作することを防ぐ。
 */
struct FTimerHandle {
    /** slot の 1-based id (0 は無効)。 */
    u32 id         = 0;

    /** 同 id の再利用を見分ける世代番号。 */
    u32 generation = 0;

    /**
     * ハンドルが有効値かを返す。
     *
     * @return id が 0 でなければ true。
     */
    bool IsValid() const noexcept { return id != 0; }

    /**
     * id と generation の両方が一致するかを返す。
     *
     * @param o 比較対象のハンドル。
     * @return 完全一致すれば true。
     */
    bool operator==(const FTimerHandle& o) const noexcept {
        return id == o.id && generation == o.generation;
    }
};

/** 無効な FTimerHandle (戻り値や初期値で使う番兵)。 */
inline constexpr FTimerHandle kInvalidTimer{};

/** タイマコールバック型 (user はユーザーデータ)。 */
using TimerCallback = void (*)(void* user);

/**
 * フレーム単位の遅延 / 周期タイマーを管理するマネージャ。
 *
 * @details
 * 全タイマを線形配列の slot で持ち、Tick で dt を減算して発火させる。SetTimeout は
 * 1 回限り、SetInterval は self-rearm 方式で周期発火する。slot は世代付きで再利用し、
 * 古いハンドルでの誤操作を防ぐ。コールバック中の追加・Cancel は安全。non-copy 型。
 */
class FTimerManager {
public:
    /** 空のマネージャを構築する。 */
    FTimerManager() noexcept = default;

    /** マネージャを破棄する。 */
    ~FTimerManager() noexcept = default;

    /** コピー禁止 (slot 配列と id 採番状態を抱えるため)。 */
    FTimerManager(const FTimerManager&) = delete;

    /** コピー代入も禁止。 */
    FTimerManager& operator=(const FTimerManager&) = delete;

    /**
     * delay_seconds 経過後に 1 回だけ呼ぶタイマを登録する。
     *
     * @param delay_seconds 発火までの秒 (負値は不正で kInvalidTimer を返す)。
     * @param cb 発火時に呼ぶコールバック。
     * @param user コールバックへ渡すユーザーデータ。
     * @return タイマハンドル (cb が null・delay が負なら kInvalidTimer)。
     */
    FTimerHandle SetTimeout(f32 delay_seconds, TimerCallback cb, void* user) noexcept;

    /**
     * period_seconds ごとに繰り返し呼ぶタイマを登録する。
     *
     * @param period_seconds 発火周期の秒 (0 以下は不正で kInvalidTimer を返す)。
     * @param cb 発火時に呼ぶコールバック。
     * @param user コールバックへ渡すユーザーデータ。
     * @return タイマハンドル (cb が null・period が 0 以下なら kInvalidTimer)。
     */
    FTimerHandle SetInterval(f32 period_seconds, TimerCallback cb, void* user) noexcept;

    /**
     * 指定タイマをキャンセルする。
     *
     * @param h キャンセルするタイマハンドル。
     * @return 該当タイマが見つかり解除できたら true (既に発火 / 解放済みなら false)。
     */
    bool Cancel(FTimerHandle h) noexcept;

    /**
     * 毎フレーム呼び、dt を経過させて発火条件を満たしたタイマを呼ぶ。
     *
     * @details
     * 周期タイマは dt が複数周期ぶんなら catch-up で複数回発火する (発火数には上限あり)。
     * コールバック中の新規タイマ追加・Cancel は安全。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 現在アクティブなタイマ数を返す (デバッグ用)。
     *
     * @return active な slot の数。
     */
    u32 ActiveCount() const noexcept;

private:
    /**
     * 1 タイマ slot (残時間・周期・コールバックを保持)。
     */
    struct Slot {
        /** slot の 1-based id (0 は未割り当て)。 */
        u32             id          = 0;

        /** slot 再利用を見分ける世代番号。 */
        u32             generation  = 0;

        /** この slot が現在有効かのフラグ。 */
        bool            active      = false;

        /** 周期タイマなら true (SetInterval)、1 回限りなら false (SetTimeout)。 */
        bool            repeating   = false;

        /** 次の発火までの残り秒 (0 以下で発火)。 */
        f32             remaining   = 0.0f;

        /** SetInterval の発火周期 (1 回限りなら 0)。 */
        f32             period      = 0.0f;

        /** 発火時に呼ぶコールバック。 */
        TimerCallback   cb          = nullptr;

        /** コールバックへ渡すユーザーデータ。 */
        void*           user        = nullptr;
    };

    /** タイマ slot の配列。 */
    TArray<Slot> m_Slots;

    /** 解放済みで再利用待ちの slot 添字。 */
    TArray<u32>  m_FreeIndices;

    /** 次に割り当てる 1-based slot id。 */
    u32         m_NextId = 1;
};

} // namespace acs
