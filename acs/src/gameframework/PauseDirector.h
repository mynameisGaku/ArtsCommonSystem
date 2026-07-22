// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FPauseDirector (統合 pause 管理)
//
// 役割:
//   ゲーム全体の "pause" 状態を一元管理する director。
//   現代のゲームでは「pause すべき理由」が同時に複数発生する:
//     ・ユーザがメニューを開いた  (UserMenu)
//     ・OS / システムメニュー表示 (SystemMenu)
//     ・ウィンドウフォーカス喪失   (FocusLost)
//     ・カットシーン再生中         (Cinematic)
//     ・FPhotoMode 中               (PhotoMode)
//     ・ネットワーク同期待ち       (NetworkSync)
//   これらを単純な on/off bool で管理すると、片方が解除されたら resume
//   してしまい「メニュー閉じたらフォーカス喪失中なのに動き出した」等の
//   バグを生む。FPauseDirector は **bit OR された EPauseReason mask** を
//   持ち、「mask が完全に 0 になるまでは pause」という stack 的挙動で
//   この問題を回避する。
//
// 想定する典型フロー:
//   FPauseDirector pause;
//   pause.SetNormalTimeScale(1.0f);
//
//   // ユーザがメニューを開いた
//   pause.Pause(EPauseReason::UserMenu);
//   game.SetTimeScale(pause.EffectiveTimeScale());  // 0.0f
//
//   // 同時に OS メニューも表示された
//   pause.Pause(EPauseReason::SystemMenu);
//   // mask = UserMenu | SystemMenu
//
//   // ユーザがメニューを閉じた → ただし SystemMenu は残っている
//   pause.Resume(EPauseReason::UserMenu);
//   game.SetTimeScale(pause.EffectiveTimeScale());  // まだ 0.0f
//
//   // OS メニューも閉じた
//   pause.Resume(EPauseReason::SystemMenu);
//   game.SetTimeScale(pause.EffectiveTimeScale());  // 1.0f に復帰
//
// 設計選択:
//   ・**bit flag 中心**: FPrivacyDirector / FSceneServices と同じく
//     enum class : u32 + operator|/& で複合 reason を表現する。
//     None (= 0) は「pause 中ではない」を表す特異値で、Pause(None) /
//     Resume(None) は no-op (OR/AND-NOT で数学的に変化なし)。
//   ・**callback は遷移時のみ**: 各 reason の bit が "立った瞬間" と
//     "落ちた瞬間" にだけ発火する。同じ reason の重複 Pause/Resume では
//     呼ばれない。caller はパッシブ UI 更新やオーディオ ducking 等に
//     使える。
//   ・**caller が時間操作を行う**: FPhotoMode と同じく EffectiveTimeScale()
//     は値を返すだけ。実際の `FGame::SetTimeScale(...)` は外側のゲーム
//     コードが呼ぶ。GameFramework モジュールから FGame への依存を切る
//     (Pillar 規約)。
//   ・**NormalTimeScale 分離**: slow-motion 演出 (= 通常時 0.5x など) と
//     pause を直交管理するため、「pause 解除時に戻る scale」を別保持。
//     EffectiveTimeScale() は pause 中 = 0, 非 pause = m_NormalTimeScale。
//   ・**非コピー・非ムーブ**: アプリ全体で 1 個運用される director なので、
//     値渡しでスタック状態が分裂しないよう移動コンストラクタも禁止。
//   ・**全 noexcept**: ACS 規約 (TResult<T,E> + 例外なし) に従う。
//     ただし FPauseDirector は失敗し得る I/O を持たないので TResult は使わず
//     全 API を noexcept void / 値返しで構成する。
//
// 範囲外:
//   ・per-system pause (オーディオだけ pause / 物理だけ pause 等)。
//     現状は全体一律の time_scale = 0 想定。
//   ・スローモーション補間 (1.0 → 0.0 の遷移カーブ)。Tween モジュール側で
//     扱う想定。
//   ・pause 中の入力 routing (Pillar A FInputMap 側の責務)。
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * pause すべき理由を表す bit flag。
 *
 * @details
 * 各 reason は独立に on/off でき、同時に複数立ち得る (e.g. UserMenu と FocusLost が
 * 同時)。FPauseDirector は OR された mask を持ち、mask が 0 になるまで pause を維持する。
 * operator| / operator& で複合 reason を表現する。
 */
enum class EPauseReason : u32 {
    /** pause 中ではないことを表す特異値 (Pause/Resume に渡しても no-op)。 */
    None        = 0,

    /** ユーザがメニューを開いた。 */
    UserMenu    = 1u << 0,

    /** OS / システムメニュー表示中。 */
    SystemMenu  = 1u << 1,

    /** ウィンドウフォーカス喪失。 */
    FocusLost   = 1u << 2,

    /** カットシーン再生中。 */
    Cinematic   = 1u << 3,

    /** FPhotoMode 中。 */
    PhotoMode    = 1u << 4,

    /** ネットワーク同期待ち。 */
    NetworkSync = 1u << 5,

    /** ゲーム固有の予備枠 1。 */
    Custom1     = 1u << 6,

    /** ゲーム固有の予備枠 2。 */
    Custom2     = 1u << 7,
};

/**
 * 2 つの reason を bit OR して複合 reason を作る。
 *
 * @param a 左辺の reason。
 * @param b 右辺の reason。
 * @return a と b の bit を OR した reason。
 */
constexpr EPauseReason operator|(EPauseReason a, EPauseReason b) noexcept {
    return static_cast<EPauseReason>(static_cast<u32>(a) | static_cast<u32>(b));
}

/**
 * 2 つの reason を bit AND して共通 bit を取り出す。
 *
 * @param a 左辺の reason。
 * @param b 右辺の reason。
 * @return a と b の bit を AND した reason。
 */
constexpr EPauseReason operator&(EPauseReason a, EPauseReason b) noexcept {
    return static_cast<EPauseReason>(static_cast<u32>(a) & static_cast<u32>(b));
}

/**
 * pause/resume 遷移時に発火する関数ポインタ型。
 *
 * @details
 * 複合 reason (UserMenu | SystemMenu) を一気に Pause/Resume した場合は、各個別 bit に
 * ついて個別に呼ばれる (= UserMenu 用 1 回 + SystemMenu 用 1 回)。
 * @param user SetCallback() で渡したコンテキストポインタ。
 * @param reason 立った / 落ちた reason の単一 bit。
 * @param paused 遷移方向 (true = pause へ立ち上がり、false = resume へ立ち下がり)。
 */
using PauseEventCallback = void(*)(void* user, EPauseReason reason, bool paused) noexcept;

/**
 * bit OR された reason mask でゲーム全体の pause 状態を一元管理する director。
 *
 * @details
 * 複数の pause 理由が同時に立ち得る状況で、mask が完全に 0 になるまで pause を維持する
 * stack 的挙動を提供する。アプリ全体で 1 個運用される想定で、状態の唯一性を担保するため
 * 非コピー・非ムーブ。time scale は値を返すだけで、実際の時間操作は caller が行う。
 */
class FPauseDirector {
public:
    /** 空状態 (pause 理由なし、NormalTimeScale = 1.0) で構築する。 */
    FPauseDirector()  noexcept = default;

    /** 破棄する。 */
    ~FPauseDirector() noexcept = default;

    /** コピー禁止 (state holder の唯一性を担保するため)。 */
    FPauseDirector(const FPauseDirector&)            = delete;

    /** コピー代入も禁止。 */
    FPauseDirector& operator=(const FPauseDirector&) = delete;

    /** ムーブ禁止 (状態の分裂を防ぐため)。 */
    FPauseDirector(FPauseDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FPauseDirector& operator=(FPauseDirector&&)      = delete;

    /**
     * 指定 reason の bit を立てて pause する (OR)。
     *
     * @details
     * 複合 reason (UserMenu | SystemMenu) も受理する。新たに立った bit のみ callback を
     * 発火し、既に立っている bit は no-op。None を渡しても変化なし。
     * @param reason 立てる pause 理由 (複合可)。
     */
    void Pause(EPauseReason reason)  noexcept;

    /**
     * 指定 reason の bit を落として resume する (& ~reason)。
     *
     * @details
     * 実際に落ちた bit のみ callback を発火し、既に落ちている bit は no-op。
     * None を渡しても変化なし。
     * @param reason 落とす pause 理由 (複合可)。
     */
    void Resume(EPauseReason reason) noexcept;

    /**
     * reason の全ての bit が立っているかを返す。
     *
     * @details 部分一致は不可。None (= 0) を渡した場合は空集合包含により常に true。
     * @param reason 確認する pause 理由 (複合可)。
     * @return reason の全 bit が立っていれば true。
     */
    bool IsPausedFor(EPauseReason reason) const noexcept;

    /**
     * いずれかの reason が立っているかを返す (EffectiveTimeScale の判定基準)。
     *
     * @return 1 つでも reason が立っていれば true。
     */
    bool IsPaused() const noexcept;

    /**
     * 現在立っている全 reason の bit OR を返す (デバッグ表示や永続化に使う)。
     *
     * @return 立っている reason を OR した mask。
     */
    EPauseReason ActiveReasons() const noexcept;

    /**
     * 全 reason を一気に落とす。
     *
     * @details
     * 立っていた各 bit について paused=false で callback を発火してから mask を 0 にする。
     * タイトル画面に戻る等、強制的に pause を全クリアしたい場面で使う。
     */
    void Clear() noexcept;

    /**
     * caller が `FGame::SetTimeScale(...)` に渡すべき実効 time scale を返す。
     *
     * @return pause 中 (= IsPaused()) なら 0.0f、非 pause なら NormalTimeScale。
     */
    f32 EffectiveTimeScale() const noexcept;

    /**
     * pause 解除時に戻る通常 time scale を設定する。
     *
     * @details
     * slow-motion 演出 (= 通常時 0.5x など) と pause を直交管理するため別保持する。
     * 負値は 0 に clamp する (負の時間進行は別概念なので別 API で扱う)。
     * @param s 通常時の time scale (負値は 0 に clamp)。
     */
    void SetNormalTimeScale(f32 s) noexcept;

    /**
     * 現在の通常 time scale を返す。
     *
     * @return 設定済みの NormalTimeScale。
     */
    f32  NormalTimeScale() const noexcept;

    /**
     * pause/resume 遷移時に呼ばれる callback を登録する。
     *
     * @details
     * cb == nullptr で登録解除。重複登録は不可 (上書き)。Pause で新たに立った bit ごとに
     * paused=true、Resume で実際に落ちた bit ごとに paused=false、Clear で立っていた全 bit
     * について paused=false で個別に 1 回呼ばれる。呼び出し中の再帰 Pause/Resume も安全だが
     * その場合の callback 順序は実装依存 (内部 mask は遷移計算後に更新される)。
     * @param cb 登録する callback (nullptr で登録解除)。
     * @param user callback 呼び出し時にそのまま渡すコンテキストポインタ。
     */
    void SetCallback(PauseEventCallback cb, void* user) noexcept;

private:
    /**
     * 変化した各 bit について個別に callback を 1 回ずつ呼ぶ内部ヘルパ。
     *
     * @param changed_bits 実際に変化した bit の OR。
     * @param paused 遷移方向 (true = pause へ、false = resume へ)。
     */
    void FireTransitions(u32 changed_bits, bool paused) const noexcept;

    /** 現在立っている reason の bit OR (0 = pause 中ではない)。 */
    u32                m_Mask              = 0u;

    /** pause 解除時に戻る通常 time scale。 */
    f32                m_NormalTimeScale = 1.0f;

    /** 遷移時に発火する callback (未登録なら nullptr)。 */
    PauseEventCallback m_Callback          = nullptr;

    /** callback 呼び出し時に渡すコンテキストポインタ。 */
    void*              m_CallbackUser     = nullptr;
};

} // namespace acs::game
