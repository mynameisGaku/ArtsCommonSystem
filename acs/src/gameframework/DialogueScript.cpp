// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (visual novel) — DialogueScript 実装
//
// 状態遷移:
//   Idle -> (LoadScript) -> Idle
//   Idle -> (Start)      -> Playing (即進行系を消化) -> {AwaitingInput | AwaitingChoice | Finished}
//   AwaitingInput  -> (Advance)      -> Playing -> 次の停止ポイントへ
//   AwaitingChoice -> (SelectChoice) -> Playing (Jump 後) -> 次の停止ポイントへ
//   Playing (Wait 中) -> (Tick で _wait_remaining <= 0) -> 次 op へ
//   いずれの状態 -> (Stop)     -> Idle
//   いずれの状態 -> (ClearAll) -> Idle + データ全消去 + callback クリア
//
// 即進行系 (Show / Hide / Background / PlayBgm / StopBgm / PlaySe / Jump):
//   RunUntilBlocked のループ内で callback を発火して _current_op_index を進める。
//   フレームをまたがず一気に消化する (= caller から見ると Say / Choice / Wait /
//   EndScene が「停止ポイント」)。
//
// Choice 群展開:
//   _current_op_index が Choice op を指していたら、連続する Choice op 群を
//   _current_choices に丸ごと積んで AwaitingChoice に遷移する。SelectChoice は
//   choice_index で jump_label を解決し、_current_op_index をジャンプ先に
//   セットして再び RunUntilBlocked を回す。
#include "gameframework/DialogueScript.h"

#include <cstring>   // strcmp

namespace acs::game {

namespace {

// ラベル名比較 (両方 nullptr は match とは扱わない)。
inline bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    return ::strcmp(a, b) == 0;
}

constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

} // namespace

// ===== public =====

void DialogueScript::Init() noexcept {
    _current_choices.Clear();
    _current_op_index = 0u;
    _wait_remaining   = 0.0f;
    _state            = DialogueScriptState::Idle;
}

void DialogueScript::LoadScript(const ScriptOp* ops, u32 op_count, const char* script_id) noexcept {
    _ops.Clear();
    _labels.Clear();
    _current_choices.Clear();
    _script_id        = script_id;
    _current_op_index = 0u;
    _wait_remaining   = 0.0f;
    _state            = DialogueScriptState::Idle;

    if (ops == nullptr || op_count == 0u) return;

    _ops.Reserve(op_count);
    for (u32 i = 0; i < op_count; ++i) {
        _ops.PushBack(ops[i]);
    }
}

void DialogueScript::AddLabel(const char* label, u32 op_index) noexcept {
    if (label == nullptr) return;
    if (op_index >= static_cast<u32>(_ops.Size())) return;

    // 同名は最初の登録のみ有効 (上書き禁止)
    for (usize i = 0; i < _labels.Size(); ++i) {
        if (StrEq(_labels[i].label, label)) return;
    }

    LabelEntry e;
    e.label    = label;
    e.op_index = op_index;
    _labels.PushBack(e);
}

void DialogueScript::Start(const char* start_label) noexcept {
    if (_ops.Size() == 0) {
        // 空スクリプト: 即 Finished にして End callback を発火
        _state = DialogueScriptState::Playing;
        EnterFinished();
        return;
    }

    u32 start_index = 0u;
    if (start_label != nullptr) {
        const u32 resolved = ResolveLabel(start_label);
        if (resolved == kInvalidIndex) {
            // ラベル未解決: 仕様簡素化のため即 Finished に倒す
            _state = DialogueScriptState::Playing;
            EnterFinished();
            return;
        }
        start_index = resolved;
    }

    _current_op_index = start_index;
    _wait_remaining   = 0.0f;
    _current_choices.Clear();
    _state            = DialogueScriptState::Playing;
    RunUntilBlocked();
}

void DialogueScript::Stop() noexcept {
    _state            = DialogueScriptState::Idle;
    _current_op_index = 0u;
    _wait_remaining   = 0.0f;
    _current_choices.Clear();
}

void DialogueScript::ClearAll() noexcept {
    _ops.Clear();
    _labels.Clear();
    _current_choices.Clear();
    _script_id        = nullptr;
    _current_op_index = 0u;
    _wait_remaining   = 0.0f;
    _state            = DialogueScriptState::Idle;

    _say_cb      = nullptr; _say_user      = nullptr;
    _show_cb     = nullptr; _show_user     = nullptr;
    _hide_cb     = nullptr; _hide_user     = nullptr;
    _bg_cb       = nullptr; _bg_user       = nullptr;
    _play_bgm_cb = nullptr; _play_bgm_user = nullptr;
    _stop_bgm_cb = nullptr; _stop_bgm_user = nullptr;
    _play_se_cb  = nullptr; _play_se_user  = nullptr;
    _choice_cb   = nullptr; _choice_user   = nullptr;
    _end_cb      = nullptr; _end_user      = nullptr;
}

bool DialogueScript::IsPlaying() const noexcept {
    return _state == DialogueScriptState::Playing
        || _state == DialogueScriptState::AwaitingInput
        || _state == DialogueScriptState::AwaitingChoice;
}

void DialogueScript::Advance() noexcept {
    if (_state != DialogueScriptState::AwaitingInput) return;
    // Say op を消費して次へ
    _current_op_index += 1u;
    _state = DialogueScriptState::Playing;
    RunUntilBlocked();
}

void DialogueScript::SelectChoice(u32 choice_index) noexcept {
    if (_state != DialogueScriptState::AwaitingChoice) return;
    if (choice_index >= static_cast<u32>(_current_choices.Size())) return;

    const ScriptChoice& c = _current_choices[choice_index];
    const u32 target = (c.jump_label != nullptr) ? ResolveLabel(c.jump_label) : kInvalidIndex;

    _current_choices.Clear();

    if (target == kInvalidIndex) {
        // ジャンプ先が無い / 未解決: スクリプト終了に倒す
        _state = DialogueScriptState::Playing;
        EnterFinished();
        return;
    }

    _current_op_index = target;
    _state            = DialogueScriptState::Playing;
    RunUntilBlocked();
}

const ScriptOp* DialogueScript::CurrentOp() const noexcept {
    if (_current_op_index >= static_cast<u32>(_ops.Size())) return nullptr;
    return &_ops[_current_op_index];
}

u32 DialogueScript::CurrentChoiceCount() const noexcept {
    if (_state != DialogueScriptState::AwaitingChoice) return 0;
    return static_cast<u32>(_current_choices.Size());
}

const ScriptChoice* DialogueScript::CurrentChoice(u32 index) const noexcept {
    if (_state != DialogueScriptState::AwaitingChoice) return nullptr;
    if (index >= static_cast<u32>(_current_choices.Size())) return nullptr;
    return &_current_choices[index];
}

void DialogueScript::Tick(f32 dt) noexcept {
    // Wait タイマは Playing 状態でのみ進める。
    // AwaitingInput / AwaitingChoice / Idle / Finished は no-op。
    if (_state != DialogueScriptState::Playing) return;
    if (dt <= 0.0f) return;

    if (_wait_remaining > 0.0f) {
        _wait_remaining -= dt;
        if (_wait_remaining <= 0.0f) {
            _wait_remaining = 0.0f;
            _current_op_index += 1u;   // Wait op を消費
            RunUntilBlocked();
        }
    }
}

// ----- callback 登録 -----

void DialogueScript::SetOnSayCallback(SayCallback cb, void* user) noexcept {
    _say_cb = cb; _say_user = user;
}
void DialogueScript::SetOnShowCallback(ShowHideCallback cb, void* user) noexcept {
    _show_cb = cb; _show_user = user;
}
void DialogueScript::SetOnHideCallback(ShowHideCallback cb, void* user) noexcept {
    _hide_cb = cb; _hide_user = user;
}
void DialogueScript::SetOnBackgroundCallback(BackgroundCallback cb, void* user) noexcept {
    _bg_cb = cb; _bg_user = user;
}
void DialogueScript::SetOnPlayBgmCallback(BgmSeCallback cb, void* user) noexcept {
    _play_bgm_cb = cb; _play_bgm_user = user;
}
void DialogueScript::SetOnStopBgmCallback(BgmSeCallback cb, void* user) noexcept {
    _stop_bgm_cb = cb; _stop_bgm_user = user;
}
void DialogueScript::SetOnPlaySeCallback(BgmSeCallback cb, void* user) noexcept {
    _play_se_cb = cb; _play_se_user = user;
}
void DialogueScript::SetOnChoicePresentCallback(ChoicePresentCallback cb, void* user) noexcept {
    _choice_cb = cb; _choice_user = user;
}
void DialogueScript::SetOnEndCallback(EndCallback cb, void* user) noexcept {
    _end_cb = cb; _end_user = user;
}

// ===== private =====

u32 DialogueScript::ResolveLabel(const char* label) const noexcept {
    if (label == nullptr) return kInvalidIndex;
    for (usize i = 0; i < _labels.Size(); ++i) {
        if (StrEq(_labels[i].label, label)) {
            return _labels[i].op_index;
        }
    }
    return kInvalidIndex;
}

void DialogueScript::RunUntilBlocked() noexcept {
    // 即進行系を一気に消化し、停止ポイント (Say / Choice / Wait / EndScene / 末尾) で抜ける。
    const u32 n = static_cast<u32>(_ops.Size());

    while (_state == DialogueScriptState::Playing) {
        if (_current_op_index >= n) {
            EnterFinished();
            return;
        }

        const ScriptOp& op = _ops[_current_op_index];
        switch (op.kind) {
        case ScriptOpKind::Say:
            // Say は停止ポイント
            if (_say_cb != nullptr) {
                _say_cb(_say_user, op.arg1, op.arg2);
            }
            _state = DialogueScriptState::AwaitingInput;
            return;

        case ScriptOpKind::Choice:
            // Choice 群を展開して AwaitingChoice に遷移
            EnterChoiceGroup();
            return;

        case ScriptOpKind::Wait:
            // Wait は Playing を維持しつつ Tick で消化するため、ここで抜ける
            _wait_remaining = (op.arg_f > 0.0f) ? op.arg_f : 0.0f;
            if (_wait_remaining <= 0.0f) {
                // 0 秒 Wait は即座に消費して次へ
                _current_op_index += 1u;
                continue;
            }
            return;

        case ScriptOpKind::EndScene:
            EnterFinished();
            return;

        case ScriptOpKind::Show:
        case ScriptOpKind::Hide:
        case ScriptOpKind::Background:
        case ScriptOpKind::PlayBgm:
        case ScriptOpKind::StopBgm:
        case ScriptOpKind::PlaySe:
        case ScriptOpKind::Jump:
            ExecuteImmediateOp(op);
            // ExecuteImmediateOp が _current_op_index を進める (Jump は飛ばす)
            break;
        }
    }
}

void DialogueScript::EnterChoiceGroup() noexcept {
    _current_choices.Clear();
    const u32 n = static_cast<u32>(_ops.Size());

    // _current_op_index 起点から連続する Choice op を全て選択肢に展開
    u32 i = _current_op_index;
    while (i < n && _ops[i].kind == ScriptOpKind::Choice) {
        ScriptChoice c;
        c.label      = _ops[i].arg1;
        c.jump_label = _ops[i].arg2;
        _current_choices.PushBack(c);
        ++i;
    }

    if (_current_choices.Size() == 0) {
        // 念のため: Choice op だが選択肢を一つも展開できなかった (= ロジックバグ)。
        // フォールバックとして次 op へ進めて Playing を継続。
        _current_op_index += 1u;
        return;
    }

    // _current_op_index 自体は「選択肢群の先頭 Choice op」を指したまま残す。
    // SelectChoice が jump_label でジャンプ先を解決するので、選択後はそちらに飛ぶ。
    _state = DialogueScriptState::AwaitingChoice;

    if (_choice_cb != nullptr) {
        const ScriptChoice* base = (_current_choices.Size() > 0) ? &_current_choices[0] : nullptr;
        _choice_cb(_choice_user, base, static_cast<u32>(_current_choices.Size()));
    }
}

void DialogueScript::ExecuteImmediateOp(const ScriptOp& op) noexcept {
    switch (op.kind) {
    case ScriptOpKind::Show:
        if (_show_cb != nullptr) _show_cb(_show_user, op.arg1, op.arg2);
        _current_op_index += 1u;
        break;

    case ScriptOpKind::Hide:
        if (_hide_cb != nullptr) _hide_cb(_hide_user, op.arg1, op.arg2);
        _current_op_index += 1u;
        break;

    case ScriptOpKind::Background:
        if (_bg_cb != nullptr) _bg_cb(_bg_user, op.arg1);
        _current_op_index += 1u;
        break;

    case ScriptOpKind::PlayBgm:
        if (_play_bgm_cb != nullptr) _play_bgm_cb(_play_bgm_user, op.arg1, op.arg_f);
        _current_op_index += 1u;
        break;

    case ScriptOpKind::StopBgm:
        if (_stop_bgm_cb != nullptr) _stop_bgm_cb(_stop_bgm_user, op.arg1, op.arg_f);
        _current_op_index += 1u;
        break;

    case ScriptOpKind::PlaySe:
        if (_play_se_cb != nullptr) _play_se_cb(_play_se_user, op.arg1, op.arg_f);
        _current_op_index += 1u;
        break;

    case ScriptOpKind::Jump: {
        const u32 target = ResolveLabel(op.arg1);
        if (target == kInvalidIndex) {
            // ジャンプ先未解決はスクリプト終了に倒す
            EnterFinished();
            return;
        }
        _current_op_index = target;
        break;
    }

    default:
        // 即進行系以外が来た場合のフォールバック (呼び出し側でフィルタ済みのため通常到達しない)
        _current_op_index += 1u;
        break;
    }
}

void DialogueScript::EnterFinished() noexcept {
    if (_state == DialogueScriptState::Finished) return;
    _state          = DialogueScriptState::Finished;
    _wait_remaining = 0.0f;
    _current_choices.Clear();
    if (_end_cb != nullptr) {
        _end_cb(_end_user, _script_id);
    }
}

} // namespace acs::game
