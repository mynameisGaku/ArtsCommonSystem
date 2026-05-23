// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — DevConsole (Phase 1 skeleton)
//
// `~` (チルダ) で開く開発用テキストコマンドコンソールの **state コンテナ**。
// 描画 / 入力ハンドリング (ImGui ウィンドウ、IME、オートコンプリート UI 等) は
// 上位レイヤ (Tools / Editor / 各サンプル) の責務で、本クラスは:
//   1. コマンド名 → 関数ポインタの登録 / 検索 / 実行
//   2. 入力履歴 (上下キーで再呼び出しする想定の生バッファ)
//   3. ログ出力バッファ (描画側がスクロールバックに表示する想定)
//   4. 開閉トグル状態
// だけを提供する。Ship build では namespace ごと丸ごと #ifdef で除外する想定。
//
// 設計選択:
//   ・**std::function 不使用**: コマンドは C 関数ポインタ + void* user の組。
//     クラスメンバを束ねたいときは静的 trampoline + this を user に渡す。
//   ・**const char* 引数**: 文字列 type 化 (int/float/bool parser) は Phase 2 送り。
//     今は raw token をそのまま渡し、呼び出し側がパースする。
//   ・**履歴 / ログは内部所有のバッファに copy**: 呼び出し側で形成した文字列が
//     スタック消滅しても保持できるよう、Push 時に DefaultAllocator から長さ +1
//     を取って memcpy する。drop 時に Free。Array<const char*> 自体は所有ポインタを
//     保持するだけのため、エントリ追加 / 削除のたびに自前で Alloc/Free 管理。
//   ・**固定キャップ 100 行**: 上限到達後に push すると最古 (index 0) を Free して
//     shift left。100 行 × O(memmove(99 ptrs)) なので push 自体は十分軽い。
//   ・**コマンド検索は線形**: 数十〜数百規模を想定。hash 化は Phase 2 で検討。
//   ・**tokenize 最大 8 args**: 上限超過時は余剰トークンを無視 (warning ログのみ)。
//     args 用一時バッファは Execute スタック上に置く (heap 触らない)。
//
// 範囲外 (Phase 2+ で):
//   ・argument type (i32 / f32 / bool) の自動パース
//   ・コマンド補完 (prefix → 候補列挙)
//   ・variadic args / quoted string ("hello world" を 1 トークンに)
//   ・UI レンダリング (ImGui ウィジェット)
//   ・コマンドツリー (set graphics.shadow_quality 2 のような階層名)
//   ・cvar (Pillar K の Phase 3 で別クラス化予定)
//
// 使い方:
//   DevConsole con;
//   con.RegisterCommand("quit", &MyApp::QuitCmd, this, "exit the application");
//   con.RegisterCommand("help", &MyApp::HelpCmd, this, "list commands");
//   ...
//   if (Input::IsKeyPressed(EKey::Tilde)) con.Toggle();
//   if (con.IsOpen() && enter_pressed) {
//       con.PushHistory(input_buf);
//       con.Execute(input_buf);
//   }
//   for (u32 i = 0; i < con.LogCount(); ++i) DrawLine(con.LogLine(i));
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"

namespace acs::game {

// コマンド引数 1 つ。Phase 1 は raw string のみ。
// (Phase 2 で kind / int_val / float_val / bool_val を足す予定)
struct ConsoleArg {
    const char* str = nullptr;
};

// コマンド本体。argc は args の長さ、argv[0] はコマンド名ではなく最初の引数。
// noexcept を強制してエラー伝搬を log で処理する規約。
using CommandFn = void(*)(void* user, u32 argc, const ConsoleArg* args) noexcept;

class DevConsole {
public:
    DevConsole() noexcept = default;
    ~DevConsole() noexcept;

    // 非コピー・非ムーブ (履歴 / ログが所有するヒープバッファの所有権を曖昧にしない)
    DevConsole(const DevConsole&)            = delete;
    DevConsole& operator=(const DevConsole&) = delete;
    DevConsole(DevConsole&&)                 = delete;
    DevConsole& operator=(DevConsole&&)      = delete;

    // ----- コマンド登録 -----
    // name / help_text は caller 所有 (リテラル想定)。重複登録は警告ログを出して
    // 後勝ち上書きする (再ロード時に古い trampoline を差し替えやすくするため)。
    void RegisterCommand(const char* name,
                         CommandFn   fn,
                         void*       user,
                         const char* help_text) noexcept;

    // ----- 実行 -----
    // command_line を空白 (' ', '\t') で tokenize → 先頭をコマンド名と照合 → dispatch。
    // 該当無し / 空行は warning ログ。本関数自体は履歴に push しない (caller の責務)。
    void Execute(const char* command_line) noexcept;

    // ----- 開閉状態 -----
    bool IsOpen() const noexcept { return _open; }
    void Open() noexcept   { _open = true; }
    void Close() noexcept  { _open = false; }
    void Toggle() noexcept { _open = !_open; }

    // ----- 履歴 (input line 単位) -----
    // line を内部バッファにコピーして末尾に追加。上限超過時は最古を drop。
    void PushHistory(const char* line) noexcept;
    u32  HistoryCount() const noexcept { return static_cast<u32>(_history.Size()); }
    // i = 0 が最古、HistoryCount()-1 が最新。範囲外は nullptr 安全。
    const char* History(u32 i) const noexcept;

    // ----- ログ (コマンド出力 / エラーメッセージ) -----
    // msg を内部バッファにコピーして末尾に追加。上限超過時は最古を drop。
    void Log(const char* msg) noexcept;
    u32  LogCount() const noexcept { return static_cast<u32>(_log.Size()); }
    // i = 0 が最古、LogCount()-1 が最新。範囲外は nullptr 安全。
    const char* LogLine(u32 i) const noexcept;

    // ----- 全クリア -----
    // 履歴とログを破棄してバッファを Free。コマンド登録は保持する。
    void Clear() noexcept;

    // 登録済みコマンド数 (helper / unit test 用)
    u32 CommandCount() const noexcept { return static_cast<u32>(_commands.Size()); }

private:
    static constexpr u32 kMaxArgs    = 8;    // tokenize 上限
    static constexpr u32 kLineCap    = 100;  // 履歴 / ログ各々の固定キャップ
    static constexpr u32 kMaxLineLen = 512;  // 履歴 / ログ 1 行の最大バイト (truncate)

    struct Command {
        const char* name = nullptr;  // caller 所有 (リテラル前提)
        CommandFn   fn   = nullptr;
        void*       user = nullptr;
        const char* help = nullptr;  // caller 所有
    };

    // src を null 終端文字列としてヒープに複製 (length は kMaxLineLen で truncate)。
    // 失敗時は nullptr。
    static const char* DupString(const char* src) noexcept;
    // DupString で確保した文字列を Free。nullptr 安全。
    static void FreeString(const char* s) noexcept;

    // line を buf に push、size cap 到達時は buf[0] を Free して shift left。
    static void PushLine(Array<const char*>& buf, const char* line) noexcept;
    // buf の全エントリを Free して Clear。
    static void ClearLines(Array<const char*>& buf) noexcept;

    Array<Command>     _commands;
    Array<const char*> _history;
    Array<const char*> _log;
    bool               _open = false;
};

} // namespace acs::game
