// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — SceneTimer (scene-scoped 遅延コールバック)
//
// シーンスコープの SetTimeout / SetInterval。Scene 死亡で自動破棄される点が
// 既存 `acs::TimerManager` (event/Timer.h、グローバル寿命) との違い。各 Scene が
// 自分の SceneTimer をメンバ保持し OnUpdate から Tick(dt) を呼ぶ想定。
//
// 使い方:
//   class GameplayScene : public Scene {
//       acs::game::SceneTimer _timers;
//       acs::game::TimerHandle _spawn_timer;
//
//       void OnEnter() noexcept override {
//           _spawn_timer = _timers.SetInterval(2.0f, &GameplayScene::SpawnEnemy, this);
//           _timers.SetTimeout(10.0f, &GameplayScene::EndWave, this);
//       }
//       void OnUpdate(f32 dt) noexcept override { _timers.Tick(dt); }
//       void OnExit() noexcept override { _timers.CancelAll(); }
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

// 24bit index + 8bit generation を packed した handle。
// _packed == 0 を invalid と定義 (gen は常に 1 以上)。
struct TimerHandle {
    u32 _packed = 0u;

    bool IsValid() const noexcept { return _packed != 0u; }

    // pack/unpack ヘルパ (manager 内部用、誤用防止に static で明示)。
    static constexpr u32 kIndexBits = 24u;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u; // 0x00FFFFFF
    static constexpr u32 kMaxIndex  = kIndexMask;              // 16777215 個

    static TimerHandle Pack(u32 index, u8 gen) noexcept {
        TimerHandle h;
        h._packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }
    u32 Index() const noexcept { return _packed & kIndexMask; }
    u8  Gen()   const noexcept { return static_cast<u8>(_packed >> kIndexBits); }
};

using TimerCallback = void(*)(void* user) noexcept;

class SceneTimer {
public:
    SceneTimer() noexcept = default;
    ~SceneTimer() noexcept = default;

    // 非コピー・非ムーブ (発火中の self 参照との競合を防ぐ)
    SceneTimer(const SceneTimer&)            = delete;
    SceneTimer& operator=(const SceneTimer&) = delete;
    SceneTimer(SceneTimer&&)                 = delete;
    SceneTimer& operator=(SceneTimer&&)      = delete;

    // delay_sec 後に cb(user) を 1 回だけ実行。
    // delay_sec <= 0 or cb == nullptr は invalid handle を返す (即時実行はしない)。
    TimerHandle SetTimeout(f32 delay_sec, TimerCallback cb, void* user) noexcept;

    // period_sec ごとに cb(user) を繰り返し実行 (Cancel まで永続)。
    // period_sec <= 0 or cb == nullptr は invalid handle を返す。
    TimerHandle SetInterval(f32 period_sec, TimerCallback cb, void* user) noexcept;

    // h が active なら停止して true を返す。stale or 既に完了は false。
    bool Cancel(TimerHandle h) noexcept;

    // 全 active timer を停止 (callback は呼ばない)。
    void CancelAll() noexcept;

    bool IsActive(TimerHandle h) const noexcept;
    u32  ActiveCount() const noexcept { return _active_count; }

    // 毎フレーム呼ぶ。dt < 0 は無視 (= 0 は何もしない)。
    // 大 dt のとき Interval は同 Tick 内で複数回発火し得る (carry 方式)。
    void Tick(f32 dt) noexcept;

private:
    struct TimerEntry {
        TimerCallback cb        = nullptr;
        void*         user      = nullptr;
        f32           elapsed   = 0.0f;
        f32           period    = 0.0f;
        bool          repeating = false;
        bool          active    = false;
        u8            gen       = 0u; // 0 = 未使用。Acquire で必ず 1 以上にする
    };

    u32         AcquireSlot() noexcept;
    TimerHandle MakeHandle(u32 index, u8 gen) const noexcept;

    TArray<TimerEntry> _entries;
    u32               _active_count = 0u;
};

} // namespace acs::game
