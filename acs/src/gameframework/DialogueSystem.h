// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — CDialogueSystem
//
// シナリオ / NPC 会話 / イベントシーンで使うダイアログ駆動 state holder。
// 1 行ずつテキストを送り出し、タイプライタ演出と分岐選択肢を扱う。
// 描画 / 入力 / 音声には触らず、「今どの行が」「何文字まで見えていて」
// 「選択肢を提示中か」を保持するだけ。実描画は caller (UI 層) の責任。
//
// 役割:
//   ・行コレクションの保持 (AddLine で順次追加)
//   ・分岐選択肢の登録 (AddChoices: 特定 line 直後に提示する選択肢群)
//   ・タイプライタ演出 (cps = chars per second, Tick で m_VisibleChars 増)
//   ・auto-advance: line に紐づく choices が無く、m_AutoAdvanceDelay > 0 の
//     ときは「タイプ完了 + 一定秒経過」で勝手に次行へ進む
//   ・分岐選択 (ChooseOption: choices[i].next_line_index にジャンプ)
//
// 設計選択:
//   ・**文字列を所有しない**: FDialogueLine::text は const char* で参照のみ持つ。
//     scenario データは literal / バンドル等で別に管理する想定 (STL <string> 禁止)。
//   ・**type_speed_cps <= 0 は瞬時表示**: 計算分岐を 1 ヶ所に集約。
//   ・**choices は別 TArray**: FChoicesAt は (line_index, choice_start, choice_count)
//     のみ持ち、実選択肢は `m_AllChoices` から slice する。挿入順に並ぶ前提で
//     線形検索 (典型シナリオで N < 数百なので問題なし)。
//   ・**auto-advance は per-line ではなく system 共通の delay**: 仕様簡素化。
//     選択肢が登録されている line では auto-advance 抑止。
//   ・**非コピー・非ムーブ**: state holder の唯一性 (現在行 / タイプ進行) を
//     担保するため。
//
// 関連実装: CSpriteAnimator (frame-based progression), FSequence (action 連鎖)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 1 行のダイアログ。
 *
 * @details 文字列は所有しない (literal / 外部バンドル参照)。
 */
struct FDialogueLine {
    /** 発話者名 (nullptr 可)。 */
    const char* speaker        = nullptr;

    /** 本文 (nullptr は空行扱い)。 */
    const char* text           = nullptr;

    /** タイプ速度 (chars per second)、<=0 で瞬時。 */
    f32         type_speed_cps = 30.0f;
};

/**
 * 1 つの選択肢。
 *
 * @details next_line_index が範囲外 (= line 数以上) ならダイアログ終了を表現する。
 */
struct FDialogueChoice {
    /** 選択肢の表示テキスト。 */
    const char* text            = nullptr;

    /** 選択時のジャンプ先 line index (範囲外で「終了」を表現)。 */
    u32         next_line_index = 0xFFFFFFFFu;
};

/**
 * ダイアログ駆動の state holder (タイプライタ演出 + 分岐選択肢)。
 *
 * @details
 * 1 行ずつテキストを送り出し、タイプライタ演出 (cps = chars per second) と分岐
 * 選択肢を扱う。描画 / 入力 / 音声には触れず、「今どの行が」「何文字まで見えていて」
 * 「選択肢を提示中か」だけを保持する。auto-advance は system 共通の delay で、
 * 選択肢が登録されている line では抑止される。非コピー・非ムーブ。
 */
class CDialogueSystem {
public:
    /** 空状態で構築する。 */
    CDialogueSystem() noexcept = default;

    /** 破棄する。 */
    ~CDialogueSystem() noexcept = default;

    /** コピー禁止 (進行状態の唯一性を担保するため)。 */
    CDialogueSystem(const CDialogueSystem&)            = delete;

    /** コピー代入も禁止。 */
    CDialogueSystem& operator=(const CDialogueSystem&) = delete;

    /** ムーブ禁止 (進行状態の唯一性を担保するため)。 */
    CDialogueSystem(CDialogueSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CDialogueSystem& operator=(CDialogueSystem&&)      = delete;

    /**
     * 行を末尾に追加する。
     *
     * @details 挿入順に再生される。
     * @param line 追加するダイアログ行。
     */
    void AddLine(const FDialogueLine& line) noexcept;

    /**
     * at_line_index 番の行が表示完了したあとに提示する選択肢群を登録する。
     *
     * @details
     * 同じ line に複数回登録すると 2 回目以降は無視 (= 上書き禁止)。
     * count == 0 / choices == nullptr / at_line_index 範囲外 は no-op。
     * @param at_line_index 選択肢を紐づける行の index。
     * @param choices 登録する選択肢配列。
     * @param count choices の要素数。
     */
    void AddChoices(u32 at_line_index,
                    const FDialogueChoice* choices, u32 count) noexcept;

    /**
     * 先頭行から再生を開始する。
     *
     * @details lines が空なら no-op (= IsActive() は false のまま)。
     */
    void Start() noexcept;

    /**
     * 次の行へ進める。
     *
     * @details
     * タイプが完了していて、かつ choices が pending でないときのみ進む。末尾行で
     * 呼ぶと m_Completed = true / m_Active = false に遷移する。
     */
    void AdvanceLine() noexcept;

    /**
     * 現在行を即座に全文表示する (タイプ中なら強制完了)。
     */
    void SkipTypewriter() noexcept;

    /**
     * 分岐選択を確定する。
     *
     * @details
     * choice_index は ChoiceCount() 未満必須、範囲外は no-op。確定後、選択肢の
     * next_line_index にジャンプする (範囲外なら終了)。
     * @param choice_index 選択した選択肢の index。
     */
    void ChooseOption(u32 choice_index) noexcept;

    /**
     * ダイアログ全体をリセットする (登録済み line / choices も破棄)。
     */
    void Reset() noexcept;

    /**
     * アクティブかを返す。
     *
     * @return 再生中なら true。
     */
    bool IsActive()    const noexcept { return m_Active; }

    /**
     * 完了済みかを返す。
     *
     * @return 末尾行まで再生完了していれば true。
     */
    bool IsCompleted() const noexcept { return m_Completed; }

    /**
     * 選択肢が pending かを返す。
     *
     * @return タイプ完了かつ選択肢が登録されており未選択なら true。
     */
    bool HasChoicesPending() const noexcept;

    /**
     * タイプライタが進行中かを返す。
     *
     * @return アクティブかつタイプ中 (m_VisibleChars < 全長) なら true。
     */
    bool IsTyping() const noexcept { return m_Active && m_Typing; }

    /**
     * 現在行を返す。
     *
     * @return 現在行へのポインタ (非アクティブ時は nullptr)。
     */
    const FDialogueLine* CurrentLine() const noexcept;

    /**
     * タイプライタで現在見えている文字数を返す。
     *
     * @return text の先頭から見えている文字数。
     */
    u32 VisibleCharCount() const noexcept { return m_VisibleChars; }

    /**
     * pending な選択肢の本数を返す。
     *
     * @return pending な選択肢数 (pending でなければ 0)。
     */
    u32                   ChoiceCount() const noexcept;

    /**
     * pending な選択肢配列の先頭ポインタを返す。
     *
     * @return 選択肢配列の先頭 (pending でなければ nullptr)。
     */
    const FDialogueChoice* Choices()     const noexcept;

    /**
     * dt 秒ぶん時間を進める。
     *
     * @details タイプライタ進行 / auto-advance を行う。dt <= 0 や非アクティブ時は no-op。
     * @param dt 経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * auto-advance の遅延秒を設定する。
     *
     * @details delay <= 0 で無効化 (= 既定)。選択肢が pending な line では発火しない。
     * @param delay_sec タイプ完了後に自動進行するまでの秒 (<=0 で無効)。
     */
    void SetAutoAdvanceDelay(f32 delay_sec) noexcept;

    /**
     * auto-advance の遅延秒を返す。
     *
     * @return 設定済みの遅延秒 (<=0 で無効)。
     */
    f32  AutoAdvanceDelay() const noexcept { return m_AutoAdvanceDelay; }

private:
    /**
     * line_index 直後に提示する選択肢群の範囲記録。
     */
    struct FChoicesAt {
        /** 選択肢を紐づける行の index。 */
        u32 line_index   = 0;

        /** m_AllChoices 内の先頭 index。 */
        u32 choice_start = 0;

        /** この行に紐づく選択肢の本数。 */
        u32 choice_count = 0;
    };

    /**
     * 現在行に対応する FChoicesAt を返す。
     *
     * @return 現在行に紐づく FChoicesAt (なければ nullptr)。
     */
    const FChoicesAt* FindChoicesForCurrent() const noexcept;

    /**
     * 現在行の全文字数を返す。
     *
     * @return 現在行の文字数 (text == nullptr のときは 0)。
     */
    u32 CurrentLineLength() const noexcept;

    /**
     * 指定 index の行へ遷移して状態を初期化する。
     *
     * @details
     * visible_chars=0 / typing=true 等を設定する。末尾を超えていれば
     * m_Completed=true、m_Active=false にする。
     * @param new_index 遷移先の行 index。
     */
    void EnterLine(u32 new_index) noexcept;

    /** 登録済みダイアログ行 (挿入順)。 */
    TArray<FDialogueLine>   m_Lines;

    /** 行ごとの選択肢範囲記録 (line_index 昇順想定、線形検索)。 */
    TArray<FChoicesAt>      m_ChoicesAt;

    /** 全選択肢をフラットに保持する配列。 */
    TArray<FDialogueChoice> m_AllChoices;

    /** 現在再生中の行 index。 */
    u32  m_CurrentLineIndex = 0;

    /** タイプライタで現在見えている文字数。 */
    u32  m_VisibleChars      = 0;

    /** 累積 char 単位 (整数化されて m_VisibleChars 化)。 */
    f32  m_CharAccum         = 0.0f;

    /** タイプ完了後の経過秒 (auto-advance 用)。 */
    f32  m_PostTypeElapsed  = 0.0f;

    /** auto-advance の遅延秒 (<= 0 で無効)。 */
    f32  m_AutoAdvanceDelay = 0.0f;

    /** アクティブフラグ。 */
    bool m_Active             = false;

    /** 完了フラグ (末尾行まで再生済み)。 */
    bool m_Completed          = false;

    /** タイプライタ進行中フラグ。 */
    bool m_Typing             = false;

    /** 現在行の選択肢を消費済みか (ChooseOption で true 化)。 */
    bool m_bChoicesConsumed   = true;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FDialogueSystem = CDialogueSystem;

} // namespace acs::game
