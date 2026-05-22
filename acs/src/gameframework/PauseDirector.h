// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — PauseDirector (統合 pause 管理)
//
// 役割:
//   ゲーム全体の "pause" 状態を一元管理する director。
//   現代のゲームでは「pause すべき理由」が同時に複数発生する:
//     ・ユーザがメニューを開いた  (UserMenu)
//     ・OS / システムメニュー表示 (SystemMenu)
//     ・ウィンドウフォーカス喪失   (FocusLost)
//     ・カットシーン再生中         (Cinematic)
//     ・PhotoMode 中               (PhotoMode)
//     ・ネットワーク同期待ち       (NetworkSync)
//   これらを単純な on/off bool で管理すると、片方が解除されたら resume
//   してしまい「メニュー閉じたらフォーカス喪失中なのに動き出した」等の
//   バグを生む。PauseDirector は **bit OR された PauseReason mask** を
//   持ち、「mask が完全に 0 になるまでは pause」という stack 的挙動で
//   この問題を回避する。
//
// 想定する典型フロー:
//   PauseDirector pause;
//   pause.SetNormalTimeScale(1.0f);
//
//   // ユーザがメニューを開いた
//   pause.Pause(PauseReason::UserMenu);
//   game.SetTimeScale(pause.EffectiveTimeScale());  // 0.0f
//
//   // 同時に OS メニューも表示された
//   pause.Pause(PauseReason::SystemMenu);
//   // mask = UserMenu | SystemMenu
//
//   // ユーザがメニューを閉じた → ただし SystemMenu は残っている
//   pause.Resume(PauseReason::UserMenu);
//   game.SetTimeScale(pause.EffectiveTimeScale());  // まだ 0.0f
//
//   // OS メニューも閉じた
//   pause.Resume(PauseReason::SystemMenu);
//   game.SetTimeScale(pause.EffectiveTimeScale());  // 1.0f に復帰
//
// 設計選択:
//   ・**bit flag 中心**: PrivacyDirector / SceneServices と同じく
//     enum class : u32 + operator|/& で複合 reason を表現する。
//     None (= 0) は「pause 中ではない」を表す特異値で、Pause(None) /
//     Resume(None) は no-op (OR/AND-NOT で数学的に変化なし)。
//   ・**callback は遷移時のみ**: 各 reason の bit が "立った瞬間" と
//     "落ちた瞬間" にだけ発火する。同じ reason の重複 Pause/Resume では
//     呼ばれない。caller はパッシブ UI 更新やオーディオ ducking 等に
//     使える。
//   ・**caller が時間操作を行う**: PhotoMode と同じく EffectiveTimeScale()
//     は値を返すだけ。実際の `Game::SetTimeScale(...)` は外側のゲーム
//     コードが呼ぶ。GameFramework モジュールから Game への依存を切る
//     (Pillar 規約)。
//   ・**NormalTimeScale 分離**: slow-motion 演出 (= 通常時 0.5x など) と
//     pause を直交管理するため、「pause 解除時に戻る scale」を別保持。
//     EffectiveTimeScale() は pause 中 = 0, 非 pause = _normal_time_scale。
//   ・**非コピー・非ムーブ**: アプリ全体で 1 個運用される director なので、
//     値渡しでスタック状態が分裂しないよう移動コンストラクタも禁止。
//   ・**全 noexcept**: ACS 規約 (Result<T,E> + 例外なし) に従う。
//     ただし PauseDirector は失敗し得る I/O を持たないので Result は使わず
//     全 API を noexcept void / 値返しで構成する。
//
// 範囲外 (Phase 2+ で):
//   ・per-system pause (オーディオだけ pause / 物理だけ pause 等)。
//     現状は全体一律の time_scale = 0 想定。
//   ・スローモーション補間 (1.0 → 0.0 の遷移カーブ)。Tween モジュール側で
//     扱う想定。
//   ・pause 中の入力 routing (Pillar A InputMap 側の責務)。
#pragma once

#include "foundation/Types.h"

namespace acs::game {

// =============================================================================
// PauseReason — bit flag で表す pause 理由
// -----------------------------------------------------------------------------
// 各 reason は独立に on/off できる必要があり、同時に複数立ち得る (e.g.
// UserMenu と FocusLost が同時)。None (= 0) は「pause 中ではない」を
// 表す特異値で、Pause(None) / Resume(None) は no-op。Custom1/Custom2 は
// ゲーム固有の理由 (e.g. "ボスイントロムービー") を割り当てる予備枠。
// =============================================================================
enum class PauseReason : u32 {
    None        = 0,         // pause 中ではない (特異値)
    UserMenu    = 1u << 0,   // ユーザがメニューを開いた
    SystemMenu  = 1u << 1,   // OS / システムメニュー表示中
    FocusLost   = 1u << 2,   // ウィンドウフォーカス喪失
    Cinematic   = 1u << 3,   // カットシーン再生中
    PhotoMode   = 1u << 4,   // PhotoMode 中
    NetworkSync = 1u << 5,   // ネットワーク同期待ち
    Custom1     = 1u << 6,   // ゲーム固有の予備枠 1
    Custom2     = 1u << 7,   // ゲーム固有の予備枠 2
};

constexpr PauseReason operator|(PauseReason a, PauseReason b) noexcept {
    return static_cast<PauseReason>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr PauseReason operator&(PauseReason a, PauseReason b) noexcept {
    return static_cast<PauseReason>(static_cast<u32>(a) & static_cast<u32>(b));
}

// =============================================================================
// PauseEventCallback — pause/resume 遷移時に発火する関数ポインタ
// -----------------------------------------------------------------------------
// reason: 立った / 落ちた reason の bit。複合 (UserMenu | SystemMenu) を
//   一気に Pause/Resume した場合は、各個別 bit について個別に呼ばれる
//   (= UserMenu 用 1 回 + SystemMenu 用 1 回)。
// paused: 遷移方向。true = 立ち上がり (= pause へ)、false = 立ち下がり (= resume へ)。
// user: SetCallback() で渡したコンテキストポインタ。
// =============================================================================
using PauseEventCallback = void(*)(void* user, PauseReason reason, bool paused) noexcept;

// =============================================================================
// PauseDirector — pause stack 管理
// -----------------------------------------------------------------------------
// アプリ全体で 1 個運用される想定。複数同時運用しないため非コピー・非ムーブ。
// =============================================================================
class PauseDirector {
public:
    PauseDirector()  noexcept = default;
    ~PauseDirector() noexcept = default;

    // 非コピー・非ムーブ (state holder の唯一性を担保)
    PauseDirector(const PauseDirector&)            = delete;
    PauseDirector& operator=(const PauseDirector&) = delete;
    PauseDirector(PauseDirector&&)                 = delete;
    PauseDirector& operator=(PauseDirector&&)      = delete;

    // ----- pause 操作 (bit 単位) -----
    // Pause:  指定 reason の bit を立てる (OR)。複合 (UserMenu | SystemMenu) 可。
    //         既に立っている bit は callback を呼ばない (no-op)。
    // Resume: 指定 reason の bit を落とす (& ~reason)。
    //         既に落ちている bit は callback を呼ばない (no-op)。
    void Pause(PauseReason reason)  noexcept;
    void Resume(PauseReason reason) noexcept;

    // ----- 問い合わせ -----
    // IsPausedFor: reason の **全ての** bit が立っているかを確認 (複合 reason 可)。
    //   None (= 0) を渡した場合は仕様により常に true (空集合は包含される)。
    bool IsPausedFor(PauseReason reason) const noexcept;

    // 1 つでも reason が立っていれば true。EffectiveTimeScale() の判定基準。
    bool IsPaused() const noexcept;

    // 現在立っている全 reason の bit OR。デバッグ表示や永続化に使う。
    PauseReason ActiveReasons() const noexcept;

    // ----- 一括解除 -----
    // 全 reason を一気に落とす。callback は立っていた各 bit について発火する。
    // タイトル画面に戻る等、強制的に pause 状態を全クリアしたい場面で使う。
    void Clear() noexcept;

    // ----- time scale 連携 -----
    // EffectiveTimeScale: caller が `Game::SetTimeScale(...)` に渡す値。
    //   pause 中 (= IsPaused()) → 0.0f
    //   非 pause              → _normal_time_scale
    f32 EffectiveTimeScale() const noexcept;

    // SetNormalTimeScale: pause 解除時に戻る scale。slow-motion 演出
    //   (= 通常時 0.5x など) と pause を直交管理するため別保持。
    //   負値は 0 に clamp (負の時間進行は別概念なので別 API で扱う)。
    void SetNormalTimeScale(f32 s) noexcept;
    f32  NormalTimeScale() const noexcept;

    // ----- 遷移通知 -----
    // SetCallback: pause/resume 遷移時に呼ばれる関数ポインタを登録する。
    //   cb == nullptr で登録解除。重複登録は不可 (上書き)。
    //   user は callback 呼び出し時にそのまま渡される。
    //
    //   呼び出しタイミング:
    //     ・Pause(reason) で reason のうち "新たに立った" bit ごとに paused=true で 1 回
    //     ・Resume(reason) で reason のうち "実際に落ちた" bit ごとに paused=false で 1 回
    //     ・Clear() で立っていた全 bit について paused=false で個別に 1 回
    //   呼び出し中に Pause/Resume を再帰呼び出ししても安全だが、その場合の
    //   callback 順序は実装依存 (= 内部 mask は遷移計算後に更新される)。
    void SetCallback(PauseEventCallback cb, void* user) noexcept;

private:
    // 立っていた各 bit について個別に callback を呼ぶ内部ヘルパ。
    // changed_bits は「実際に変化した bit」、paused は遷移方向。
    void FireTransitions(u32 changed_bits, bool paused) const noexcept;

    u32                _mask              = 0u;    // 現在立っている reason の bit OR
    f32                _normal_time_scale = 1.0f;  // pause 解除時に戻る scale
    PauseEventCallback _callback          = nullptr;
    void*              _callback_user     = nullptr;
};

} // namespace acs::game
