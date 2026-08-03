// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — CGameFlow 実装
//
// 設計メモ:
//   ・遷移許可テーブルは「典型的なゲームフロー」を許可する保守的な集合。
//     例えば Gameplay → Splash のような「ゲーム途中で起動ロゴへ戻る」遷移は
//     普通あり得ないので不許可。許可方針:
//       Splash      → MainTitle, ExitingGame
//       MainTitle   → MainMenu, Credits, ExitingGame
//       MainMenu    → MainTitle, Settings, Credits, Loading, ExitingGame
//       Settings    → MainMenu, PauseMenu (どちらから入ったか保存しないので
//                     両方許可。呼び出し側責任)
//       Credits     → MainTitle, MainMenu, ExitingGame
//       Loading     → Gameplay, MainMenu (ロードエラー時)
//       Gameplay    → PauseMenu, GameOver, Loading, ExitingGame
//       PauseMenu   → Gameplay, Settings, MainMenu (ゲーム放棄), ExitingGame
//       GameOver    → Gameplay (Continue), MainTitle, MainMenu, ExitingGame
//       ExitingGame → (どこにも遷移しない、終端)
//   ・fade_sec=0 は即時遷移。Tick 1 回で m_Current 切替まで完了し、その Tick
//     終了時点で IsTransitioning()=false / FadeProgress()=0 になる。
//   ・fade_sec>0 は fade_out (= fade_sec/2) → 切替 → fade_in (= fade_sec/2)
//     の 2 段階。fade_out 中は m_Current=旧 / m_Pending=新、fade_in 中は
//     m_Current=新 / m_Pending=新。
//   ・遷移中の追加 RequestTransition は no-op。後勝ち設計にしない理由は、
//     enter/exit コールバックの副作用 (サウンド再生 / Save 書き出し) が
//     2 重発火するのを避けるため。
#include "gameframework/GameFlow.h"

namespace acs::game {

namespace {

/**
 * 経過時間を [0, 1] の進捗に正規化する (CFadeTransition.cpp と同形)。
 *
 * @details duration<=0 のときは即完了として 1.0 を返す。結果は [0, 1] にクランプされる。
 * @param elapsed フェーズ内での経過秒。
 * @param duration フェーズの総秒数。
 * @return 進捗 [0, 1]。
 */
inline f32 SafeProgress(f32 elapsed, f32 duration) noexcept {
    if (duration <= 0.0f) return 1.0f;
    const f32 t = elapsed / duration;
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

} // namespace

/** 典型的なゲームフローを許可する保守的な遷移テーブルを組み立てる。 */
void CGameFlow::BuildTransitionTable() noexcept {
    // 全 false で初期化 (default-init で既に 0 だが明示)。
    for (u32 i = 0; i < kFlowStateCount; ++i) {
        for (u32 j = 0; j < kFlowStateCount; ++j) {
            m_Allowed[i][j] = false;
        }
    }

    auto allow = [this](EFlowState from, EFlowState to) noexcept {
        m_Allowed[IndexOfByKey(from)][IndexOfByKey(to)] = true;
    };

    // Splash → MainTitle, ExitingGame
    allow(EFlowState::Splash,      EFlowState::MainTitle);
    allow(EFlowState::Splash,      EFlowState::ExitingGame);

    // MainTitle → MainMenu, Credits, ExitingGame
    allow(EFlowState::MainTitle,   EFlowState::MainMenu);
    allow(EFlowState::MainTitle,   EFlowState::Credits);
    allow(EFlowState::MainTitle,   EFlowState::ExitingGame);

    // MainMenu → MainTitle, Settings, Credits, Loading, ExitingGame
    allow(EFlowState::MainMenu,    EFlowState::MainTitle);
    allow(EFlowState::MainMenu,    EFlowState::Settings);
    allow(EFlowState::MainMenu,    EFlowState::Credits);
    allow(EFlowState::MainMenu,    EFlowState::Loading);
    allow(EFlowState::MainMenu,    EFlowState::ExitingGame);

    // Settings → MainMenu, PauseMenu (呼び出し側がどちらから入ったか管理)
    allow(EFlowState::Settings,     EFlowState::MainMenu);
    allow(EFlowState::Settings,     EFlowState::PauseMenu);

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

    // PauseMenu → Gameplay, Settings, MainMenu, ExitingGame
    allow(EFlowState::PauseMenu,   EFlowState::Gameplay);
    allow(EFlowState::PauseMenu,   EFlowState::Settings);
    allow(EFlowState::PauseMenu,   EFlowState::MainMenu);
    allow(EFlowState::PauseMenu,   EFlowState::ExitingGame);

    // GameOver → Gameplay (Continue), MainTitle, MainMenu, ExitingGame
    allow(EFlowState::GameOver,    EFlowState::Gameplay);
    allow(EFlowState::GameOver,    EFlowState::MainTitle);
    allow(EFlowState::GameOver,    EFlowState::MainMenu);
    allow(EFlowState::GameOver,    EFlowState::ExitingGame);

    // ExitingGame からはどこにも行けない (終端)。
}

/** state スロットと遷移テーブルを構築し、初期状態へ入って OnEnter を即発火する。 */
void CGameFlow::Init(EFlowState initial_state) noexcept {
    // 既存スロットを破棄して再構築 (複数回 Init 対応)。
    _states.Reset();
    for (u32 i = 0; i < kFlowStateCount; ++i) {
        _states.Add(FStateSlot{});
    }

    BuildTransitionTable();

    m_Current          = initial_state;
    m_Pending          = initial_state;
    m_Phase            = EPhase::Idle;
    m_IsTransitioning = false;
    m_FadeOutSec     = 0.0f;
    m_FadeInSec      = 0.0f;
    m_PhaseElapsed    = 0.0f;
    m_FadeProgress    = 0.0f;
    m_Initialized      = true;

    // initial_state の OnEnter を即発火 (画面起動時 = Splash 入場相当)。
    const u32 idx = IndexOfByKey(initial_state);
    if (idx < kFlowStateCount) {
        const FStateSlot& slot = _states[idx];
        if (slot.enter != nullptr) {
            slot.enter(slot.enter_user, initial_state);
        }
    }
}

/** 現在 state から to への遷移が許可テーブルで許されているかを返す。 */
bool CGameFlow::CanTransitionTo(EFlowState to) const noexcept {
    if (!m_Initialized) return false;
    const u32 from_idx = IndexOfByKey(m_Current);
    const u32 to_idx   = IndexOfByKey(to);
    if (from_idx >= kFlowStateCount || to_idx >= kFlowStateCount) return false;
    if (from_idx == to_idx) return false;  // 同一 state への遷移は不可
    return m_Allowed[from_idx][to_idx];
}

/** 遷移を要求し fade フェーズを開始する (不正遷移 / 遷移中は no-op)。 */
void CGameFlow::RequestTransition(EFlowState to, f32 fade_sec) noexcept {
    if (!m_Initialized) return;
    if (m_IsTransitioning) return;            // 進行中の追加要求は無視
    if (!CanTransitionTo(to)) return;          // 不正遷移は無視

    m_Pending          = to;
    m_IsTransitioning = true;
    m_PhaseElapsed    = 0.0f;

    if (fade_sec <= 0.0f) {
        // 即時遷移: fade なし。次の Tick で 1 step だけ進行して完了する。
        m_FadeOutSec  = 0.0f;
        m_FadeInSec   = 0.0f;
        m_Phase         = EPhase::FadingOut;     // Tick で即 FadingIn → Idle に進む
        m_FadeProgress = 0.0f;
        return;
    }

    // fade_sec を out / in で等分。
    const f32 half = fade_sec * 0.5f;
    m_FadeOutSec  = half;
    m_FadeInSec   = half;
    m_Phase         = EPhase::FadingOut;
    m_FadeProgress = 0.0f;
}

/** 指定 state の OnEnter コールバックを登録する (範囲外は no-op)。 */
void CGameFlow::SetOnEnterCallback(EFlowState state, StateCallback cb, void* user) noexcept {
    if (!m_Initialized) return;
    const u32 idx = IndexOfByKey(state);
    if (idx >= kFlowStateCount) return;
    FStateSlot& slot = _states[idx];
    slot.enter      = cb;
    slot.enter_user = user;
}

/** 指定 state の OnExit コールバックを登録する (範囲外は no-op)。 */
void CGameFlow::SetOnExitCallback(EFlowState state, StateCallback cb, void* user) noexcept {
    if (!m_Initialized) return;
    const u32 idx = IndexOfByKey(state);
    if (idx >= kFlowStateCount) return;
    FStateSlot& slot = _states[idx];
    slot.exit       = cb;
    slot.exit_user  = user;
}

/** fade timer を進め、完了時に state を切り替えて exit/enter コールバックを発火する。 */
void CGameFlow::Tick(f32 dt) noexcept {
    if (!m_Initialized) return;
    if (!m_IsTransitioning) return;
    if (dt < 0.0f) dt = 0.0f;

    m_PhaseElapsed += dt;

    switch (m_Phase) {
        case EPhase::FadingOut: {
            const f32 t   = SafeProgress(m_PhaseElapsed, m_FadeOutSec);
            m_FadeProgress = t;                // 0 → 1
            if (t >= 1.0f) {
                // 旧 state の OnExit を発火。
                const u32 old_idx = IndexOfByKey(m_Current);
                if (old_idx < kFlowStateCount) {
                    const FStateSlot& old_slot = _states[old_idx];
                    if (old_slot.exit != nullptr) {
                        old_slot.exit(old_slot.exit_user, m_Current);
                    }
                }

                // m_Current を切替。
                m_Current = m_Pending;

                // 新 state の OnEnter を発火。
                const u32 new_idx = IndexOfByKey(m_Current);
                if (new_idx < kFlowStateCount) {
                    const FStateSlot& new_slot = _states[new_idx];
                    if (new_slot.enter != nullptr) {
                        new_slot.enter(new_slot.enter_user, m_Current);
                    }
                }

                // fade_in フェーズへ。
                m_Phase         = EPhase::FadingIn;
                m_PhaseElapsed = 0.0f;
                m_FadeProgress = 1.0f;
            }
            break;
        }

        case EPhase::FadingIn: {
            const f32 t   = SafeProgress(m_PhaseElapsed, m_FadeInSec);
            m_FadeProgress = 1.0f - t;          // 1 → 0
            if (t >= 1.0f) {
                m_Phase            = EPhase::Idle;
                m_PhaseElapsed    = 0.0f;
                m_FadeProgress    = 0.0f;
                m_IsTransitioning = false;
                m_Pending          = m_Current;
            }
            break;
        }

        case EPhase::Idle:
        default:
            // 到達不能 (冒頭で return 済)
            break;
    }
}

} // namespace acs::game
