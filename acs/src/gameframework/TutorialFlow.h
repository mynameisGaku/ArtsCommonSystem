// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CTutorialFlow
//
// 役割:
//   連続する「チュートリアルステップ」を順序付きで表示・完了判定し、ユーザー操作
//   または明示的な前進指示に応じて次ステップへ進めるシンプルな state machine。
//   描画は行わず、現在ステップへの const ポインタを公開するだけ。表示は呼び出し側
//   (UI レイヤ / AScene) が CurrentStep() を見て自前で描く。
//
// 設計上の方針:
//   ・**FSequence との棲み分け**: FSequence は時間ベースの自動進行 (cutscene 等)。
//     CTutorialFlow は「ユーザーが action を達成したら進む」という能動的進行で、
//     timer ベースではない (require_user_action=true の間は dt をいくら積んでも
//     自動 advance しない)。require_user_action=false なら表示後に AdvanceStep
//     を呼ぶだけで素直に次へ進む (ガイダンス表示 → OK ボタンなど)。
//   ・**所有しない const char***: ACS 規約通り <string> 禁止。id / message /
//     highlight_target は文字列リテラル or 長寿命バッファを想定し、寿命は呼び
//     出し側が保証する。
//   ・**非コピー・非ムーブ**: チュートリアルは通常 AScene につき 1 個の長寿命
//     オブジェクトで、誤コピーで state 分裂すると詰むため最初から禁止。
//   ・**Skip は不可逆**: Skip() を呼ぶと m_Completed=true / m_Active=false に
//     遷移し、Reset() しない限りどの query も終了扱い。誤って 2 度目を呼ばれ
//     ても no-op になるよう冪等。
//
// 使い方:
//   class FTutorialScene : public AScene {
//       CTutorialFlow m_Tut;
//       void OnEnter() noexcept override {
//           m_Tut.AddStep({"move",  "WASD で移動してみよう", "player", true});
//           m_Tut.AddStep({"jump",  "SPACE でジャンプ",       "player", true});
//           m_Tut.AddStep({"done",  "チュートリアル完了！",  nullptr,  false});
//           m_Tut.Start();
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Tut.Tick(dt);
//           if (input.JustPressed(EKey::W)) m_Tut.NotifyAction("move");
//           if (input.JustPressed(EKey::Space)) m_Tut.NotifyAction("jump");
//       }
//   };
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * チュートリアルステップ 1 件分。
 *
 * @details 全フィールドは非所有の const char* で、寿命は呼び出し側が保証する。
 */
struct FTutorialStep {
    /** NotifyAction で参照される識別子 (一意でなくてよいが、重複時は最初のマッチが採用される)。 */
    const char* id                  = nullptr;

    /** 表示用テキスト (UI 側が描画)。 */
    const char* message             = nullptr;

    /** UI ハイライト対象 (例: "player" / "inventory_button")。nullptr 可。本クラスは保持するだけで解釈はしない。 */
    const char* highlight_target    = nullptr;

    /**
     * ユーザー操作の達成を待つかのフラグ。
     *
     * @details
     * true の場合、NotifyAction で id と一致する action_id が来るか AdvanceStep が明示的に
     * 呼ばれるまで自動進行しない。false の場合は AdvanceStep 呼び出しのみで進む
     * (timer 等は Tick で扱わない、明示前進のみ)。
     */
    bool        require_user_action = false;
};

/**
 * 順序付きチュートリアルステップを表示・前進させる軽量 state machine。
 *
 * @details
 * 描画は行わず、現在ステップへの const ポインタを公開するだけ。表示は呼び出し側
 * (UI レイヤ / AScene) が CurrentStep() を見て自前で描く。時間ベースで自動進行する
 * FSequence と異なり「ユーザーが action を達成したら進む」能動的進行で、
 * require_user_action=true の間は dt をいくら積んでも自動 advance しない。
 * 全フィールドは非所有 const char* (寿命は呼び出し側保証)。非コピー・非ムーブ。
 * Skip は不可逆で冪等。
 */
class CTutorialFlow {
public:
    /** 空のチュートリアルフローを構築する (ステップ未登録)。 */
    CTutorialFlow()  noexcept = default;

    /** 破棄する。 */
    ~CTutorialFlow() noexcept = default;

    /** コピー禁止 (state 分裂を避けるため。通常は AScene につき 1 個の長寿命オブジェクト)。 */
    CTutorialFlow(const CTutorialFlow&)            = delete;

    /** コピー代入も禁止。 */
    CTutorialFlow& operator=(const CTutorialFlow&) = delete;

    /** ムーブ禁止 (state 分裂を避けるため)。 */
    CTutorialFlow(CTutorialFlow&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CTutorialFlow& operator=(CTutorialFlow&&)      = delete;

    /**
     * ステップを末尾に追加する。
     *
     * @details
     * Start 前のみ呼ぶ想定だが、走行中に追加されても末尾に積まれるだけで現在進行には影響しない。
     * @param step 追加するチュートリアルステップ。
     */
    void AddStep(const FTutorialStep& step) noexcept;

    /**
     * ステップ 0 から表示を開始する。
     *
     * @details
     * ステップが 1 件も無い場合は m_Completed=true に遷移して即終了する (空チュートリアルの no-op)。
     * 複数回呼ばれた場合は Reset 相当の再初期化を行う。
     */
    void Start() noexcept;

    /**
     * 次のステップへ手動で前進する。
     *
     * @details
     * require_user_action のステップで条件達成後、または require_user_action=false のステップで
     * ガイダンス確認後に呼ぶ。最終ステップで呼ばれた場合は m_Completed=true に遷移する。
     * inactive 状態 (Start 前 / Skip 後 / 完了後) の呼び出しは no-op。
     */
    void AdvanceStep() noexcept;

    /**
     * ユーザーが action を実行したことを通知する。
     *
     * @details
     * 現在ステップの id と一致し require_user_action=true なら自動的に AdvanceStep を呼ぶ。
     * action_id == nullptr / inactive 状態 / 不一致は no-op。
     * @param action_id 実行された action の識別子。
     */
    void NotifyAction(const char* action_id) noexcept;

    /** ステップ定義を保持したまま走行 state を初期化する (Start 前と同じ状態)。 */
    void Reset() noexcept;

    /** チュートリアル全体を skip して完了扱いにする (冪等、2 度目以降 no-op)。 */
    void Skip() noexcept;

    /**
     * 走行中かを返す。
     *
     * @return Start 後・Skip / 完了前なら true。
     */
    bool IsActive() const noexcept { return m_Active; }

    /**
     * 完了済みかを返す。
     *
     * @return 全ステップを通過 or Skip 済みなら true。
     */
    bool IsCompleted() const noexcept { return m_Completed; }

    /**
     * 現在のステップ番号を返す。
     *
     * @return 現在のステップインデックス (Start 前は 0)。
     */
    u32 CurrentStepIndex() const noexcept { return m_CurrentStep; }

    /**
     * 描画 / 表示用に現在ステップへのポインタを返す。
     *
     * @return 現在ステップへのポインタ。Start 前・Skip 後・完了後は nullptr。
     */
    const FTutorialStep* CurrentStep() const noexcept;

    /**
     * 登録済みステップ数を返す。
     *
     * @return 登録済みステップ数。
     */
    u32 StepCount() const noexcept;

    /**
     * 毎フレーム呼ぶ state 確認用 hook。
     *
     * @details
     * 現状は内部 timer を持たないが、将来の auto-advance ステップ / hint 出現遅延等のために
     * dt を受け取る noexcept fn として予約している。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

private:
    /** 登録済みステップ列。 */
    TArray<FTutorialStep> m_Steps;

    /** 現在のステップインデックス。 */
    u32                 m_CurrentStep = 0;

    /** 走行中フラグ。 */
    bool                m_Active       = false;

    /** 完了済みフラグ。 */
    bool                m_Completed    = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FTutorialFlow = CTutorialFlow;

} // namespace acs::game
