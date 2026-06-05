// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — FPanic 実装
// -----------------------------------------------------------------------------
// パニック発生時に行うこと:
//   1. SRWLOCK で他のパニックと排他（出力混在を防ぐ）
//   2. 場所 / 失敗式 / メッセージを stderr + OutputDebugString に出力
//   3. シンボル化されたスタックトレースを出力
//   4. ユーザー登録の PanicHook を呼ぶ
//   5. デバッガ接続中なら __debugbreak、なければ TerminateProcess
// =============================================================================
#include "foundation/Panic.h"
#include "foundation/Platform.h"
#include "foundation/StackTrace.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace acs {

namespace {

/** パニック処理全体を排他するロック (複数スレッド同時 panic 時の出力混在を防ぐ)。 */
SRWLOCK   g_panic_lock = SRWLOCK_INIT;

/** ユーザー登録の事前フック (未登録なら nullptr)。 */
PanicHook g_hook = nullptr;

/** フックに渡すコンテキストポインタ。 */
void*     g_hook_user = nullptr;

/**
 * バッファを HANDLE に書き出す (部分書き込みは無視する)。
 *
 * @param h 書き込み先ハンドル (無効ハンドルなら何もしない)。
 * @param buf 書き込むバッファ。
 * @param n 書き込むバイト数。
 */
void WriteAll(HANDLE h, const char* buf, usize n) noexcept {
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    DWORD written = 0;
    ::WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr);
}

/**
 * stderr / OutputDebugString / 登録フックに同時出力する共通ルート。
 *
 * @param buf 出力するバッファ。
 * @param n 出力するバイト数。
 */
void Emit(const char* buf, usize n) noexcept {
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    WriteAll(err, buf, n);
    ::OutputDebugStringA(buf);
    if (g_hook) g_hook(g_hook_user, buf, n);
}

/**
 * StackTrace::Print 用のシンク関数 (1 行ずつ Emit に流す)。
 *
 * @param line 出力するスタックトレース 1 行。
 * @param len line のバイト数。
 */
void StackSink(void* /*user*/, const char* line, usize len) noexcept {
    Emit(line, len);
}

} // namespace

/**
 * パニックフックを登録する (nullptr で解除可能)。
 *
 * @param hook 登録するフック (nullptr で解除)。
 * @param user フックに渡すコンテキストポインタ。
 */
void SetPanicHook(PanicHook hook, void* user) noexcept {
    AcquireSRWLockExclusive(&g_panic_lock);
    g_hook = hook;
    g_hook_user = user;
    ReleaseSRWLockExclusive(&g_panic_lock);
}

/**
 * パニック本体。ヘッダとメッセージとスタックトレースを出力してプロセスを終了する。
 *
 * @details 可変長引数を vsnprintf で整形し、終了コード 3 で TerminateProcess する。
 * @param loc 発生位置。
 * @param expr 失敗した式の文字列 (nullptr なら "<none>")。
 * @param fmt printf 形式のメッセージ書式 (nullptr なら "(no message)")。
 * @param ... fmt に対応する可変長引数。
 */
ACS_NORETURN void FPanic(FSourceLoc loc, const char* expr, const char* fmt, ...) noexcept {
    AcquireSRWLockExclusive(&g_panic_lock);

    // ヘッダ部（場所、失敗式、メッセージ前置き）
    char header[1024];
    int hn = ::snprintf(header, sizeof(header),
        "\n==================== ACS PANIC ====================\n"
        " location : %s:%u (%s)\n"
        " expr     : %s\n"
        " message  : ",
        loc.File(), loc.Line(), loc.Function(),
        expr ? expr : "<none>");
    if (hn < 0) hn = 0;
    Emit(header, static_cast<usize>(hn));

    // メッセージ本文（printf 互換フォーマット）
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    int mn = ::vsnprintf(msg, sizeof(msg), fmt ? fmt : "(no message)", ap);
    va_end(ap);
    if (mn < 0) mn = 0;
    Emit(msg, static_cast<usize>(mn));
    Emit("\n stack    :\n", 13);

    // スタックトレース取得＋出力（ここまで来た時点で 2 フレーム skip すれば呼び出し元が見える）
    StackTrace st;
    st.Capture(/*skip*/ 2);
    st.Resolve();
    st.Print(&StackSink, nullptr);

    Emit("===================================================\n", 52);

    ReleaseSRWLockExclusive(&g_panic_lock);

    // デバッガ接続中なら __debugbreak で停止（後続処理をキャンセル）
    if (::IsDebuggerPresent()) {
        ACS_DEBUGBREAK();
    }
    // stderr バッファをフラッシュ
    ::FlushFileBuffers(::GetStdHandle(STD_ERROR_HANDLE));
    // プロセス強制終了。終了コード 3（abort 慣例）
    ::TerminateProcess(::GetCurrentProcess(), 3);
    // [[noreturn]] 契約のための無限ループ（実際には到達しない）
    for (;;) {}
}

} // namespace acs
