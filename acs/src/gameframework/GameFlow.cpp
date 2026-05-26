// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — FGameFlow 実装
//
// 設計メモ:
//   ・遷移許可テーブルは「典型的なゲームフロー」を許可する保守的な集合。
//     例えば Gameplay → Splash のような「ゲーム途中で起動ロゴへ戻る」遷移は
//     普通あり得ないので不許可。許可方針:
//       Splash      → MainTitle, ExitingGame
//       MainTitle   → MainMenu, Credits, ExitingGame
//       MainMenu    → MainTitle, FSettings, Credits, Loading, ExitingGame
//       FSettings    → MainMenu, PauseMenu (どちらから入ったか保存しないので
//                     両方許可。呼び出し側責任)
//       Credits     → MainTitle, MainMenu, ExitingGame
//       Loading     → Gameplay, MainMenu (ロードエラー時)
//       Gameplay    → PauseMenu, GameOver, Loading, ExitingGame
//       PauseMenu   → Gameplay, FSettings, MainMenu (ゲーム放棄), ExitingGame
//       GameOver    → Gameplay (Continue), MainTitle, MainMenu, ExitingGame
//       ExitingGame → (どこにも遷移しない、終端)
//   ・fade_sec=0 は即時遷移。Tick 1 回で _current 切替まで完了し、その Tick
//     終了時点で IsTransitioning()=false / FadeProgress()=0 になる。
//   ・fade_sec>0 は fade_out (= fade_sec/2) → 切替 → fade_in (= fade_sec/2)
//     の 2 段階。fade_out 中は _current=旧 / _pending=新、fade_in 中は
//     _current=新 / _pending=新。
//   ・遷移中の追加 RequestTransition は no-op。後勝ち設計にしない理由は、
//     enter/exit コールバックの副作用 (サウンド再生 / Save 書き出し) が
//     2 重発火するのを避けるため。
#include "gameframework/GameFlow.h"

namespace acs::game {

namespace {

// duration<=0 のときに 1.0 を返す進捗ヘルパ (FadeTransition.cpp と同形)。
inline f32 SafeProgress(f32 elapsed, f32 duration) noexcept {
    if (duration <= 0.0f) return 1.0f;
    const f32 t = elapsed / duration;
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

} // namespace

void FGameFlow::BuildTransitionTable() noexcept {
    // 全 false で初期化 (default-init で既に 0 だが明示)。
    for (u32 i = 0; i < kFlowStateCount; ++i) {
        for (u32 j = 0; j < kFlowStateCount; ++j) {
            _allowed[i][j] = false;
        }
    }

    auto allow = [this](EFlowState from, EFlowState to) noexcept {
        _allowed[IndexOf(from)][IndexOf(to)] = true;
    };

    // Splash → MainTitle, ExitingGame
    allow(EFlowState::Splash,      EFlowState::MainTitle);
    allow(EFlowState::Splash,      EFlowState::ExitingGame);

    // MainTitle → MainMenu, Credits, ExitingGame
    allow(EFlowState::MainTitle,   EFlowState::MainMenu);
    allow(EFlowState::MainTitle,   EFlowState::Credits);
    allow(EFlowState::MainTitle,   EFlowState::ExitingGame);

    // MainMenu → MainTitle, FSettings, Credits, Loading, ExitingGame
    allow(EFlowState::MainMenu,    EFlowState::MainTitle);
    allow(EFlowState::MainMenu,    EFlowState::FSettings);
    allow(EFlowState::MainMenu,    EFlowState::Credits);
    allow(EFlowState::MainMenu,    EFlowState::Loading);
    allow(EFlowState::MainMenu,    EFlowState::ExitingGame);

    // FSettings → MainMenu, PauseMenu (呼び出し側がどちらから入ったか管理)
    allow(EFlowState::FSettings,    EFlowState::MainMenu);
    allow(EFlowState::FSettings,    EFlowState::PauseMenu);

    // Credits → MainTitle, MainMenu, ExitingGame
    allow(EFlowState::Credits,     EFlowState::MainTitle);
    allow(EFlowState::Credits,     EFlowState::MainMenu);
    allow(EFlowState::Credits,     EFlowState::ExitingGame);

    // Loading → Gameplay, MainMenu (ロード失敗時の戻り)
    allow(EFlowState::Loading,     EFlowState::Gameplay);
    allow(EFlowState::Loading,     EFlowState::MainMenu);

    // Gameplay → PauseMenu, GameOver, Loading, ExitingGame
    allow(EFlowState::Gameplay,    EFlowState::PauseMenu);
    allow(EFlowState::Gameplay,    EFlowState::GameOver);
    allow(EFlowState::Gameplay,    EFlowState::Loading);
    allow(EFlowState::Gameplay,    EFlowState::ExitingGame);

    // PauseMenu → Gameplay, FSettings, MainMenu, ExitingGame
    allow(EFlowState::PauseMenu,   EFlowState::Gameplay);
    allow(EFlowState::PauseMenu,   EFlowState::FSettings);
    allow(EFlowState::PauseMenu,   EFlowState::MainMenu);
    allow(EFlowState::PauseMenu,   EFlowState::ExitingGame);

    // GameOver → Gameplay (Continue), MainTitle, MainMenu, ExitingGame
    allow(EFlowState::GameOver,    EFlowState::Gameplay);
    allow(EFlowState::GameOver,    EFlowState::MainTitle);
    allow(EFlowState::GameOver,    EFlowState::MainMenu);
    allow(EFlowState::GameOver,    EFlowState::ExitingGame);

    // ExitingGame からはどこにも行けない (終端)。
}

void FGameFlow::Init(EFlowState initial_state) noexcept {
    // 既存スロットを破棄して再構築 (複数回 Init 対応)。
    _states.Clear();
    for (u32 i = 0; i < kFlowStateCount; ++i) {
        _states.PushBack(FStateSlot{});
    }

    BuildTransitionTable();

    _current          = initial_state;
    _pending          = initial_state;
    _phase            = Phase::Idle;
    _is_transitioning = false;
    _fade_out_sec     = 0.0f;
    _fade_in_sec      = 0.0f;
    _phase_elapsed    = 0.0f;
    _fade_progress    = 0.0f;
    _initialized      = true;

    // initial_state の OnEnter を即発火 (画面起動時 = Splash 入場相当)。
    const u32 idx = IndexOf(initial_state);
    if (idx < kFlowStateCount) {
        const FStateSlot& slot = _states[idx];
        if (slot.enter != nullptr) {
            slot.enter(slot.enter_user, initial_state);
        }
    }
}

bool FGameFlow::CanTransitionTo(EFlowState to) const noexcept {
    if (!_initialized) return false;
    const u32 from_idx = IndexOf(_current);
    const u32 to_idx   = IndexOf(to);
    if (from_idx >= kFlowStateCount || to_idx >= kFlowStateCount) return false;
    if (from_idx == to_idx) return false;  // 同一 state への遷移は不可
    return _allowed[from_idx][to_idx];
}

void FGameFlow::RequestTransition(EFlowState to, f32 fade_sec) noexcept {
    if (!_initialized) return;
    if (_is_transitioning) return;            // 進行中の追加要求は無視
    if (!CanTransitionTo(to)) return;          // 不正遷移は無視

    _pending          = to;
    _is_transitioning = true;
    _phase_elapsed    = 0.0f;

    if (fade_sec <= 0.0f) {
        // 即時遷移: fade なし。次の Tick で 1 step だけ進行して完了する。
        _fade_out_sec  = 0.0f;
        _fade_in_sec   = 0.0f;
        _phase         = Phase::FadingOut;     // Tick で即 FadingIn → Idle に進む
        _fade_progress = 0.0f;
        return;
    }

    // fade_sec を out / in で等分。
    const f32 half = fade_sec * 0.5f;
    _fade_out_sec  = half;
    _fade_in_sec   = half;
    _phase         = Phase::FadingOut;
    _fade_progress = 0.0f;
}

void FGameFlow::SetOnEnterCallback(EFlowState state, StateCallback cb, void* user) noexcept {
    if (!_initialized) return;
    const u32 idx = IndexOf(state);
    if (idx >= kFlowStateCount) return;
    FStateSlot& slot = _states[idx];
    slot.enter      = cb;
    slot.enter_user = user;
}

void FGameFlow::SetOnExitCallback(EFlowState state, StateCallback cb, void* user) noexcept {
    if (!_initialized) return;
    const u32 idx = IndexOf(state);
    if (idx >= kFlowStateCount) return;
    FStateSlot& slot = _states[idx];
    slot.exit       = cb;
    slot.exit_user  = user;
}

void FGameFlow::Tick(f32 dt) noexcept {
    if (!_initialized) return;
    if (!_is_transitioning) return;
    if (dt < 0.0f) dt = 0.0f;

    _phase_elapsed += dt;

    switch (_phase) {
        case Phase::FadingOut: {
            const f32 t   = SafeProgress(_phase_elapsed, _fade_out_sec);
            _fade_progress = t;                // 0 → 1
            if (t >= 1.0f) {
                // 旧 state の OnExit を発火。
                const u32 old_idx = IndexOf(_current);
                if (old_idx < kFlowStateCount) {
                    const FStateSlot& old_slot = _states[old_idx];
                    if (old_slot.exit != nullptr) {
                        old_slot.exit(old_slot.exit_user, _current);
                    }
                }

                // _current を切替。
                _current = _pending;

                // 新 state の OnEnter を発火。
                const u32 new_idx = IndexOf(_current);
                if (new_idx < kFlowStateCount) {
                    const FStateSlot& new_slot = _states[new_idx];
                    if (new_slot.enter != nullptr) {
                        new_slot.enter(new_slot.enter_user, _current);
                    }
                }

                // fade_in フェーズへ。
                _phase         = Phase::FadingIn;
                _phase_elapsed = 0.0f;
                _fade_progress = 1.0f;
            }
            break;
        }

        case Phase::FadingIn: {
            const f32 t   = SafeProgress(_phase_elapsed, _fade_in_sec);
            _fade_progress = 1.0f - t;          // 1 → 0
            if (t >= 1.0f) {
                _phase            = Phase::Idle;
                _phase_elapsed    = 0.0f;
                _fade_progress    = 0.0f;
                _is_transitioning = false;
                _pending          = _current;
            }
            break;
        }

        case Phase::Idle:
        default:
            // 到達不能 (冒頭で return 済)
            break;
    }
}

} // namespace acs::game
