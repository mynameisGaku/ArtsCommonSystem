// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — TStateMachine<Owner>
//
// 小さな汎用 FSM (有限状態機械)。AI / ゲームフロー / アニメーション制御等に。
//   ・状態は u32 ID で識別 (連続値を想定、最大 kMaxStates=16)
//   ・状態ごとに `on_enter` / `on_update(dt)` / `on_exit` を関数ポインタで登録
//     (`std::function` 不使用、ACS 規約)
//   ・テンプレート `<Owner>` で「所有者」型を持つ → 関数は `Owner&` を受け取る
//     (= scene/actor/component が自分自身を渡す典型パターン)
//
// 使い方:
//   class FEnemy {
//   public:
//       enum EStates { Idle = 0, Chase, Attack };
//       acs::game::TStateMachine<FEnemy> sm;
//
//       FEnemy() noexcept {
//           sm.Configure(Idle,  { &FEnemy::EnterIdle, &FEnemy::UpdateIdle, nullptr });
//           sm.Configure(Chase, { nullptr, &FEnemy::UpdateChase, nullptr });
//           sm.Start(Idle, *this);
//       }
//
//       void Tick(f32 dt) noexcept { sm.Update(*this, dt); }
//
//       static void EnterIdle (FEnemy& e) noexcept            { ... }
//       static void UpdateIdle(FEnemy& e, f32 dt) noexcept    { if (...) e.sm.ChangeState(Chase, e); }
//       static void UpdateChase(FEnemy& e, f32 dt) noexcept   { ... }
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

/**
 * 所有者型 Owner を持つ小さな汎用有限状態機械 (FSM)。
 *
 * @details
 * 状態は u32 ID で識別し (最大 kMaxStates=16)、状態ごとに on_enter / on_update /
 * on_exit を関数ポインタ (std::function 不使用) で登録する。各コールバックは
 * Owner& を受け取るため、scene/actor/component が自分自身を渡す使い方を想定する。
 * ChangeState は即時遷移 (旧 on_exit → 新 on_enter を同期で呼ぶ)。範囲外 ID や
 * 未 Configure の ID は静かに skip するソフトフェイル設計。non-copy。
 * @tparam Owner コールバックに渡される所有者型。
 */
template<typename Owner>
class TStateMachine {
public:
    /** on_update コールバックの関数ポインタ型 (Owner& と経過秒 dt を受け取る)。 */
    using StateFn = void(*)(Owner& owner, f32 dt) noexcept;

    /** on_enter コールバックの関数ポインタ型 (Owner& を受け取る)。 */
    using EnterFn = void(*)(Owner& owner) noexcept;

    /** on_exit コールバックの関数ポインタ型 (Owner& を受け取る)。 */
    using ExitFn  = void(*)(Owner& owner) noexcept;

    /**
     * 1 状態分のコールバック束。
     *
     * @details いずれの関数ポインタも nullptr 可 (その遷移点では何もしない)。
     */
    struct FState {
        /** 状態に入ったとき 1 回呼ばれる関数 (nullptr 可)。 */
        EnterFn on_enter  = nullptr;

        /** 毎フレーム呼ばれる更新関数 (nullptr 可)。 */
        StateFn on_update = nullptr;

        /** 状態から出るとき 1 回呼ばれる関数 (nullptr 可)。 */
        ExitFn  on_exit   = nullptr;
    };

    /** 登録可能な状態数の上限 (これ以上の ID は no-op になる)。 */
    static constexpr u32 kMaxStates    = 16;

    /** 「未開始」を表す無効状態 ID (Start 前の m_Current 初期値)。 */
    static constexpr u32 kInvalidState = 0xFFFFFFFFu;

    /** 空の状態機械を構築する (どの状態にも入っていない)。 */
    TStateMachine() noexcept = default;

    /** コピー禁止 (状態機械は単一所有を想定)。 */
    TStateMachine(const TStateMachine&)            = delete;

    /** コピー代入も禁止。 */
    TStateMachine& operator=(const TStateMachine&) = delete;

    /**
     * 状態 ID に FState を登録する。
     *
     * @details state_id が kMaxStates 以上なら no-op (静かに無視)。
     * @param state_id 登録先の状態 ID。
     * @param state 登録するコールバック束。
     */
    void Configure(u32 state_id, FState state) noexcept {
        if (state_id >= kMaxStates) return;
        _states[state_id] = state;
    }

    /**
     * 初期状態に入る (その状態の on_enter を呼ぶ)。
     *
     * @param initial_state 開始する状態 ID。
     * @param owner コールバックに渡す所有者。
     */
    void Start(u32 initial_state, Owner& owner) noexcept {
        m_Current = initial_state;
        if (m_Current < kMaxStates && _states[m_Current].on_enter != nullptr) {
            _states[m_Current].on_enter(owner);
        }
    }

    /**
     * 即時遷移する (現状態の on_exit → 新状態の on_enter を同期実行)。
     *
     * @param new_state 遷移先の状態 ID。
     * @param owner コールバックに渡す所有者。
     */
    void ChangeState(u32 new_state, Owner& owner) noexcept {
        if (m_Current < kMaxStates && _states[m_Current].on_exit != nullptr) {
            _states[m_Current].on_exit(owner);
        }
        m_Current = new_state;
        if (m_Current < kMaxStates && _states[m_Current].on_enter != nullptr) {
            _states[m_Current].on_enter(owner);
        }
    }

    /**
     * 現状態の on_update を発火する (毎フレーム呼ぶ)。
     *
     * @param owner コールバックに渡す所有者。
     * @param dt 前フレームからの経過秒。
     */
    void Update(Owner& owner, f32 dt) noexcept {
        if (m_Current < kMaxStates && _states[m_Current].on_update != nullptr) {
            _states[m_Current].on_update(owner, dt);
        }
    }

    /**
     * 現在の状態 ID を返す。
     *
     * @return 現在の状態 ID (未開始なら kInvalidState)。
     */
    u32  Current() const noexcept { return m_Current; }

    /**
     * 指定 ID が現在の状態かを返す。
     *
     * @param state_id 比較する状態 ID。
     * @return 現在の状態が state_id なら true。
     */
    bool IsIn(u32 state_id) const noexcept { return m_Current == state_id; }

private:
    /** 状態 ID をインデックスとするコールバック束テーブル。 */
    FState _states[kMaxStates] = {};

    /** 現在の状態 ID (未開始は kInvalidState)。 */
    u32   m_Current = kInvalidState;
};

} // namespace acs::game
