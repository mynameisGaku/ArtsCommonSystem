// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FSequence / FSequenceRunner
//
// 時間付きアクションの連鎖 (cutscene / 出現ウェーブ / scripted UI 等)。
//   `seq.Wait(0.5f).Call(my_fn, user).Tween(&x, 0, 100, 1.0f).Loop(0)`
// のようなビルダーで定義し、FSequenceRunner::Start(Move(seq)) で実行開始。
//
// 設計選択:
//   ・パラレル合成 (`Parallel(sub)`) は未実装。複数 FSequence を Runner に
//     並列で Start すれば事実上のパラレルになる。
//   ・コールバックは `void(*)(void*)` 関数ポインタ。ACS 規約 (std::function 不使用)。
//   ・FTween は FSequence 内蔵 (FTweenManager に委譲しない) — sequence の進行
//     時間と一体化させたいため。完了時は最終値を正確に書く。
//
// 使い方:
//   class TitleScene : public Scene {
//       FSequenceRunner m_Seqs;
//       FVec3 m_LogoColor;
//       void OnEnter() noexcept override {
//           FSequence s;
//           s.Wait(0.3f)
//            .Tween(&m_LogoColor, FVec3{0,0,0}, FVec3{1,1,1}, 0.5f, Easing::OutCubic)
//            .Wait(1.0f)
//            .Call(&TitleScene::FadeOutBegin, this);
//           m_Seqs.Start(Move(s));
//       }
//       void OnUpdate(f32 dt) noexcept override { m_Seqs.Tick(dt); }
//       static void FadeOutBegin(void* self) noexcept {
//           static_cast<TitleScene*>(self)->_ready_to_quit = true;
//       }
//   };
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "gameframework/Easing.h"

namespace acs::game {

/**
 * シーケンス内のアクション 1 つを表す POD。
 *
 * @details FSequence 内で TArray に詰めて保持する。kind に応じて意味を持つフィールドが変わる。
 */
struct SeqAction {
    /**
     * アクションの種別。
     */
    enum class Kind : u8 {
        /** 指定秒だけ待つ。 */
        Wait,

        /** コールバックを 1 回呼ぶ。 */
        Call,

        /** f32 値を tween する。 */
        TweenF,

        /** FVec2 値を tween する。 */
        TweenV2,

        /** FVec3 値を tween する。 */
        TweenV3,
    };

    /** このアクションの種別。 */
    Kind kind     = Kind::Wait;

    /** Wait / FTween の所要秒 (Call は 0)。 */
    f32  duration = 0.0f;

    /** Call で呼ぶコールバック関数ポインタ。 */
    void(*call_fn)(void* user) noexcept = nullptr;

    /** call_fn に渡す user ポインタ。 */
    void* call_user                     = nullptr;

    /** TweenF の対象 f32 へのポインタ。 */
    f32*  tween_f_target  = nullptr;

    /** TweenF の開始値。 */
    f32   tween_f_from    = 0.0f;

    /** TweenF の終了値。 */
    f32   tween_f_to      = 0.0f;

    /** TweenV2 の対象 FVec2 へのポインタ。 */
    FVec2* tween_v2_target = nullptr;

    /** TweenV2 の開始値。 */
    FVec2  tween_v2_from   {};

    /** TweenV2 の終了値。 */
    FVec2  tween_v2_to     {};

    /** TweenV3 の対象 FVec3 へのポインタ。 */
    FVec3* tween_v3_target = nullptr;

    /** TweenV3 の開始値。 */
    FVec3  tween_v3_from   {};

    /** TweenV3 の終了値。 */
    FVec3  tween_v3_to     {};

    /** tween の補間に使うイージング関数。 */
    Easing::EasingFn ease = Easing::Linear;
};

/**
 * 時間付きアクションの連鎖を builder パターンで構築するシーケンス。
 *
 * @details
 * Wait / Call / FTween を連鎖して定義し、FSequenceRunner::Start に Move して実行する。
 * non-copy だが move 可能 (Runner が所有権を奪うため)。
 */
class FSequence {
public:
    /** 空のシーケンスを構築する。 */
    FSequence() noexcept = default;

    /** 破棄する。 */
    ~FSequence() noexcept = default;

    /** コピー禁止。 */
    FSequence(const FSequence&)            = delete;

    /** コピー代入も禁止。 */
    FSequence& operator=(const FSequence&) = delete;

    /** ムーブ構築 (Runner への所有権移動に使う)。 */
    FSequence(FSequence&&) noexcept            = default;

    /** ムーブ代入 (Runner への所有権移動に使う)。 */
    FSequence& operator=(FSequence&&) noexcept = default;

    /**
     * 指定秒だけ待つアクションを追加する。
     *
     * @param seconds 待機秒 (負値は 0 にクランプ)。
     * @return チェイン記述用の自身への参照。
     */
    FSequence& Wait(f32 seconds) noexcept;

    /**
     * コールバックを 1 回呼ぶアクションを追加する。
     *
     * @param fn 呼び出すコールバック関数ポインタ。
     * @param user fn に渡す user ポインタ。
     * @return チェイン記述用の自身への参照。
     */
    FSequence& Call(void(*fn)(void* user) noexcept, void* user = nullptr) noexcept;

    /**
     * f32 値を tween するアクションを追加する。
     *
     * @param target 補間して書き込む対象 f32 へのポインタ。
     * @param from 開始値。
     * @param to 終了値。
     * @param duration 補間にかける秒 (負値は 0 にクランプ)。
     * @param ease 補間に使うイージング関数 (nullptr なら Linear)。
     * @return チェイン記述用の自身への参照。
     */
    FSequence& FTween(f32* target,  f32  from, f32  to, f32 duration,
                     Easing::EasingFn ease = Easing::Linear) noexcept;

    /**
     * FVec2 値を tween するアクションを追加する。
     *
     * @param target 補間して書き込む対象 FVec2 へのポインタ。
     * @param from 開始値。
     * @param to 終了値。
     * @param duration 補間にかける秒 (負値は 0 にクランプ)。
     * @param ease 補間に使うイージング関数 (nullptr なら Linear)。
     * @return チェイン記述用の自身への参照。
     */
    FSequence& FTween(FVec2* target, FVec2 from, FVec2 to, f32 duration,
                     Easing::EasingFn ease = Easing::Linear) noexcept;

    /**
     * FVec3 値を tween するアクションを追加する。
     *
     * @param target 補間して書き込む対象 FVec3 へのポインタ。
     * @param from 開始値。
     * @param to 終了値。
     * @param duration 補間にかける秒 (負値は 0 にクランプ)。
     * @param ease 補間に使うイージング関数 (nullptr なら Linear)。
     * @return チェイン記述用の自身への参照。
     */
    FSequence& FTween(FVec3* target, FVec3 from, FVec3 to, f32 duration,
                     Easing::EasingFn ease = Easing::Linear) noexcept;

    /**
     * ループ回数を設定する。
     *
     * @param count ループ回数 (0 = 無限、既定 1 で 1 回再生して終了)。
     * @return チェイン記述用の自身への参照。
     */
    FSequence& Loop(u32 count) noexcept {
        m_LoopCount = count;
        return *this;
    }

    /**
     * 登録済みアクション列への const 参照を返す。
     *
     * @return アクション列。
     */
    const TArray<SeqAction>& Actions() const noexcept { return m_Actions; }

    /**
     * 設定済みのループ回数を返す。
     *
     * @return ループ回数 (0 = 無限)。
     */
    u32 LoopCount() const noexcept { return m_LoopCount; }

private:
    /** 登録されたアクション列 (実行順)。 */
    TArray<SeqAction> m_Actions;

    /** ループ回数 (0 = 無限)。 */
    u32              m_LoopCount = 1;
};

/**
 * 実行中シーケンスを参照する世代付きハンドル。
 */
struct SeqHandle {
    /** スロット配列のインデックス。 */
    u32  index      = 0xFFFFFFFFu;

    /** スロットの世代 (再利用検出用、0 は invalid)。 */
    u32  generation = 0;

    /**
     * ハンドルが有効値かを返す。
     *
     * @return generation != 0 なら true。
     */
    bool IsValid() const noexcept { return generation != 0; }
};

/**
 * 複数の FSequence を並列に実行・更新するランナー。
 *
 * @details
 * Start で FSequence の所有権を奪ってスロットに格納し、毎フレーム Tick で全スロットを
 * 進める。スロットは世代付きで再利用される。非コピー。
 */
class FSequenceRunner {
public:
    /** 空のランナーを構築する。 */
    FSequenceRunner() noexcept = default;

    /** 破棄する。 */
    ~FSequenceRunner() noexcept = default;

    /** コピー禁止。 */
    FSequenceRunner(const FSequenceRunner&)            = delete;

    /** コピー代入も禁止。 */
    FSequenceRunner& operator=(const FSequenceRunner&) = delete;

    /**
     * FSequence の所有権を奪って実行を開始する。
     *
     * @param seq 実行するシーケンス (所有権が移る)。
     * @return 実行中シーケンスのハンドル。空シーケンスは invalid handle。
     */
    SeqHandle Start(FSequence seq) noexcept;

    /**
     * 進行中のシーケンスを中止する。
     *
     * @details 進行中の tween は最後に書いた値で止まる (最終値への補正はしない)。
     * @param h 中止するシーケンスのハンドル。
     */
    void Cancel(SeqHandle h) noexcept;

    /** 全シーケンスを破棄する (Scene::OnExit などで使う)。 */
    void CancelAll() noexcept;

    /**
     * 指定ハンドルのシーケンスが実行中かを返す。
     *
     * @param h 照会するハンドル。
     * @return 実行中で世代も一致すれば true。
     */
    bool IsActive(SeqHandle h) const noexcept;

    /**
     * 実行中のシーケンス数を返す。
     *
     * @return アクティブなスロット数。
     */
    u32  ActiveCount() const noexcept { return m_ActiveCount; }

    /**
     * 全シーケンスを 1 フレーム進める。
     *
     * @details Scene::OnUpdate から FSceneClock::Dt() を渡して毎フレーム呼ぶ想定。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

private:
    /**
     * 1 シーケンスの実行状態を持つスロット。
     */
    struct Slot {
        /** このスロットが実行中なら true。 */
        bool active         = false;

        /** 現在の Call アクションを発火済みなら true (二重発火防止)。 */
        bool call_fired     = false;

        /** スロット世代 (Start ごとにインクリメント)。 */
        u32  generation     = 0;

        /** 実行中のシーケンス (所有)。 */
        FSequence seq;

        /** 現在実行中のアクションのインデックス。 */
        u32  action_idx     = 0;

        /** 現在のアクションの経過秒。 */
        f32  action_elapsed = 0.0f;

        /** これまでに完了したループ数。 */
        u32  loops_done     = 0;
    };

    /**
     * 空きスロットを確保する (再利用優先、無ければ追加)。
     *
     * @return 確保したスロットのインデックス。
     */
    u32  AcquireSlot() noexcept;

    /**
     * スロットを次のアクションへ進める (末尾ならループ判定 / 終了処理)。
     *
     * @param s 進めるスロット。
     */
    void AdvanceToNext(Slot& s) noexcept;

    /**
     * 完了したアクションの最終値を正確に書き込む (tween の浮動小数誤差を残さない)。
     *
     * @param s 対象スロット。
     * @param act 完了したアクション。
     */
    void FinishAction(Slot& s, const SeqAction& act) noexcept;

    /** シーケンス実行スロットの配列 (世代付きで再利用)。 */
    TArray<Slot> m_Slots;

    /** 実行中スロット数。 */
    u32         m_ActiveCount = 0;
};

} // namespace acs::game
