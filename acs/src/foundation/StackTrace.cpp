// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — FStackTrace 実装
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

// DbgHelp は内部状態を共有するためスレッドセーフでない。
// 全シンボル解決呼び出しを 1 つの SRWLOCK で守る。
SRWLOCK   g_sym_lock = SRWLOCK_INIT;
volatile LONG g_sym_initialized = 0;  // 0=未初期化, 1=初期化済み

// SymInitialize を一度だけ呼ぶ（多重呼び出しは無害だが無駄）
void EnsureSymbols() noexcept {
    if (g_sym_initialized) return;
    AcquireSRWLockExclusive(&g_sym_lock);
    if (!g_sym_initialized) {
        // SYMOPT_LOAD_LINES: 行番号情報も読み込む
        // SYMOPT_DEFERRED_LOADS: モジュール読み込みは必要時まで遅延
        // SYMOPT_UNDNAME: C++ 名前マングリングを解除して人間可読に
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        InterlockedExchange(&g_sym_initialized, 1);
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
}

} // namespace

// 現在のスタックフレームを取得する。
// CaptureStackBackTrace は WinAPI で、非常に高速かつスレッドセーフ。
void FStackTrace::Capture(u32 skip) noexcept {
    USHORT n = ::CaptureStackBackTrace(static_cast<DWORD>(skip), kStackTraceMaxFrames, _addrs, nullptr);
    _count    = static_cast<u32>(n);
    _resolved = false;
}

// 取得済みアドレスをシンボル名 / ファイル名 / 行番号に変換する。
// SYMBOL_INFO は末尾可変長の構造体なので適切なサイズで確保する。
void FStackTrace::Resolve() noexcept {
    if (_resolved || _count == 0) return;
    EnsureSymbols();

    AcquireSRWLockExclusive(&g_sym_lock);
    HANDLE proc = GetCurrentProcess();

    // SYMBOL_INFO + 名前用バッファをスタックに確保（ヒープ割り当てを避ける）
    alignas(SYMBOL_INFO) byte sym_buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 255;

    IMAGEHLP_LINE64 line {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    for (u32 i = 0; i < _count; ++i) {
        FStackFrame& f = _frames[i];
        f.address  = _addrs[i];
        f.symbol[0] = 0;
        f.file[0]   = 0;
        f.line      = 0;

        DWORD64 addr = reinterpret_cast<DWORD64>(_addrs[i]);
        DWORD64 disp64 = 0;
        // シンボル取得（失敗時はアドレスのみ表示）
        if (SymFromAddr(proc, addr, &disp64, sym)) {
            ::strncpy_s(f.symbol, sym->Name, sizeof(f.symbol) - 1);
        } else {
            ::snprintf(f.symbol, sizeof(f.symbol), "??? @ 0x%p", _addrs[i]);
        }
        // ファイル名 / 行番号取得
        DWORD disp32 = 0;
        if (SymGetLineFromAddr64(proc, addr, &disp32, &line) && line.FileName) {
            ::strncpy_s(f.file, line.FileName, sizeof(f.file) - 1);
            f.line = line.LineNumber;
        }
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
    _resolved = true;
}

// 解決済みフレームを 1 行ずつ整形して sink に渡す。
// 出力先（stderr / ロガー / ファイル）は呼び出し元が決める。
void FStackTrace::Print(Sink sink, void* user) const noexcept {
    char buf[640];
    for (u32 i = 0; i < _count; ++i) {
        const FStackFrame& f = _frames[i];
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
