// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — DevConsole 実装 (Phase 1 skeleton)
//
// state コンテナのみ。UI / 入力ハンドラは別レイヤ。詳細は DevConsole.h 参照。
#include "gameframework/DevConsole.h"

#include "memory/Allocator.h"
#include "memory/Memory.h"
#include "foundation/SourceLoc.h"

#include <cstring>   // strlen, strcmp

namespace acs::game {

// ============================================================================
// 文字列ヘルパ
// ============================================================================

const char* DevConsole::DupString(const char* src) noexcept {
    if (src == nullptr) return nullptr;
    // strnlen 相当を手で書く (kMaxLineLen を超えたら truncate)。
    usize len = 0;
    while (len < kMaxLineLen && src[len] != '\0') ++len;

    Allocator& a = DefaultAllocator();
    char* buf = static_cast<char*>(a.Alloc(len + 1, alignof(char), SourceLoc::Current()));
    if (buf == nullptr) return nullptr;
    if (len > 0) MemCopy(buf, src, len);
    buf[len] = '\0';
    return buf;
}

void DevConsole::FreeString(const char* s) noexcept {
    if (s == nullptr) return;
    // DupString が確保したものを Free する。const_cast は所有元が分かっているため安全。
    DefaultAllocator().Free(const_cast<void*>(static_cast<const void*>(s)));
}

// ============================================================================
// リング相当 push (Array<const char*> + cap 100 + drop oldest)
// ============================================================================

void DevConsole::PushLine(Array<const char*>& buf, const char* line) noexcept {
    const char* copy = DupString(line);
    if (copy == nullptr) return;  // 確保失敗時は黙って捨てる (Log 内 Log のループ防止)

    if (buf.Size() >= kLineCap) {
        // 最古を Free して shift left。100 要素以下なので memmove 1 回で済む。
        FreeString(buf[0]);
        for (usize i = 1; i < buf.Size(); ++i) {
            buf[i - 1] = buf[i];
        }
        buf.PopBack();
    }
    buf.PushBack(copy);
}

void DevConsole::ClearLines(Array<const char*>& buf) noexcept {
    for (usize i = 0; i < buf.Size(); ++i) {
        FreeString(buf[i]);
    }
    buf.Clear();
}

// ============================================================================
// ライフサイクル
// ============================================================================

DevConsole::~DevConsole() noexcept {
    ClearLines(_history);
    ClearLines(_log);
    // _commands は POD ポインタのみで、name/help は caller 所有なので Free 不要。
}

// ============================================================================
// コマンド登録
// ============================================================================

void DevConsole::RegisterCommand(const char* name,
                                 CommandFn   fn,
                                 void*       user,
                                 const char* help_text) noexcept {
    if (name == nullptr || name[0] == '\0' || fn == nullptr) {
        ACS_LOG_WARN("DevConsole::RegisterCommand: invalid name or fn (ignored)");
        return;
    }

    // 既存検索 (線形)。あれば後勝ち上書き。
    for (usize i = 0; i < _commands.Size(); ++i) {
        if (std::strcmp(_commands[i].name, name) == 0) {
            ACS_LOG_WARN("DevConsole: command '%s' re-registered (overwriting)", name);
            _commands[i].fn   = fn;
            _commands[i].user = user;
            _commands[i].help = help_text;
            return;
        }
    }

    Command c;
    c.name = name;
    c.fn   = fn;
    c.user = user;
    c.help = help_text;
    _commands.PushBack(c);
}

// ============================================================================
// 実行 (tokenize + dispatch)
// ============================================================================

void DevConsole::Execute(const char* command_line) noexcept {
    if (command_line == nullptr) return;

    // 1) スタック上の作業バッファに line をコピー (truncate)。
    //    tokenize は in-place で区切り文字を '\0' に書き換えるため、入力を破壊しないようコピー。
    char buf[kMaxLineLen + 1];
    usize n = 0;
    while (n < kMaxLineLen && command_line[n] != '\0') {
        buf[n] = command_line[n];
        ++n;
    }
    buf[n] = '\0';

    // 2) 空白で tokenize。最大 kMaxArgs+1 個 (コマンド名 + kMaxArgs)。
    constexpr u32 kMaxTokens = kMaxArgs + 1;
    const char* tokens[kMaxTokens] = {};
    u32 token_count = 0;

    usize i = 0;
    while (i < n) {
        // skip whitespace
        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) {
            buf[i] = '\0';
            ++i;
        }
        if (i >= n) break;

        if (token_count >= kMaxTokens) {
            ACS_LOG_WARN("DevConsole::Execute: too many tokens (max %u, extra ignored)", kMaxTokens);
            break;
        }
        tokens[token_count++] = &buf[i];

        // skip non-whitespace
        while (i < n && buf[i] != ' ' && buf[i] != '\t') ++i;
    }

    if (token_count == 0) {
        // 空行は黙ってスキップ (UI 側で空 Enter する想定)
        return;
    }

    // 3) コマンド名検索 (線形)。
    const char* cmd_name = tokens[0];
    const Command* found = nullptr;
    for (usize ci = 0; ci < _commands.Size(); ++ci) {
        if (std::strcmp(_commands[ci].name, cmd_name) == 0) {
            found = &_commands[ci];
            break;
        }
    }

    if (found == nullptr) {
        ACS_LOG_WARN("DevConsole: unknown command '%s'", cmd_name);
        // ログにも残してユーザが UI 上で確認できるようにする (cap 内で安全)
        Log("unknown command");
        return;
    }

    // 4) dispatch。args は tokens[1..] (ConsoleArg にラップ)。
    ConsoleArg args[kMaxArgs];
    const u32 argc = (token_count > 1) ? (token_count - 1) : 0;
    for (u32 ai = 0; ai < argc; ++ai) {
        args[ai].str = tokens[ai + 1];
    }
    found->fn(found->user, argc, args);
}

// ============================================================================
// 履歴 / ログ
// ============================================================================

void DevConsole::PushHistory(const char* line) noexcept {
    if (line == nullptr || line[0] == '\0') return;
    PushLine(_history, line);
}

const char* DevConsole::History(u32 i) const noexcept {
    if (i >= _history.Size()) return nullptr;
    return _history[i];
}

void DevConsole::Log(const char* msg) noexcept {
    if (msg == nullptr) return;
    PushLine(_log, msg);
}

const char* DevConsole::LogLine(u32 i) const noexcept {
    if (i >= _log.Size()) return nullptr;
    return _log[i];
}

void DevConsole::Clear() noexcept {
    ClearLines(_history);
    ClearLines(_log);
}

} // namespace acs::game
