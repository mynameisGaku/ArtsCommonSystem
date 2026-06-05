// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — FDialogueSystem 実装
//
// 状態遷移:
//   NotStarted -> (Start) -> Typing -> (タイプ完了) -> Idle
//   Idle -> (choices あり) -> 選択待ち -> (ChooseOption) -> 次行 EnterLine
//   Idle -> (choices なし) -> (AdvanceLine or auto-advance) -> 次行 EnterLine
//   末尾行で AdvanceLine / auto-advance -> Completed
//
// タイプライタ:
//   m_CharAccum += dt * cps を毎フレーム加算し、floor して m_VisibleChars 化。
//   text 全長を超えたらクリップして m_Typing=false。
//   cps <= 0 は EnterLine 段階で m_VisibleChars = 全長 / m_Typing=false にする
//   (= 瞬時表示)。
#include "gameframework/DialogueSystem.h"

#include <cstring>   // strlen

namespace acs::game {

/** 末尾に会話行を 1 行追加する。 */
void FDialogueSystem::AddLine(const DialogueLine& line) noexcept {
    m_Lines.PushBack(line);
}

/** 指定行に選択肢を紐づける (同 line への重複登録は無視)。 */
void FDialogueSystem::AddChoices(u32 at_line_index,
                                const DialogueChoice* choices, u32 count) noexcept {
    if (choices == nullptr || count == 0u) return;
    if (at_line_index >= static_cast<u32>(m_Lines.Size())) return;

    // 同 line への重複登録は無視 (= 上書き禁止、最初の登録のみ有効)
    for (usize i = 0; i < m_ChoicesAt.Size(); ++i) {
        if (m_ChoicesAt[i].line_index == at_line_index) return;
    }

    ChoicesAt rec;
    rec.line_index   = at_line_index;
    rec.choice_start = static_cast<u32>(m_AllChoices.Size());
    rec.choice_count = count;
    for (u32 i = 0; i < count; ++i) {
        m_AllChoices.PushBack(choices[i]);
    }
    m_ChoicesAt.PushBack(rec);
}

/** 会話を開始し先頭行へ入る (行が無ければ非アクティブのまま)。 */
void FDialogueSystem::Start() noexcept {
    if (m_Lines.Size() == 0) {
        // 行が無い場合は何もしない (caller は IsActive()==false を見て検知)
        m_Active    = false;
        m_Completed = false;
        return;
    }
    m_Active    = true;
    m_Completed = false;
    EnterLine(0);
}

/** 次行へ進める (タイプ未完・選択肢提示中は無視)。 */
void FDialogueSystem::AdvanceLine() noexcept {
    if (!m_Active) return;
    if (m_Typing)  return;                 // タイプ未完では進めない
    if (HasChoicesPending()) return;      // 選択肢提示中は進めない (要 ChooseOption)

    const u32 next = m_CurrentLineIndex + 1u;
    EnterLine(next);
}

/** タイプライタ演出を打ち切り現在行を即座に全表示する。 */
void FDialogueSystem::SkipTypewriter() noexcept {
    if (!m_Active) return;
    if (!m_Typing) return;
    m_VisibleChars = CurrentLineLength();
    m_CharAccum    = static_cast<f32>(m_VisibleChars);
    m_Typing        = false;
    m_PostTypeElapsed = 0.0f;            // auto-advance タイマ起点をリセット
}

/** 提示中の選択肢を 1 つ選び、その分岐先行へ遷移する。 */
void FDialogueSystem::ChooseOption(u32 choice_index) noexcept {
    if (!m_Active) return;
    if (!HasChoicesPending()) return;

    const ChoicesAt* rec = FindChoicesForCurrent();
    if (rec == nullptr) return;            // HasChoicesPending と整合性で来ないはず
    if (choice_index >= rec->choice_count) return;

    const DialogueChoice& c = m_AllChoices[rec->choice_start + choice_index];
    m_bChoicesConsumed = true;              // 同 line で再提示しないように消費フラグ

    // next_line_index が範囲外なら終了扱い
    EnterLine(c.next_line_index);
}

/** 全行・選択肢・進行状態をクリアして初期状態に戻す。 */
void FDialogueSystem::Reset() noexcept {
    m_Lines.Clear();
    m_ChoicesAt.Clear();
    m_AllChoices.Clear();
    m_CurrentLineIndex = 0;
    m_VisibleChars      = 0;
    m_CharAccum         = 0.0f;
    m_PostTypeElapsed  = 0.0f;
    m_Active             = false;
    m_Completed          = false;
    m_Typing             = false;
    m_bChoicesConsumed   = true;
}

/** 現在行に未消費の選択肢が提示待ちかを返す。 */
bool FDialogueSystem::HasChoicesPending() const noexcept {
    if (!m_Active)         return false;
    if (m_Typing)          return false;    // タイプ完了するまで提示しない
    if (m_bChoicesConsumed) return false;
    return FindChoicesForCurrent() != nullptr;
}

/** 現在表示中の会話行を返す (非アクティブ・範囲外なら nullptr)。 */
const DialogueLine* FDialogueSystem::CurrentLine() const noexcept {
    if (!m_Active) return nullptr;
    if (m_CurrentLineIndex >= static_cast<u32>(m_Lines.Size())) return nullptr;
    return &m_Lines[m_CurrentLineIndex];
}

/** 現在提示中の選択肢の個数を返す (提示中でなければ 0)。 */
u32 FDialogueSystem::ChoiceCount() const noexcept {
    if (!HasChoicesPending()) return 0;
    const ChoicesAt* rec = FindChoicesForCurrent();
    return rec ? rec->choice_count : 0;
}

/** 現在提示中の選択肢配列の先頭を返す (提示中でなければ nullptr)。 */
const DialogueChoice* FDialogueSystem::Choices() const noexcept {
    if (!HasChoicesPending()) return nullptr;
    const ChoicesAt* rec = FindChoicesForCurrent();
    if (rec == nullptr) return nullptr;
    if (m_AllChoices.Size() == 0) return nullptr;
    return &m_AllChoices[rec->choice_start];
}

/** 毎フレーム呼び、タイプライタ進行と auto-advance を処理する。 */
void FDialogueSystem::Tick(f32 dt) noexcept {
    if (!m_Active) return;
    if (dt <= 0.0f) return;

    // 1) タイプライタ進行
    if (m_Typing) {
        const DialogueLine* line = CurrentLine();
        if (line != nullptr && line->type_speed_cps > 0.0f) {
            m_CharAccum += dt * line->type_speed_cps;
            const u32 total = CurrentLineLength();
            // floor して整数化
            u32 next_visible = static_cast<u32>(m_CharAccum);
            if (next_visible > total) next_visible = total;
            m_VisibleChars = next_visible;

            if (m_VisibleChars >= total) {
                m_Typing            = false;
                m_PostTypeElapsed = 0.0f;
            }
        } else {
            // 不正状態 (Typing == true なのに cps<=0) は瞬時完了に倒す
            m_VisibleChars     = CurrentLineLength();
            m_Typing            = false;
            m_PostTypeElapsed = 0.0f;
        }
        return; // タイプ中は auto-advance を評価しない (1 フレーム 1 進行)
    }

    // 2) auto-advance
    // 選択肢 pending 中は auto-advance しない
    if (HasChoicesPending()) return;
    if (m_AutoAdvanceDelay <= 0.0f) return;

    m_PostTypeElapsed += dt;
    if (m_PostTypeElapsed >= m_AutoAdvanceDelay) {
        const u32 next = m_CurrentLineIndex + 1u;
        EnterLine(next);
    }
}

/** auto-advance までの待ち秒を設定する (0 以下で auto-advance 無効)。 */
void FDialogueSystem::SetAutoAdvanceDelay(f32 delay_sec) noexcept {
    m_AutoAdvanceDelay = delay_sec > 0.0f ? delay_sec : 0.0f;
}

/** 現在行に紐づく選択肢レコードを線形検索で返す (無ければ nullptr)。 */
const FDialogueSystem::ChoicesAt* FDialogueSystem::FindChoicesForCurrent() const noexcept {
    for (usize i = 0; i < m_ChoicesAt.Size(); ++i) {
        if (m_ChoicesAt[i].line_index == m_CurrentLineIndex) {
            return &m_ChoicesAt[i];
        }
    }
    return nullptr;
}

/** 現在行のテキスト長 (文字数) を返す (行・テキストが無ければ 0)。 */
u32 FDialogueSystem::CurrentLineLength() const noexcept {
    const DialogueLine* line = CurrentLine();
    if (line == nullptr || line->text == nullptr) return 0;
    return static_cast<u32>(::strlen(line->text));
}

/** 指定行に入って表示状態を初期化する (範囲外なら完了扱い)。 */
void FDialogueSystem::EnterLine(u32 new_index) noexcept {
    // 範囲外 (= 末尾の先 or ChooseOption が UINT32_MAX を返した) なら終了
    if (new_index >= static_cast<u32>(m_Lines.Size())) {
        m_Active             = false;
        m_Completed          = true;
        m_Typing             = false;
        m_VisibleChars      = 0;
        m_CharAccum         = 0.0f;
        m_PostTypeElapsed  = 0.0f;
        m_bChoicesConsumed   = true;
        return;
    }

    m_CurrentLineIndex = new_index;
    m_VisibleChars      = 0;
    m_CharAccum         = 0.0f;
    m_PostTypeElapsed  = 0.0f;
    m_bChoicesConsumed   = false;          // この line に紐づく choices は未消費

    // cps <= 0 は瞬時表示
    const DialogueLine& line = m_Lines[new_index];
    if (line.type_speed_cps > 0.0f && line.text != nullptr && line.text[0] != '\0') {
        m_Typing        = true;
        m_VisibleChars = 0;
    } else {
        m_Typing        = false;
        m_VisibleChars = CurrentLineLength();
        m_CharAccum    = static_cast<f32>(m_VisibleChars);
    }
}

} // namespace acs::game
