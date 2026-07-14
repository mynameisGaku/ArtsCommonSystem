// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — StackTrace 実装
// -----------------------------------------------------------------------------
// CaptureStackBackTrace で生のフレームアドレスを取得し、SymFromAddr 等で
// シンボル解決する。Sym* 系は非スレッドセーフのため SRWLOCK で直列化する。
// =============================================================================
#include "foundation/StackTrace.h"
#include "foundation/Platform.h"

#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")  // 自動的に dbghelp.lib をリンク

#include <cstdio>
#include <cstring>

namespace acs {

namespace {

/** 全シンボル解決呼び出しを直列化するロック (DbgHelp は非スレッドセーフ)。 */
SRWLOCK   g_sym_lock = SRWLOCK_INIT;

/** シンボル初期化済みフラグ (0=未初期化, 1=初期化済み)。 */
volatile LONG g_sym_initialized = 0;

/**
 * 排他ロック保持中に SymInitialize を一度だけ呼ぶ。
 *
 * @details 初期化失敗を成功扱いせず、次回 Resolve で再試行できる状態を保つ。
 */
bool EnsureSymbolsLocked() noexcept
{
    if (g_sym_initialized != 0) return true;

    // SYMOPT_LOAD_LINES: 行番号情報も読み込む
    // SYMOPT_DEFERRED_LOADS: モジュール読み込みは必要時まで遅延
    // SYMOPT_UNDNAME: C++ 名前マングリングを解除して人間可読に
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    if (!::SymInitialize(::GetCurrentProcess(), nullptr, TRUE)) return false;

    InterlockedExchange(&g_sym_initialized, 1);
    return true;
}

} // namespace

/** 現在のスタックフレームを取得する (CaptureStackBackTrace は高速かつスレッドセーフ)。詳細は宣言を参照。 */
void StackTrace::Capture(u32 skip) noexcept {
    USHORT n = ::CaptureStackBackTrace(static_cast<DWORD>(skip), kStackTraceMaxFrames, m_Addrs, nullptr);
    m_Count    = static_cast<u32>(n);
    m_Resolved = false;
}

/** 取得済みアドレスをシンボル名 / ファイル名 / 行番号に変換する。詳細は宣言を参照。 */
void StackTrace::Resolve() noexcept {
    if (m_Resolved || m_Count == 0) return;

    AcquireSRWLockExclusive(&g_sym_lock);
    const bool symbols_available = EnsureSymbolsLocked();
    const HANDLE proc = GetCurrentProcess();

    // SYMBOL_INFO + 名前用バッファをスタックに確保（ヒープ割り当てを避ける）
    alignas(SYMBOL_INFO) byte sym_buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* const sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 255;

    IMAGEHLP_LINE64 line {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    for (u32 i = 0; i < m_Count; ++i) {
        StackFrame& f = m_Frames[i];
        f.address  = m_Addrs[i];
        f.symbol[0] = 0;
        f.file[0]   = 0;
        f.line      = 0;

        const DWORD64 addr = reinterpret_cast<DWORD64>(m_Addrs[i]);
        DWORD64 disp64 = 0;
        // シンボル取得（失敗時はアドレスのみ表示）
        if (symbols_available && SymFromAddr(proc, addr, &disp64, sym)) {
            ::strncpy_s(f.symbol, sym->Name, sizeof(f.symbol) - 1);
        } else {
            ::snprintf(f.symbol, sizeof(f.symbol), "??? @ 0x%p", m_Addrs[i]);
        }
        // ファイル名 / 行番号取得
        DWORD disp32 = 0;
        if (symbols_available && SymGetLineFromAddr64(proc, addr, &disp32, &line) && line.FileName) {
            ::strncpy_s(f.file, line.FileName, sizeof(f.file) - 1);
            f.line = line.LineNumber;
        }
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
    m_Resolved = true;
}

/** DbgHelp の共有状態を解放する。終了後の Resolve は再び遅延初期化する。 */
void StackTrace::ShutdownSymbolResolver() noexcept
{
    AcquireSRWLockExclusive(&g_sym_lock);
    if (g_sym_initialized != 0) {
        // Cleanup 失敗時は初期化済み扱いを維持し、次回 Shutdown で再試行できる。
        if (::SymCleanup(::GetCurrentProcess())) {
            InterlockedExchange(&g_sym_initialized, 0);
        }
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
}

/** DbgHelp の共有状態をロック下で照会する。 */
bool StackTrace::IsSymbolResolverInitialized() noexcept
{
    AcquireSRWLockShared(&g_sym_lock);
    const bool initialized = g_sym_initialized != 0;
    ReleaseSRWLockShared(&g_sym_lock);
    return initialized;
}

/** 解決済みフレームを 1 行ずつ整形して sink に渡す (出力先は呼び出し元が決める)。詳細は宣言を参照。 */
void StackTrace::Print(Sink sink, void* user) const noexcept {
    char buf[640];
    for (u32 i = 0; i < m_Count; ++i) {
        const StackFrame& f = m_Frames[i];
        int n;
        if (f.file[0])
            n = ::snprintf(buf, sizeof(buf), "  #%u %s\n      at %s:%llu\n",
                           i, f.symbol, f.file, static_cast<unsigned long long>(f.line));
        else
            n = ::snprintf(buf, sizeof(buf), "  #%u %s\n", i, f.symbol);
        if (n > 0) sink(user, buf, static_cast<usize>(n));
    }
}

} // namespace acs
