// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — GameFlow (高レベルゲームフロー state machine)
//
// 役割:
//   タイトル / メインメニュー / ゲームプレイ / ポーズ / 設定 / GameOver 等、ゲーム
//   全体のフロー状態を保持する 1 シングルトン的 state machine。SceneManager より
//   1 段上のレイヤで、「いまユーザーがゲームのどの段階に居るか」を識別する。
//
// SceneManager との棲み分け:
//   ・SceneManager は描画用 Scene のスタックを管理する低レベル機構
//     (push / pop / change + 退場 ring buffer + fixed_dt)。
//   ・GameFlow は「ゲームとしての論理状態」を管理する高レベル state machine。
//     状態ごとの enter / exit コールバックでサウンド切替や Save 書き出し等の
//     副作用を発火し、実描画はコールバックの中で SceneManager を呼び分ける。
//   ・両者は独立。GameFlow は SceneManager に依存しない (テスト容易性のため)。
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
//     不使用。Pillar Q CinematicsDirector / HotReload と同形。1 state につき
//     最大 enter / exit 1 個ずつ。
//   ・**fade 量は state holder のみ**: FadeProgress() を [0, 1] で返す。描画は
//     呼び出し側が SpriteBatch で fullscreen overlay を被せる責任。
//     FadeTransition と独立 (シーン内 fade と画面間 fade を別レイヤで扱える)。
//   ・**非コピー・非ムーブ**: Game に 1 個持つ長寿命オブジェクト。state 分裂
//     を避けるため最初から禁止。
//
// 範囲外 (将来拡張):
//   ・state ごとの transient state (例: Loading の進捗値) — 必要なら呼び出し側
//     が AppState で別途持つ
//   ・遷移履歴の back stack — Pop 系 API は持たず、要求は常に「to 指定」
//   ・並列 fade (画面内 fade) — Scene 単位の FadeTransition が独立して動く
//
// 使い方:
//   acs::game::GameFlow flow;
//   flow.SetOnEnterCallback(EFlowState::Gameplay, &MyApp::OnGameplayEnter, this);
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

// 高レベルゲームフロー状態。10 個固定 (Init 時に内部 _states を 10 個確保)。
// 列挙順序は kFlowStateCount (= 10) に合わせること。
enum class EFlowState : u8 {
    Splash       = 0,   // ロゴ表示 (Studio / Publisher / Engine)
    MainTitle    = 1,   // タイトル画面 (PRESS START 待機)
    MainMenu     = 2,   // メインメニュー (NewGame / Load / Settings / Exit)
    Settings     = 3,   // 設定画面 (MainMenu / PauseMenu のどちらからも入れる)
    Credits      = 4,   // クレジット表示
    Loading      = 5,   // セーブ読込 or ステージロード
    Gameplay     = 6,   // ゲーム本編 (実際のプレイ)
    PauseMenu    = 7,   // Gameplay 中のポーズ
    GameOver     = 8,   // ゲームオーバー画面 (Continue / Title へ戻る)
    ExitingGame  = 9,   // アプリ終了直前 (シャットダウン処理中)
};

// EFlowState の総数 (内部 _states 配列サイズに使う)。
inline constexpr u32 kFlowStateCount = 10;

// 遷移メタデータ。RequestTransition で内部に保持され、Tick で進行する。
// fade_in_sec / fade_out_sec が両方 0 の場合は「即時遷移」(Tick 1 回で完了)。
struct FlowTransition {
    EFlowState from         = EFlowState::Splash;
    EFlowState to           = EFlowState::Splash;
    f32       fade_in_sec  = 0.0f;   // 新 state 入場時の fade-in 秒数
    f32       fade_out_sec = 0.0f;   // 旧 state 退場時の fade-out 秒数
};

class GameFlow {
public:
    // 関数ポインタ型コールバック。第 1 引数 user は SetOn...Callback で渡した値
    // (任意のオブジェクト this 等)、第 2 引数 entered_state は遷移先 (Enter) or
    // 遷移元 (Exit) の state ID。
    using StateCallback = void(*)(void* user, EFlowState entered_state) noexcept;

    GameFlow()  noexcept = default;
    ~GameFlow() noexcept = default;

    GameFlow(const GameFlow&)            = delete;
    GameFlow& operator=(const GameFlow&) = delete;
    GameFlow(GameFlow&&)                 = delete;
    GameFlow& operator=(GameFlow&&)      = delete;

    // ----- 初期化 -----
    // 内部の state スロット (10 個) と遷移許可テーブルを構築し、initial_state
    // に入る (このとき該当 state の OnEnter があれば即発火)。複数回呼ぶと再初期化。
    void Init(EFlowState initial_state = EFlowState::Splash) noexcept;

    // ----- 状態 query -----
    EFlowState CurrentState()    const noexcept { return _current; }
    EFlowState PendingState()    const noexcept { return _pending; }
    bool      IsTransitioning() const noexcept { return _is_transitioning; }

    // 遷移の合法性を返す。Init 後の table を参照。同一 state (from==to) は false。
    bool CanTransitionTo(EFlowState to) const noexcept;

    // 現在 fade overlay の進捗 [0, 1]。fade_out 中は 0→1、切替後の fade_in 中は
    // 1→0。非遷移中は 0。
    f32  FadeProgress() const noexcept { return _fade_progress; }

    // ----- 遷移要求 -----
    // 不正遷移は no-op。既に遷移中の追加要求も no-op (= 後勝ちしない、現遷移を
    // 完了させてから次を受け付ける設計)。fade_sec=0 の場合は即時遷移。
    // 内部実装上は fade_in / fade_out を等分 (fade_sec / 2 ずつ) する。
    void RequestTransition(EFlowState to, f32 fade_sec = 0.3f) noexcept;

    // ----- コールバック設定 -----
    // 各 state につき OnEnter / OnExit を 1 個ずつ登録できる。範囲外の state は
    // no-op。cb=nullptr の登録で解除可能。
    void SetOnEnterCallback(EFlowState state, StateCallback cb, void* user) noexcept;
    void SetOnExitCallback (EFlowState state, StateCallback cb, void* user) noexcept;

    // ----- 駆動 -----
    // fade timer を進め、必要なら _current の切替と enter/exit コールバックを
    // 発火する。Init 前 / 非遷移中の Tick は no-op (timer 進行なし)。
    void Tick(f32 dt) noexcept;

private:
    // 1 state スロット。Init() で 10 個確保される。
    struct StateSlot {
        StateCallback enter      = nullptr;
        void*         enter_user = nullptr;
        StateCallback exit       = nullptr;
        void*         exit_user  = nullptr;
    };

    // 遷移の進行段階。
    enum class Phase : u8 {
        Idle      = 0,  // 非遷移中
        FadingOut = 1,  // fade_out 中 (旧 state がまだ _current)
        FadingIn  = 2,  // _current 切替済、fade_in で overlay が消えていく
    };

    // 遷移許可テーブルを作る (10x10 の bool)。Init() から呼ばれる。
    void BuildTransitionTable() noexcept;

    // 内部 helper: state → 配列インデックス。範囲外なら kFlowStateCount を返す
    // (= 呼び出し側で skip 判定用)。
    static u32 IndexOf(EFlowState s) noexcept { return static_cast<u32>(s); }

    Array<StateSlot> _states;          // size = kFlowStateCount
    bool             _allowed[kFlowStateCount][kFlowStateCount] = {};
    bool             _initialized      = false;

    EFlowState _current          = EFlowState::Splash;
    EFlowState _pending          = EFlowState::Splash;
    Phase     _phase            = Phase::Idle;
    bool      _is_transitioning = false;

    f32 _fade_out_sec  = 0.0f;
    f32 _fade_in_sec   = 0.0f;
    f32 _phase_elapsed = 0.0f;     // 現在 phase 内での経過秒
    f32 _fade_progress = 0.0f;     // [0, 1] overlay 不透明度
};

} // namespace acs::game
