// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — FDialogueSystem
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
//   ・**文字列を所有しない**: DialogueLine::text は const char* で参照のみ持つ。
//     scenario データは literal / バンドル等で別に管理する想定 (STL <string> 禁止)。
//   ・**type_speed_cps <= 0 は瞬時表示**: 計算分岐を 1 ヶ所に集約。
//   ・**choices は別 TArray**: ChoicesAt は (line_index, choice_start, choice_count)
//     のみ持ち、実選択肢は `m_AllChoices` から slice する。挿入順に並ぶ前提で
//     線形検索 (典型シナリオで N < 数百なので問題なし)。
//   ・**auto-advance は per-line ではなく system 共通の delay**: 仕様簡素化。
//     選択肢が登録されている line では auto-advance 抑止。
//   ・**非コピー・非ムーブ**: state holder の唯一性 (現在行 / タイプ進行) を
//     担保するため。
//
// 参考: FSpriteAnimator (frame-based progression), FSequence (action 連鎖)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 1 行のダイアログ。文字列は所有しない (literal / 外部バンドル参照)。
struct DialogueLine {
    const char* speaker        = nullptr;  // 発話者名 (nullptr 可)
    const char* text           = nullptr;  // 本文 (nullptr は空行扱い)
    f32         type_speed_cps = 30.0f;    // タイプ速度 (chars per second)、<=0 で瞬時
};

// 選択肢 1 つ。next_line_index が範囲外 (= line 数以上) ならダイアログ終了。
struct DialogueChoice {
    const char* text            = nullptr;
    u32         next_line_index = 0xFFFFFFFFu;  // 範囲外で「終了」を表現
};

class FDialogueSystem {
public:
    FDialogueSystem() noexcept = default;
    ~FDialogueSystem() noexcept = default;

    // 進行状態の唯一性を担保するため非コピー・非ムーブ
    FDialogueSystem(const FDialogueSystem&)            = delete;
    FDialogueSystem& operator=(const FDialogueSystem&) = delete;
    FDialogueSystem(FDialogueSystem&&)                 = delete;
    FDialogueSystem& operator=(FDialogueSystem&&)      = delete;

    // ----- セットアップ -----
    // 行を末尾に追加。挿入順に再生される。
    void AddLine(const DialogueLine& line) noexcept;

    // at_line_index 番の行が表示完了したあとに提示する選択肢群を登録。
    // 同じ line に複数回 AddChoices すると 2 回目以降は無視 (= 上書き禁止)。
    // count == 0 / choices == nullptr / at_line_index 範囲外 は no-op。
    void AddChoices(u32 at_line_index,
                    const DialogueChoice* choices, u32 count) noexcept;

    // ----- 再生制御 -----
    // 先頭行から再生開始。lines が空なら no-op (= IsActive() は false のまま)。
    void Start() noexcept;

    // 次の行へ。タイプが完了していて、かつ choices が pending でないときのみ進む。
    // 末尾行で呼ぶと m_Completed = true / m_Active = false に遷移。
    void AdvanceLine() noexcept;

    // 現在行を即座に全文表示 (タイプ中なら強制完了)。
    void SkipTypewriter() noexcept;

    // 分岐選択を確定。choice_index は ChoiceCount() 未満必須、範囲外は no-op。
    // 確定後、選択肢の next_line_index にジャンプする (範囲外なら終了)。
    void ChooseOption(u32 choice_index) noexcept;

    // ダイアログ全体をリセット (登録済み line / choices も破棄)。
    void Reset() noexcept;

    // ----- 進行確認 -----
    bool IsActive()    const noexcept { return m_Active; }
    bool IsCompleted() const noexcept { return m_Completed; }
    // 「タイプが完了し」「選択肢が登録されていて」「まだ選択されていない」状態
    bool HasChoicesPending() const noexcept;
    // タイプライタが進行中か (= m_VisibleChars < 全長)
    bool IsTyping() const noexcept { return m_Active && m_Typing; }

    // ----- 現在状態のアクセサ -----
    // 非アクティブ時は nullptr。
    const DialogueLine* CurrentLine() const noexcept;

    // タイプライタで現在見えている文字数 (text の先頭から)。
    u32 VisibleCharCount() const noexcept { return m_VisibleChars; }

    // pending な選択肢の本数 / 配列ポインタ。pending でなければ 0 / nullptr。
    u32                   ChoiceCount() const noexcept;
    const DialogueChoice* Choices()     const noexcept;

    // ----- フレーム更新 -----
    // dt 秒進める。タイプライタ進行 / auto-advance を行う。
    // dt <= 0 や非アクティブ時は no-op。
    void Tick(f32 dt) noexcept;

    // auto-advance を有効化したいときに呼ぶ。delay <= 0 で無効化 (= 既定)。
    // 選択肢が pending な line では発火しない。
    void SetAutoAdvanceDelay(f32 delay_sec) noexcept;
    f32  AutoAdvanceDelay() const noexcept { return m_AutoAdvanceDelay; }

private:
    // 「line_index 直後に提示する選択肢」の範囲記録
    struct ChoicesAt {
        u32 line_index   = 0;
        u32 choice_start = 0;
        u32 choice_count = 0;
    };

    // m_CurrentLineIndex に対応する ChoicesAt を返す (なければ nullptr)。
    const ChoicesAt* FindChoicesForCurrent() const noexcept;

    // 現在行の全文字数 (text == nullptr のときは 0)。
    u32 CurrentLineLength() const noexcept;

    // current_line が変わったときの初期化 (visible_chars=0, typing=true 等)。
    // 末尾を超えていれば m_Completed=true、m_Active=false にする。
    void EnterLine(u32 new_index) noexcept;

    TArray<DialogueLine>   m_Lines;
    TArray<ChoicesAt>      m_ChoicesAt;     // line_index 昇順想定 (線形検索)
    TArray<DialogueChoice> m_AllChoices;    // 全 choice をフラットに保持

    u32  m_CurrentLineIndex = 0;
    u32  m_VisibleChars      = 0;
    f32  m_CharAccum         = 0.0f;       // 累積 char 単位 (整数化されて m_VisibleChars 化)
    f32  m_PostTypeElapsed  = 0.0f;       // タイプ完了後の経過 (auto-advance 用)

    f32  m_AutoAdvanceDelay = 0.0f;       // <= 0 で無効

    bool m_Active             = false;
    bool m_Completed          = false;
    bool m_Typing             = false;
    // 選択肢が pending なときに choice_consumed=false。ChooseOption で true 化。
    bool m_bChoicesConsumed   = true;
};

} // namespace acs::game
