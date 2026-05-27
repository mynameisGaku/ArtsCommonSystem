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

// Timer のハンドル (id=0 なら無効)
struct FTimerHandle {
    u32 id         = 0;   // 1-based、0 は無効
    u32 generation = 0;   // 同 id の再利用を見分ける

    bool IsValid() const noexcept { return id != 0; }
    bool operator==(const FTimerHandle& o) const noexcept {
        return id == o.id && generation == o.generation;
    }
};

inline constexpr FTimerHandle kInvalidTimer{};

// コールバック型 (FThreadPool::TaskFn と同じパターン)
using TimerCallback = void (*)(void* user);

class FTimerManager {
public:
    FTimerManager() noexcept = default;
    ~FTimerManager() noexcept = default;

    FTimerManager(const FTimerManager&) = delete;
    FTimerManager& operator=(const FTimerManager&) = delete;

    // delay_seconds 後に 1 回だけ呼ぶ
    FTimerHandle SetTimeout(f32 delay_seconds, TimerCallback cb, void* user) noexcept;

    // period_seconds 経つたびに繰り返し呼ぶ
    FTimerHandle SetInterval(f32 period_seconds, TimerCallback cb, void* user) noexcept;

    // 指定タイマをキャンセル (既に発火 / 解放済みなら false)
    bool Cancel(FTimerHandle h) noexcept;

    // 毎フレーム呼ぶ。dt 経過させ、発火条件を満たしたタイマを呼び出す。
    // コールバック中に新規タイマを追加 / Cancel するのは安全。
    void Tick(f32 dt) noexcept;

    // 現在のアクティブタイマ数 (デバッグ用)
    u32 ActiveCount() const noexcept;

private:
    struct Slot {
        u32             id          = 0;       // 1-based
        u32             generation  = 0;
        bool            active      = false;
        bool            repeating   = false;   // true = SetInterval、false = SetTimeout
        f32             remaining   = 0.0f;    // 0 になったら発火
        f32             period      = 0.0f;    // SetInterval の周期
        TimerCallback   cb          = nullptr;
        void*           user        = nullptr;
    };

    TArray<Slot> _slots;
    TArray<u32>  _free_indices;  // 1-based id を再利用するための空きスロット
    u32         _next_id = 1;
};

} // namespace acs
