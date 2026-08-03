// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — CDevConsole
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
//   ・**const char* 引数**: raw token をそのまま渡し、呼び出し側がパースする。
//   ・**履歴 / ログは内部所有のバッファに copy**: 呼び出し側で形成した文字列が
//     スタック消滅しても保持できるよう、Push 時に DefaultAllocator から長さ +1
//     を取って memcpy する。drop 時に Free。TArray<const char*> 自体は所有ポインタを
//     保持するだけのため、エントリ追加 / 削除のたびに自前で Alloc/Free 管理。
//   ・**固定キャップ 100 行**: 上限到達後に push すると最古 (index 0) を Free して
//     shift left。100 行 × O(memmove(99 ptrs)) なので push 自体は十分軽い。
//   ・**コマンド検索は線形**: 数十〜数百規模を想定。
//   ・**tokenize 最大 8 args**: 上限超過時は余剰トークンを無視 (warning ログのみ)。
//     args 用一時バッファは Execute スタック上に置く (heap 触らない)。
//
// 範囲外:
//   ・argument type (i32 / f32 / bool) の自動パース
//   ・コマンド補完 (prefix → 候補列挙)
//   ・variadic args / quoted string ("hello world" を 1 トークンに)
//   ・UI レンダリング (ImGui ウィジェット)
//   ・コマンドツリー (set graphics.shadow_quality 2 のような階層名)
//   ・cvar (別クラス)
//
// 使い方:
//   CDevConsole con;
//   con.RegisterCommand("quit", &FMyApp::QuitCmd, this, "exit the application");
//   con.RegisterCommand("help", &FMyApp::HelpCmd, this, "list commands");
//   ...
//   if (CInput::IsKeyPressed(EKey::Tilde)) con.Toggle();
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

/** コマンド引数 1 つ (raw string のみ、パースは呼出側の責務)。 */
struct FConsoleArg {
    /** トークン文字列 (Execute の作業バッファを指す、非所有)。 */
    const char* str = nullptr;
};

/**
 * コマンド本体の関数ポインタ型。
 *
 * @details argv[0] はコマンド名ではなく最初の引数。noexcept を強制し、エラー伝搬は
 * log で処理する規約。
 * @param user RegisterCommand で渡したコンテキスト (非所有)。
 * @param argc args の長さ。
 * @param args 引数配列 (raw token を FConsoleArg にラップしたもの)。
 */
using CommandFn = void(*)(void* user, u32 argc, const FConsoleArg* args) noexcept;

/**
 * `~` で開く開発用コマンドコンソールの state コンテナ。
 *
 * @details
 * 描画 / 入力ハンドリング (ImGui ウィンドウ、IME、補完 UI 等) は上位レイヤの責務で、
 * 本クラスはコマンド名→関数ポインタの登録 / 検索 / 実行、入力履歴、ログ出力バッファ、
 * 開閉トグル状態だけを提供する。履歴 / ログは内部所有のヒープバッファに copy し、固定
 * キャップ 100 行で最古を drop する。非コピー・非ムーブ。
 */
class CDevConsole {
public:
    /** 空状態で構築する。 */
    CDevConsole() noexcept : CDevConsole(DefaultAllocator())
    {
    }

    /** 履歴・ログ・内部配列の確保元を明示して構築する。 */
    explicit CDevConsole(IAllocator& allocator) noexcept
        : m_Allocator(&allocator), m_Commands(allocator), m_History(allocator), m_Log(allocator)
    {
    }

    /** 履歴 / ログのヒープバッファを Free して破棄する。 */
    ~CDevConsole() noexcept;

    /** コピー禁止 (履歴 / ログが所有するヒープバッファの所有権を曖昧にしないため)。 */
    CDevConsole(const CDevConsole&)            = delete;

    /** コピー代入も禁止。 */
    CDevConsole& operator=(const CDevConsole&) = delete;

    /** ムーブ禁止 (履歴 / ログが所有するヒープバッファの所有権を曖昧にしないため)。 */
    CDevConsole(CDevConsole&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CDevConsole& operator=(CDevConsole&&)      = delete;

    /**
     * コマンドを登録する。
     *
     * @details name / help_text は caller 所有 (リテラル想定)。同名の重複登録は警告ログを
     * 出して後勝ち上書きする (再ロード時に古い trampoline を差し替えやすくするため)。
     * name が空 / nullptr または fn が nullptr のときは警告ログを出して無視する。
     * @param name コマンド名 (caller 所有、非所有参照)。
     * @param fn 実行する関数ポインタ。
     * @param user fn に渡すコンテキスト (非所有)。
     * @param help_text ヘルプ文字列 (caller 所有、非所有参照)。
     */
    void RegisterCommand(const char* name,
                         CommandFn   fn,
                         void*       user,
                         const char* help_text) noexcept;

    /**
     * コマンド行を tokenize して dispatch する。
     *
     * @details 空白 (' ', '\t') で tokenize し、先頭トークンをコマンド名と照合して実行する。
     * 該当無し / 空行は warning ログ。本関数自体は履歴に push しない (caller の責務)。
     * @param command_line 実行するコマンド行 (kMaxLineLen で truncate)。
     */
    void Execute(const char* command_line) noexcept;

    /**
     * コンソールが開いているかを返す。
     *
     * @return 開いていれば true。
     */
    bool IsOpen() const noexcept { return _open; }

    /** コンソールを開く。 */
    void Open() noexcept   { _open = true; }

    /** コンソールを閉じる。 */
    void Close() noexcept  { _open = false; }

    /** 開閉状態を反転する。 */
    void Toggle() noexcept { _open = !_open; }

    /**
     * 入力行を履歴に追加する。
     *
     * @details line を内部バッファにコピーして末尾に追加し、上限超過時は最古を drop する。
     * line が空 / nullptr なら no-op。
     * @param line 追加する入力行。
     */
    void PushHistory(const char* line) noexcept;

    /**
     * 履歴の件数を返す。
     *
     * @return 履歴エントリ数。
     */
    u32  HistoryCount() const noexcept { return static_cast<u32>(m_History.Num()); }

    /**
     * 履歴エントリを取得する (i = 0 が最古、HistoryCount()-1 が最新)。
     *
     * @param i 取得する履歴インデックス。
     * @return 履歴文字列。範囲外なら nullptr。
     */
    const char* History(u32 i) const noexcept;

    /**
     * ログ行を追加する (コマンド出力 / エラーメッセージ)。
     *
     * @details msg を内部バッファにコピーして末尾に追加し、上限超過時は最古を drop する。
     * @param msg 追加するログ文字列。
     */
    void Log(const char* msg) noexcept;

    /**
     * ログの件数を返す。
     *
     * @return ログ行数。
     */
    u32  LogCount() const noexcept { return static_cast<u32>(m_Log.Num()); }

    /**
     * ログ行を取得する (i = 0 が最古、LogCount()-1 が最新)。
     *
     * @param i 取得するログインデックス。
     * @return ログ文字列。範囲外なら nullptr。
     */
    const char* LogLine(u32 i) const noexcept;

    /** 履歴とログを破棄してバッファを Free する (コマンド登録は保持)。 */
    void Clear() noexcept;

    /**
     * 登録済みコマンド数を返す (helper / unit test 用)。
     *
     * @return 登録済みコマンド数。
     */
    u32 CommandCount() const noexcept { return static_cast<u32>(m_Commands.Num()); }

private:
    /** tokenize する引数の上限 (コマンド名を除く)。 */
    static constexpr u32 kMaxArgs    = 8;

    /** 履歴 / ログ各々の固定キャップ (行数)。 */
    static constexpr u32 kLineCap    = 100;

    /** 履歴 / ログ 1 行の最大バイト (超過分は truncate)。 */
    static constexpr u32 kMaxLineLen = 512;

    /** 登録済みコマンド 1 件のレコード。 */
    struct FCommand {
        /** コマンド名 (caller 所有、リテラル前提、非所有参照)。 */
        const char* name = nullptr;

        /** 実行する関数ポインタ。 */
        CommandFn   fn   = nullptr;

        /** fn に渡すコンテキスト (非所有)。 */
        void*       user = nullptr;

        /** ヘルプ文字列 (caller 所有、非所有参照)。 */
        const char* help = nullptr;
    };

    /**
     * src を null 終端文字列としてヒープに複製する。
     *
     * @details 長さは kMaxLineLen で truncate する。DefaultAllocator から確保する。
     * @param src 複製元 (nullptr は nullptr を返す)。
     * @return 複製した文字列。確保失敗時は nullptr。
     */
    const char* DupString(const char* src) noexcept;

    /**
     * DupString で確保した文字列を Free する。
     *
     * @param s 解放する文字列 (nullptr 安全)。
     */
    void FreeString(const char* s) noexcept;

    /**
     * line を複製して buf 末尾に push する (cap 到達時は最古を drop)。
     *
     * @details size cap (kLineCap) 到達時は buf[0] を Free して shift left してから push する。
     * 複製失敗時は黙って捨てる。
     * @param buf 追加先のバッファ。
     * @param line 追加する文字列。
     */
    void PushLine(TArray<const char*>& buf, const char* line) noexcept;

    /**
     * buf の全エントリを Free して Clear する。
     *
     * @param buf クリアするバッファ。
     */
    void ClearLines(TArray<const char*>& buf) noexcept;

    /** 文字列と内部配列を確保するアロケータ。 */
    IAllocator* m_Allocator = nullptr;

    /** 登録済みコマンド列。 */
    TArray<FCommand>     m_Commands;

    /** 入力履歴 (各要素は所有するヒープ文字列)。 */
    TArray<const char*> m_History;

    /** ログ出力バッファ (各要素は所有するヒープ文字列)。 */
    TArray<const char*> m_Log;

    /** 開閉状態 (true で開いている)。 */
    bool               _open = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FDevConsole = CDevConsole;

} // namespace acs::game
