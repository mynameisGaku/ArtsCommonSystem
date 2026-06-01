// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (visual novel) — FDialogueScript 実装
//
// 状態遷移:
//   Idle -> (LoadScript) -> Idle
//   Idle -> (Start)      -> Playing (即進行系を消化) -> {AwaitingInput | AwaitingChoice | Finished}
//   AwaitingInput  -> (Advance)      -> Playing -> 次の停止ポイントへ
//   AwaitingChoice -> (SelectChoice) -> Playing (Jump 後) -> 次の停止ポイントへ
//   Playing (Wait 中) -> (Tick で m_WaitRemaining <= 0) -> 次 op へ
//   いずれの状態 -> (Stop)     -> Idle
//   いずれの状態 -> (ClearAll) -> Idle + データ全消去 + callback クリア
//
// 即進行系 (Show / Hide / Background / PlayBgm / StopBgm / PlaySe / Jump):
//   RunUntilBlocked のループ内で callback を発火して m_CurrentOpIndex を進める。
//   フレームをまたがず一気に消化する (= caller から見ると Say / Choice / Wait /
//   EndScene が「停止ポイント」)。
//
// Choice 群展開:
//   m_CurrentOpIndex が Choice op を指していたら、連続する Choice op 群を
//   m_CurrentChoices に丸ごと積んで AwaitingChoice に遷移する。SelectChoice は
//   choice_index で jump_label を解決し、m_CurrentOpIndex をジャンプ先に
//   セットして再び RunUntilBlocked を回す。
#include "gameframework/DialogueScript.h"

#include "foundation/Log.h"

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

void FDialogueScript::Init() noexcept {
    m_CurrentChoices.Clear();
    m_CurrentOpIndex = 0u;
    m_WaitRemaining   = 0.0f;
    _state            = EDialogueScriptState::Idle;
}

void FDialogueScript::LoadScript(const ScriptOp* ops, u32 op_count, const char* script_id) noexcept {
    m_Ops.Clear();
    m_Labels.Clear();
    m_CurrentChoices.Clear();
    m_ScriptId        = script_id;
    m_CurrentOpIndex = 0u;
    m_WaitRemaining   = 0.0f;
    _state            = EDialogueScriptState::Idle;

    if (ops == nullptr || op_count == 0u) return;

    m_Ops.Reserve(op_count);
    for (u32 i = 0; i < op_count; ++i) {
        m_Ops.PushBack(ops[i]);
    }
}

void FDialogueScript::AddLabel(const char* label, u32 op_index) noexcept {
    if (label == nullptr) return;
    if (op_index >= static_cast<u32>(m_Ops.Size())) return;

    // 同名は最初の登録のみ有効 (上書き禁止)
    for (usize i = 0; i < m_Labels.Size(); ++i) {
        if (StrEq(m_Labels[i].label, label)) return;
    }

    LabelEntry e;
    e.label    = label;
    e.op_index = op_index;
    m_Labels.PushBack(e);
}

void FDialogueScript::Start(const char* start_label) noexcept {
    if (m_Ops.Size() == 0) {
        // 空スクリプト: 即 Finished にして End callback を発火
        _state = EDialogueScriptState::Playing;
        EnterFinished();
        return;
    }

    u32 start_index = 0u;
    if (start_label != nullptr) {
        const u32 resolved = ResolveLabel(start_label);
        if (resolved == kInvalidIndex) {
            // ラベル未解決: 仕様簡素化のため即 Finished に倒す
            _state = EDialogueScriptState::Playing;
            EnterFinished();
            return;
        }
        start_index = resolved;
    }

    m_CurrentOpIndex = start_index;
    m_WaitRemaining   = 0.0f;
    m_CurrentChoices.Clear();
    _state            = EDialogueScriptState::Playing;
    RunUntilBlocked();
}

void FDialogueScript::Stop() noexcept {
    _state            = EDialogueScriptState::Idle;
    m_CurrentOpIndex = 0u;
    m_WaitRemaining   = 0.0f;
    m_CurrentChoices.Clear();
}

void FDialogueScript::ClearAll() noexcept {
    m_Ops.Clear();
    m_Labels.Clear();
    m_CurrentChoices.Clear();
    m_ScriptId        = nullptr;
    m_CurrentOpIndex = 0u;
    m_WaitRemaining   = 0.0f;
    _state            = EDialogueScriptState::Idle;

    m_SayCb      = nullptr; m_SayUser      = nullptr;
    m_ShowCb     = nullptr; m_ShowUser     = nullptr;
    m_HideCb     = nullptr; m_HideUser     = nullptr;
    m_BgCb       = nullptr; m_BgUser       = nullptr;
    m_PlayBgmCb = nullptr; m_PlayBgmUser = nullptr;
    m_StopBgmCb = nullptr; m_StopBgmUser = nullptr;
    m_PlaySeCb  = nullptr; m_PlaySeUser  = nullptr;
    m_ChoiceCb   = nullptr; m_ChoiceUser   = nullptr;
    m_EndCb      = nullptr; m_EndUser      = nullptr;
}

bool FDialogueScript::IsPlaying() const noexcept {
    return _state == EDialogueScriptState::Playing
        || _state == EDialogueScriptState::AwaitingInput
        || _state == EDialogueScriptState::AwaitingChoice;
}

void FDialogueScript::Advance() noexcept {
    if (_state != EDialogueScriptState::AwaitingInput) return;
    // Say op を消費して次へ
    m_CurrentOpIndex += 1u;
    _state = EDialogueScriptState::Playing;
    RunUntilBlocked();
}

void FDialogueScript::SelectChoice(u32 choice_index) noexcept {
    if (_state != EDialogueScriptState::AwaitingChoice) return;
    if (choice_index >= static_cast<u32>(m_CurrentChoices.Size())) return;

    const ScriptChoice& c = m_CurrentChoices[choice_index];
    const u32 target = (c.jump_label != nullptr) ? ResolveLabel(c.jump_label) : kInvalidIndex;

    m_CurrentChoices.Clear();

    if (target == kInvalidIndex) {
        // ジャンプ先が無い / 未解決: スクリプト終了に倒す
        _state = EDialogueScriptState::Playing;
        EnterFinished();
        return;
    }

    m_CurrentOpIndex = target;
    _state            = EDialogueScriptState::Playing;
    RunUntilBlocked();
}

const ScriptOp* FDialogueScript::CurrentOp() const noexcept {
    if (m_CurrentOpIndex >= static_cast<u32>(m_Ops.Size())) return nullptr;
    return &m_Ops[m_CurrentOpIndex];
}

u32 FDialogueScript::CurrentChoiceCount() const noexcept {
    if (_state != EDialogueScriptState::AwaitingChoice) return 0;
    return static_cast<u32>(m_CurrentChoices.Size());
}

const ScriptChoice* FDialogueScript::CurrentChoice(u32 index) const noexcept {
    if (_state != EDialogueScriptState::AwaitingChoice) return nullptr;
    if (index >= static_cast<u32>(m_CurrentChoices.Size())) return nullptr;
    return &m_CurrentChoices[index];
}

void FDialogueScript::Tick(f32 dt) noexcept {
    // Wait タイマは Playing 状態でのみ進める。
    // AwaitingInput / AwaitingChoice / Idle / Finished は no-op。
    if (_state != EDialogueScriptState::Playing) return;
    if (dt <= 0.0f) return;

    if (m_WaitRemaining > 0.0f) {
        m_WaitRemaining -= dt;
        if (m_WaitRemaining <= 0.0f) {
            m_WaitRemaining = 0.0f;
            m_CurrentOpIndex += 1u;   // Wait op を消費
            RunUntilBlocked();
        }
    }
}

// ----- callback 登録 -----

void FDialogueScript::SetOnSayCallback(SayCallback cb, void* user) noexcept {
    m_SayCb = cb; m_SayUser = user;
}
void FDialogueScript::SetOnShowCallback(ShowHideCallback cb, void* user) noexcept {
    m_ShowCb = cb; m_ShowUser = user;
}
void FDialogueScript::SetOnHideCallback(ShowHideCallback cb, void* user) noexcept {
    m_HideCb = cb; m_HideUser = user;
}
void FDialogueScript::SetOnBackgroundCallback(BackgroundCallback cb, void* user) noexcept {
    m_BgCb = cb; m_BgUser = user;
}
void FDialogueScript::SetOnPlayBgmCallback(BgmSeCallback cb, void* user) noexcept {
    m_PlayBgmCb = cb; m_PlayBgmUser = user;
}
void FDialogueScript::SetOnStopBgmCallback(BgmSeCallback cb, void* user) noexcept {
    m_StopBgmCb = cb; m_StopBgmUser = user;
}
void FDialogueScript::SetOnPlaySeCallback(BgmSeCallback cb, void* user) noexcept {
    m_PlaySeCb = cb; m_PlaySeUser = user;
}
void FDialogueScript::SetOnChoicePresentCallback(ChoicePresentCallback cb, void* user) noexcept {
    m_ChoiceCb = cb; m_ChoiceUser = user;
}
void FDialogueScript::SetOnEndCallback(EndCallback cb, void* user) noexcept {
    m_EndCb = cb; m_EndUser = user;
}

// ===== private =====

u32 FDialogueScript::ResolveLabel(const char* label) const noexcept {
    if (label == nullptr) return kInvalidIndex;
    for (usize i = 0; i < m_Labels.Size(); ++i) {
        if (StrEq(m_Labels[i].label, label)) {
            return m_Labels[i].op_index;
        }
    }
    return kInvalidIndex;
}

void FDialogueScript::RunUntilBlocked() noexcept {
    // 即進行系を一気に消化し、停止ポイント (Say / Choice / Wait / EndScene / 末尾) で抜ける。
    const u32 n = static_cast<u32>(m_Ops.Size());

    // Jump によるラベル循環 (op0 → Jump op0 等) では停止ポイントに到達できず
    // while が無限ループする。即進行 op の処理回数は最大でも op 数 n 回までで、
    // それを超えると必ず同じ index を再訪している = 循環。ガードカウンタで打ち切り、
    // 安全に EnterFinished へ落とす (ハングよりシーン終了が望ましい)。
    u32 guard = 0;
    const u32 guard_max = n + 1u;  // 終端チェックの 1 反復ぶん余裕を持たせる

    while (_state == EDialogueScriptState::Playing) {
        if (++guard > guard_max) {
            ACS_LOG_WARN("FDialogueScript::RunUntilBlocked: Jump ラベル循環を検知 (op数=%u)。スクリプトを終了します。", n);
            EnterFinished();
            return;
        }
        if (m_CurrentOpIndex >= n) {
            EnterFinished();
            return;
        }

        const ScriptOp& op = m_Ops[m_CurrentOpIndex];
        switch (op.kind) {
        case EScriptOpKind::Say:
            // Say は停止ポイント
            if (m_SayCb != nullptr) {
                m_SayCb(m_SayUser, op.arg1, op.arg2);
            }
            _state = EDialogueScriptState::AwaitingInput;
            return;

        case EScriptOpKind::Choice:
            // Choice 群を展開して AwaitingChoice に遷移
            EnterChoiceGroup();
            return;

        case EScriptOpKind::Wait:
            // Wait は Playing を維持しつつ Tick で消化するため、ここで抜ける
            m_WaitRemaining = (op.arg_f > 0.0f) ? op.arg_f : 0.0f;
            if (m_WaitRemaining <= 0.0f) {
                // 0 秒 Wait は即座に消費して次へ
                m_CurrentOpIndex += 1u;
                continue;
            }
            return;

        case EScriptOpKind::EndScene:
            EnterFinished();
            return;

        case EScriptOpKind::Show:
        case EScriptOpKind::Hide:
        case EScriptOpKind::Background:
        case EScriptOpKind::PlayBgm:
        case EScriptOpKind::StopBgm:
        case EScriptOpKind::PlaySe:
        case EScriptOpKind::Jump:
            ExecuteImmediateOp(op);
            // ExecuteImmediateOp が m_CurrentOpIndex を進める (Jump は飛ばす)
            break;
        }
    }
}

void FDialogueScript::EnterChoiceGroup() noexcept {
    m_CurrentChoices.Clear();
    const u32 n = static_cast<u32>(m_Ops.Size());

    // m_CurrentOpIndex 起点から連続する Choice op を全て選択肢に展開
    u32 i = m_CurrentOpIndex;
    while (i < n && m_Ops[i].kind == EScriptOpKind::Choice) {
        ScriptChoice c;
        c.label      = m_Ops[i].arg1;
        c.jump_label = m_Ops[i].arg2;
        m_CurrentChoices.PushBack(c);
        ++i;
    }

    if (m_CurrentChoices.Size() == 0) {
        // 念のため: Choice op だが選択肢を一つも展開できなかった (= ロジックバグ)。
        // フォールバックとして次 op へ進めて Playing を継続。
        m_CurrentOpIndex += 1u;
        return;
    }

    // m_CurrentOpIndex 自体は「選択肢群の先頭 Choice op」を指したまま残す。
    // SelectChoice が jump_label でジャンプ先を解決するので、選択後はそちらに飛ぶ。
    _state = EDialogueScriptState::AwaitingChoice;

    if (m_ChoiceCb != nullptr) {
        const ScriptChoice* base = (m_CurrentChoices.Size() > 0) ? &m_CurrentChoices[0] : nullptr;
        m_ChoiceCb(m_ChoiceUser, base, static_cast<u32>(m_CurrentChoices.Size()));
    }
}

void FDialogueScript::ExecuteImmediateOp(const ScriptOp& op) noexcept {
    switch (op.kind) {
    case EScriptOpKind::Show:
        if (m_ShowCb != nullptr) m_ShowCb(m_ShowUser, op.arg1, op.arg2);
        m_CurrentOpIndex += 1u;
        break;

    case EScriptOpKind::Hide:
        if (m_HideCb != nullptr) m_HideCb(m_HideUser, op.arg1, op.arg2);
        m_CurrentOpIndex += 1u;
        break;

    case EScriptOpKind::Background:
        if (m_BgCb != nullptr) m_BgCb(m_BgUser, op.arg1);
        m_CurrentOpIndex += 1u;
        break;

    case EScriptOpKind::PlayBgm:
        if (m_PlayBgmCb != nullptr) m_PlayBgmCb(m_PlayBgmUser, op.arg1, op.arg_f);
        m_CurrentOpIndex += 1u;
        break;

    case EScriptOpKind::StopBgm:
        if (m_StopBgmCb != nullptr) m_StopBgmCb(m_StopBgmUser, op.arg1, op.arg_f);
        m_CurrentOpIndex += 1u;
        break;

    case EScriptOpKind::PlaySe:
        if (m_PlaySeCb != nullptr) m_PlaySeCb(m_PlaySeUser, op.arg1, op.arg_f);
        m_CurrentOpIndex += 1u;
        break;

    case EScriptOpKind::Jump: {
        const u32 target = ResolveLabel(op.arg1);
        if (target == kInvalidIndex) {
            // ジャンプ先未解決はスクリプト終了に倒す
            EnterFinished();
            return;
        }
        m_CurrentOpIndex = target;
        break;
    }

    default:
        // 即進行系以外が来た場合のフォールバック (呼び出し側でフィルタ済みのため通常到達しない)
        m_CurrentOpIndex += 1u;
        break;
    }
}

void FDialogueScript::EnterFinished() noexcept {
    if (_state == EDialogueScriptState::Finished) return;
    _state          = EDialogueScriptState::Finished;
    m_WaitRemaining = 0.0f;
    m_CurrentChoices.Clear();
    if (m_EndCb != nullptr) {
        m_EndCb(m_EndUser, m_ScriptId);
    }
}

} // namespace acs::game
