// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — DialogueSystem 実装
//
// 状態遷移:
//   NotStarted -> (Start) -> Typing -> (タイプ完了) -> Idle
//   Idle -> (choices あり) -> 選択待ち -> (ChooseOption) -> 次行 EnterLine
//   Idle -> (choices なし) -> (AdvanceLine or auto-advance) -> 次行 EnterLine
//   末尾行で AdvanceLine / auto-advance -> Completed
//
// タイプライタ:
//   _char_accum += dt * cps を毎フレーム加算し、floor して _visible_chars 化。
//   text 全長を超えたらクリップして _typing=false。
//   cps <= 0 は EnterLine 段階で _visible_chars = 全長 / _typing=false にする
//   (= 瞬時表示)。
#include "gameframework/DialogueSystem.h"

#include <cstring>   // strlen

namespace acs::game {

void DialogueSystem::AddLine(const DialogueLine& line) noexcept {
    _lines.PushBack(line);
}

void DialogueSystem::AddChoices(u32 at_line_index,
                                const DialogueChoice* choices, u32 count) noexcept {
    if (choices == nullptr || count == 0u) return;
    if (at_line_index >= static_cast<u32>(_lines.Size())) return;

    // 同 line への重複登録は無視 (= 上書き禁止、最初の登録のみ有効)
    for (usize i = 0; i < _choices_at.Size(); ++i) {
        if (_choices_at[i].line_index == at_line_index) return;
    }

    ChoicesAt rec;
    rec.line_index   = at_line_index;
    rec.choice_start = static_cast<u32>(_all_choices.Size());
    rec.choice_count = count;
    for (u32 i = 0; i < count; ++i) {
        _all_choices.PushBack(choices[i]);
    }
    _choices_at.PushBack(rec);
}

void DialogueSystem::Start() noexcept {
    if (_lines.Size() == 0) {
        // 行が無い場合は何もしない (caller は IsActive()==false を見て検知)
        _active    = false;
        _completed = false;
        return;
    }
    _active    = true;
    _completed = false;
    EnterLine(0);
}

void DialogueSystem::AdvanceLine() noexcept {
    if (!_active) return;
    if (_typing)  return;                 // タイプ未完では進めない
    if (HasChoicesPending()) return;      // 選択肢提示中は進めない (要 ChooseOption)

    const u32 next = _current_line_index + 1u;
    EnterLine(next);
}

void DialogueSystem::SkipTypewriter() noexcept {
    if (!_active) return;
    if (!_typing) return;
    _visible_chars = CurrentLineLength();
    _char_accum    = static_cast<f32>(_visible_chars);
    _typing        = false;
    _post_type_elapsed = 0.0f;            // auto-advance タイマ起点をリセット
}

void DialogueSystem::ChooseOption(u32 choice_index) noexcept {
    if (!_active) return;
    if (!HasChoicesPending()) return;

    const ChoicesAt* rec = FindChoicesForCurrent();
    if (rec == nullptr) return;            // HasChoicesPending と整合性で来ないはず
    if (choice_index >= rec->choice_count) return;

    const DialogueChoice& c = _all_choices[rec->choice_start + choice_index];
    _choices_consumed = true;              // 同 line で再提示しないように消費フラグ

    // next_line_index が範囲外なら終了扱い
    EnterLine(c.next_line_index);
}

void DialogueSystem::Reset() noexcept {
    _lines.Clear();
    _choices_at.Clear();
    _all_choices.Clear();
    _current_line_index = 0;
    _visible_chars      = 0;
    _char_accum         = 0.0f;
    _post_type_elapsed  = 0.0f;
    _active             = false;
    _completed          = false;
    _typing             = false;
    _choices_consumed   = true;
}

bool DialogueSystem::HasChoicesPending() const noexcept {
    if (!_active)         return false;
    if (_typing)          return false;    // タイプ完了するまで提示しない
    if (_choices_consumed) return false;
    return FindChoicesForCurrent() != nullptr;
}

const DialogueLine* DialogueSystem::CurrentLine() const noexcept {
    if (!_active) return nullptr;
    if (_current_line_index >= static_cast<u32>(_lines.Size())) return nullptr;
    return &_lines[_current_line_index];
}

u32 DialogueSystem::ChoiceCount() const noexcept {
    if (!HasChoicesPending()) return 0;
    const ChoicesAt* rec = FindChoicesForCurrent();
    return rec ? rec->choice_count : 0;
}

const DialogueChoice* DialogueSystem::Choices() const noexcept {
    if (!HasChoicesPending()) return nullptr;
    const ChoicesAt* rec = FindChoicesForCurrent();
    if (rec == nullptr) return nullptr;
    if (_all_choices.Size() == 0) return nullptr;
    return &_all_choices[rec->choice_start];
}

void DialogueSystem::Tick(f32 dt) noexcept {
    if (!_active) return;
    if (dt <= 0.0f) return;

    // ----- 1) タイプライタ進行 -----
    if (_typing) {
        const DialogueLine* line = CurrentLine();
        if (line != nullptr && line->type_speed_cps > 0.0f) {
            _char_accum += dt * line->type_speed_cps;
            const u32 total = CurrentLineLength();
            // floor して整数化
            u32 next_visible = static_cast<u32>(_char_accum);
            if (next_visible > total) next_visible = total;
            _visible_chars = next_visible;

            if (_visible_chars >= total) {
                _typing            = false;
                _post_type_elapsed = 0.0f;
            }
        } else {
            // 不正状態 (Typing == true なのに cps<=0) は瞬時完了に倒す
            _visible_chars     = CurrentLineLength();
            _typing            = false;
            _post_type_elapsed = 0.0f;
        }
        return; // タイプ中は auto-advance を評価しない (1 フレーム 1 進行)
    }

    // ----- 2) auto-advance -----
    // 選択肢 pending 中は auto-advance しない
    if (HasChoicesPending()) return;
    if (_auto_advance_delay <= 0.0f) return;

    _post_type_elapsed += dt;
    if (_post_type_elapsed >= _auto_advance_delay) {
        const u32 next = _current_line_index + 1u;
        EnterLine(next);
    }
}

void DialogueSystem::SetAutoAdvanceDelay(f32 delay_sec) noexcept {
    _auto_advance_delay = delay_sec > 0.0f ? delay_sec : 0.0f;
}

// ===== private =====

const DialogueSystem::ChoicesAt* DialogueSystem::FindChoicesForCurrent() const noexcept {
    for (usize i = 0; i < _choices_at.Size(); ++i) {
        if (_choices_at[i].line_index == _current_line_index) {
            return &_choices_at[i];
        }
    }
    return nullptr;
}

u32 DialogueSystem::CurrentLineLength() const noexcept {
    const DialogueLine* line = CurrentLine();
    if (line == nullptr || line->text == nullptr) return 0;
    return static_cast<u32>(::strlen(line->text));
}

void DialogueSystem::EnterLine(u32 new_index) noexcept {
    // 範囲外 (= 末尾の先 or ChooseOption が UINT32_MAX を返した) なら終了
    if (new_index >= static_cast<u32>(_lines.Size())) {
        _active             = false;
        _completed          = true;
        _typing             = false;
        _visible_chars      = 0;
        _char_accum         = 0.0f;
        _post_type_elapsed  = 0.0f;
        _choices_consumed   = true;
        return;
    }

    _current_line_index = new_index;
    _visible_chars      = 0;
    _char_accum         = 0.0f;
    _post_type_elapsed  = 0.0f;
    _choices_consumed   = false;          // この line に紐づく choices は未消費

    // cps <= 0 は瞬時表示
    const DialogueLine& line = _lines[new_index];
    if (line.type_speed_cps > 0.0f && line.text != nullptr && line.text[0] != '\0') {
        _typing        = true;
        _visible_chars = 0;
    } else {
        _typing        = false;
        _visible_chars = CurrentLineLength();
        _char_accum    = static_cast<f32>(_visible_chars);
    }
}

} // namespace acs::game
