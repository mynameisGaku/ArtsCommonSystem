// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FStateMachine<Owner> (Phase 4)
//
// 小さな汎用 FSM (有限状態機械)。AI / ゲームフロー / アニメーション制御等に。
//   ・状態は u32 ID で識別 (連続値を想定、最大 kMaxStates=16)
//   ・状態ごとに `on_enter` / `on_update(dt)` / `on_exit` を関数ポインタで登録
//     (`std::function` 不使用、ACS 規約)
//   ・テンプレート `<Owner>` で「所有者」型を持つ → 関数は `Owner&` を受け取る
//     (= scene/actor/component が自分自身を渡す典型パターン)
//
// 使い方:
//   class Enemy {
//   public:
//       enum States { Idle = 0, Chase, Attack };
//       acs::game::FStateMachine<Enemy> sm;
//
//       Enemy() noexcept {
//           sm.Configure(Idle,  { &Enemy::EnterIdle, &Enemy::UpdateIdle, nullptr });
//           sm.Configure(Chase, { nullptr, &Enemy::UpdateChase, nullptr });
//           sm.Start(Idle, *this);
//       }
//
//       void Tick(f32 dt) noexcept { sm.Update(*this, dt); }
//
//       static void EnterIdle (Enemy& e) noexcept            { ... }
//       static void UpdateIdle(Enemy& e, f32 dt) noexcept    { if (...) e.sm.ChangeState(Chase, e); }
//       static void UpdateChase(Enemy& e, f32 dt) noexcept   { ... }
//   };
//
// 注意:
//   ・`ChangeState` は **即時遷移** (OnExit 旧 → OnEnter 新 を同期で呼ぶ)。
//     OnUpdate 内から ChangeState すれば、その時点で旧の OnExit + 新の OnEnter
//     が走る。同フレーム中の再 ChangeState は許容するが、循環遷移には注意。
//   ・状態 ID が `kMaxStates` を超える / `Configure` していない ID で `Start` /
//     `ChangeState` した場合は静かに skip (assert なしのソフトフェイル)。
#pragma once

#include "foundation/Types.h"

namespace acs::game {

template<typename Owner>
class FStateMachine {
public:
    using StateFn = void(*)(Owner& owner, f32 dt) noexcept;
    using EnterFn = void(*)(Owner& owner) noexcept;
    using ExitFn  = void(*)(Owner& owner) noexcept;

    struct FState {
        EnterFn on_enter  = nullptr;
        StateFn on_update = nullptr;
        ExitFn  on_exit   = nullptr;
    };

    static constexpr u32 kMaxStates    = 16;
    static constexpr u32 kInvalidState = 0xFFFFFFFFu;

    FStateMachine() noexcept = default;

    FStateMachine(const FStateMachine&)            = delete;
    FStateMachine& operator=(const FStateMachine&) = delete;

    // 状態 ID に FState を登録。範囲外は no-op。
    void Configure(u32 state_id, FState state) noexcept {
        if (state_id >= kMaxStates) return;
        _states[state_id] = state;
    }

    // 初期状態に入る。OnEnter が呼ばれる。
    void Start(u32 initial_state, Owner& owner) noexcept {
        _current = initial_state;
        if (_current < kMaxStates && _states[_current].on_enter != nullptr) {
            _states[_current].on_enter(owner);
        }
    }

    // 即時遷移: 現状態の OnExit → 新状態の OnEnter を同期実行。
    void ChangeState(u32 new_state, Owner& owner) noexcept {
        if (_current < kMaxStates && _states[_current].on_exit != nullptr) {
            _states[_current].on_exit(owner);
        }
        _current = new_state;
        if (_current < kMaxStates && _states[_current].on_enter != nullptr) {
            _states[_current].on_enter(owner);
        }
    }

    // OnUpdate を発火。毎フレーム呼ぶ。
    void Update(Owner& owner, f32 dt) noexcept {
        if (_current < kMaxStates && _states[_current].on_update != nullptr) {
            _states[_current].on_update(owner, dt);
        }
    }

    u32  Current() const noexcept { return _current; }
    bool IsIn(u32 state_id) const noexcept { return _current == state_id; }

private:
    FState _states[kMaxStates] = {};
    u32   _current = kInvalidState;
};

} // namespace acs::game
