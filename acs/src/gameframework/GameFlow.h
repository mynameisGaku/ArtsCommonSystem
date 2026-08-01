// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — CGameFlow (高レベルゲームフロー state machine)
//
// 役割:
//   タイトル / メインメニュー / ゲームプレイ / ポーズ / 設定 / GameOver 等、ゲーム
//   全体のフロー状態を保持する 1 シングルトン的 state machine。CSceneManager より
//   1 段上のレイヤで、「いまユーザーがゲームのどの段階に居るか」を識別する。
//
// CSceneManager との棲み分け:
//   ・CSceneManager は描画用 AScene のスタックを管理する低レベル機構
//     (push / pop / change + 退場 ring buffer + fixed_dt)。
//   ・CGameFlow は「ゲームとしての論理状態」を管理する高レベル state machine。
//     状態ごとの enter / exit コールバックでサウンド切替や Save 書き出し等の
//     副作用を発火し、実描画はコールバックの中で CSceneManager を呼び分ける。
//   ・両者は独立。CGameFlow は CSceneManager に依存しない (テスト容易性のため)。
//
// 設計選択:
//   ・**enum EFlowState (10 状態固定)**: ゲームの抽象状態を列挙。動的追加なし。
//   ・**遷移は要求 + Tick 適用**: RequestTransition で要求 → Tick が fade_out 中
//     はカウントダウン → 完了したら _OnExit → state 切替 → _OnEnter → fade_in。
//     これにより「現在 / 次の state が両方分かる」期間を作り、UI 側で overlay
//     を被せられる。fade_sec=0 の場合は同フレーム内で即時遷移 (Tick での fade
//     経過なし)。
//   ・**遷移テーブル**: 不正遷移 (例: Gameplay → Splash) を防ぐため、from →
//     to の可否を 10x10 の bool テーブルで持つ。Init() 時に組み立てる。
//   ・**コールバックは関数ポインタ + void* user**: ACS 規約に従い std::function
//     不使用。Pillar Q CCinematicsDirector / CHotReloadWatcher と同形。1 state につき
//     最大 enter / exit 1 個ずつ。
//   ・**fade 量は state holder のみ**: FadeProgress() を [0, 1] で返す。描画は
//     呼び出し側が FSpriteBatch で fullscreen overlay を被せる責任。
//     CFadeTransition と独立 (シーン内 fade と画面間 fade を別レイヤで扱える)。
//   ・**非コピー・非ムーブ**: CGame に 1 個持つ長寿命オブジェクト。state 分裂
//     を避けるため最初から禁止。
//
// 範囲外 (将来拡張):
//   ・state ごとの transient state (例: Loading の進捗値) — 必要なら呼び出し側
//     が AppState で別途持つ
//   ・遷移履歴の back stack — Pop 系 API は持たず、要求は常に「to 指定」
//   ・並列 fade (画面内 fade) — AScene 単位の CFadeTransition が独立して動く
//
// 使い方:
//   acs::game::CGameFlow flow;
//   flow.SetOnEnterCallback(EFlowState::Gameplay, &FMyApp::OnGameplayEnter, this);
//   flow.Init(EFlowState::Splash);
//   // ... 毎フレーム:
//   flow.Tick(dt);
//   if (input.JustPressed(EKey::Enter) && flow.CurrentState() == EFlowState::MainTitle) {
//       flow.RequestTransition(EFlowState::MainMenu, 0.5f);
//   }
//   if (flow.IsTransitioning()) {
//       // fade overlay を FadeProgress() の alpha で描く
//   }
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 高レベルゲームフロー状態。
 *
 * @details
 * 10 個固定で、Init 時に内部 _states を 10 個確保する。列挙順序は
 * kFlowStateCount (= 10) に合わせること (値を配列インデックスに使う)。
 */
enum class EFlowState : u8 {
    /** ロゴ表示 (Studio / Publisher / Engine)。 */
    Splash       = 0,

    /** タイトル画面 (PRESS START 待機)。 */
    MainTitle    = 1,

    /** メインメニュー (NewGame / Load / Settings / Exit)。 */
    MainMenu     = 2,

    /** 設定画面 (MainMenu / PauseMenu のどちらからも入れる)。 */
    Settings      = 3,

    /** クレジット表示。 */
    Credits      = 4,

    /** セーブ読込 or ステージロード。 */
    Loading      = 5,

    /** ゲーム本編 (実際のプレイ)。 */
    Gameplay     = 6,

    /** Gameplay 中のポーズ。 */
    PauseMenu    = 7,

    /** ゲームオーバー画面 (Continue / Title へ戻る)。 */
    GameOver     = 8,

    /** アプリ終了直前 (シャットダウン処理中)。 */
    ExitingGame  = 9,
};

/** EFlowState の総数 (内部 _states 配列サイズに使う)。 */
inline constexpr u32 kFlowStateCount = 10;

/**
 * 遷移メタデータ。
 *
 * @details
 * RequestTransition で内部に保持され、Tick で進行する。fade_in_sec / fade_out_sec
 * が両方 0 の場合は「即時遷移」(Tick 1 回で完了)。
 */
struct FFlowTransition {
    /** 遷移元の state。 */
    EFlowState from         = EFlowState::Splash;

    /** 遷移先の state。 */
    EFlowState to           = EFlowState::Splash;

    /** 新 state 入場時の fade-in 秒数。 */
    f32       fade_in_sec  = 0.0f;

    /** 旧 state 退場時の fade-out 秒数。 */
    f32       fade_out_sec = 0.0f;
};

/**
 * 高レベルゲームフローを管理する state machine。
 *
 * @details
 * タイトル / メニュー / ゲームプレイ / ポーズ等、ゲーム全体のフロー状態を保持する。
 * 遷移は RequestTransition で要求し Tick で適用する 2 段階方式で、fade_out → 旧 state
 * の OnExit → state 切替 → 新 state の OnEnter → fade_in と進む。状態ごとの enter/exit
 * コールバックは関数ポインタ + void* user で登録する (std::function 不使用)。不正遷移は
 * 10x10 の許可テーブルで弾く。CSceneManager とは独立した非コピー・非ムーブ型。
 */
class CGameFlow {
public:
    /**
     * 状態遷移コールバックの関数ポインタ型。
     *
     * @details std::function 不使用ポリシーに従い void* user でコンテキストを引き回す。
     * @param user SetOn...Callback で渡した値 (任意のオブジェクト this 等)。
     * @param entered_state 遷移先 (Enter) or 遷移元 (Exit) の state ID。
     */
    using StateCallback = void(*)(void* user, EFlowState entered_state) noexcept;

    /** 未初期化状態で構築する (Init を呼ぶまで遷移系 API は no-op)。 */
    CGameFlow()  noexcept = default;

    /** 破棄する。 */
    ~CGameFlow() noexcept = default;

    /** コピー禁止 (CGame に 1 個持つ長寿命オブジェクトで state 分裂を避けるため)。 */
    CGameFlow(const CGameFlow&)            = delete;

    /** コピー代入も禁止。 */
    CGameFlow& operator=(const CGameFlow&) = delete;

    /** ムーブ禁止。 */
    CGameFlow(CGameFlow&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CGameFlow& operator=(CGameFlow&&)      = delete;

    /**
     * state スロットと遷移許可テーブルを構築し、初期状態へ入る。
     *
     * @details
     * 内部の state スロット (10 個) と遷移許可テーブルを組み立て、initial_state に入る
     * (該当 state の OnEnter があれば即発火)。複数回呼ぶと再初期化される。
     * @param initial_state 初期状態 (既定 Splash)。
     */
    void Init(EFlowState initial_state = EFlowState::Splash) noexcept;

    /**
     * 現在の state を返す。
     *
     * @return 現在の EFlowState (fade_out 中は旧 state)。
     */
    EFlowState CurrentState()    const noexcept { return m_Current; }

    /**
     * 遷移先 (保留中) の state を返す。
     *
     * @return 遷移先の EFlowState (非遷移中は m_Current と同値)。
     */
    EFlowState PendingState()    const noexcept { return m_Pending; }

    /**
     * 遷移中かどうかを返す。
     *
     * @return fade_out / fade_in のいずれか進行中なら true。
     */
    bool      IsTransitioning() const noexcept { return m_IsTransitioning; }

    /**
     * 現在の state から to へ遷移できるかを返す。
     *
     * @details Init 後の許可テーブルを参照する。同一 state (from==to) は常に false。
     * @param to 遷移先候補の state。
     * @return 遷移が許可されていれば true。
     */
    bool CanTransitionTo(EFlowState to) const noexcept;

    /**
     * 現在の fade overlay の進捗を返す。
     *
     * @return [0, 1] の不透明度。fade_out 中は 0→1、切替後の fade_in 中は 1→0、非遷移中は 0。
     */
    f32  FadeProgress() const noexcept { return m_FadeProgress; }

    /**
     * state 遷移を要求する。
     *
     * @details
     * 不正遷移は no-op、既に遷移中の追加要求も no-op (後勝ちしない。現遷移を完了させてから
     * 次を受け付ける)。fade_sec=0 の場合は即時遷移。内部実装上は fade_in / fade_out を
     * fade_sec / 2 ずつに等分する。
     * @param to 遷移先の state。
     * @param fade_sec fade 全体の秒数 (out/in で等分。0 で即時遷移。既定 0.3)。
     */
    void RequestTransition(EFlowState to, f32 fade_sec = 0.3f) noexcept;

    /**
     * 指定 state の OnEnter コールバックを登録する。
     *
     * @details 範囲外の state は no-op。cb=nullptr の登録で解除できる。
     * @param state 対象の state。
     * @param cb 入場時に呼ぶコールバック (nullptr で解除)。
     * @param user コールバックへ渡すコンテキスト。
     */
    void SetOnEnterCallback(EFlowState state, StateCallback cb, void* user) noexcept;

    /**
     * 指定 state の OnExit コールバックを登録する。
     *
     * @details 範囲外の state は no-op。cb=nullptr の登録で解除できる。
     * @param state 対象の state。
     * @param cb 退場時に呼ぶコールバック (nullptr で解除)。
     * @param user コールバックへ渡すコンテキスト。
     */
    void SetOnExitCallback (EFlowState state, StateCallback cb, void* user) noexcept;

    /**
     * fade timer を進め、必要なら state 切替と enter/exit コールバックを発火する。
     *
     * @details Init 前 / 非遷移中の Tick は no-op (timer 進行なし)。
     * @param dt 前フレームからの経過秒 (負値は 0 にクランプ)。
     */
    void Tick(f32 dt) noexcept;

private:
    /**
     * 1 state 分のコールバックスロット。
     *
     * @details Init() で kFlowStateCount 個確保される。
     */
    struct FStateSlot {
        /** 入場コールバック (未登録なら nullptr)。 */
        StateCallback enter      = nullptr;

        /** 入場コールバックへ渡す user ポインタ。 */
        void*         enter_user = nullptr;

        /** 退場コールバック (未登録なら nullptr)。 */
        StateCallback exit       = nullptr;

        /** 退場コールバックへ渡す user ポインタ。 */
        void*         exit_user  = nullptr;
    };

    /** 遷移の進行段階。 */
    enum class EPhase : u8 {
        /** 非遷移中。 */
        Idle      = 0,

        /** fade_out 中 (旧 state がまだ m_Current)。 */
        FadingOut = 1,

        /** m_Current 切替済、fade_in で overlay が消えていく段階。 */
        FadingIn  = 2,
    };

    /** 遷移許可テーブル (10x10 の bool) を組み立てる (Init から呼ばれる)。 */
    void BuildTransitionTable() noexcept;

    /**
     * state を配列インデックスに変換する。
     *
     * @param s 変換する state。
     * @return state の数値。範囲外なら kFlowStateCount 以上 (呼び出し側で skip 判定に使う)。
     */
    static u32 IndexOf(EFlowState s) noexcept { return static_cast<u32>(s); }

    /** state ごとのコールバックスロット (size = kFlowStateCount)。 */
    TArray<FStateSlot> _states;

    /** 遷移許可テーブル ([from][to] が true なら遷移可能)。 */
    bool             m_Allowed[kFlowStateCount][kFlowStateCount] = {};

    /** Init 済みフラグ (false 中は遷移系 API が no-op)。 */
    bool             m_Initialized      = false;

    /** 現在の state (fade_out 中は旧 state)。 */
    EFlowState m_Current          = EFlowState::Splash;

    /** 遷移先 (保留中) の state。 */
    EFlowState m_Pending          = EFlowState::Splash;

    /** 遷移の進行段階。 */
    EPhase     m_Phase            = EPhase::Idle;

    /** 遷移中フラグ。 */
    bool      m_IsTransitioning = false;

    /** fade_out フェーズの秒数 (fade_sec / 2)。 */
    f32 m_FadeOutSec  = 0.0f;

    /** fade_in フェーズの秒数 (fade_sec / 2)。 */
    f32 m_FadeInSec   = 0.0f;

    /** 現在 phase 内での経過秒。 */
    f32 m_PhaseElapsed = 0.0f;

    /** fade overlay の不透明度 [0, 1]。 */
    f32 m_FadeProgress = 0.0f;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FGameFlow = CGameFlow;

} // namespace acs::game
