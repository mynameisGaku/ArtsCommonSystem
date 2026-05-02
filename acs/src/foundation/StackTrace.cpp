#include "foundation/StackTrace.h"
#include "foundation/Platform.h"

#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <cstdio>
#include <cstring>

namespace acs {

namespace {

// DbgHelp Sym* APIs are NOT reentrant — every symbolication call goes through
// this single SRWLOCK. Initialization is lazy and idempotent.
SRWLOCK   g_sym_lock = SRWLOCK_INIT;
volatile LONG g_sym_initialized = 0;

void EnsureSymbols() noexcept {
    if (g_sym_initialized) return;
    AcquireSRWLockExclusive(&g_sym_lock);
    if (!g_sym_initialized) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        InterlockedExchange(&g_sym_initialized, 1);
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
}

} // namespace

void StackTrace::Capture(u32 skip) noexcept {
    // CaptureStackBackTrace's `FramesToSkip` param skips THIS function's frame
    // by default; we add the caller's request on top.
    USHORT n = ::CaptureStackBackTrace(static_cast<DWORD>(skip), kStackTraceMaxFrames, _addrs, nullptr);
    _count    = static_cast<u32>(n);
    _resolved = false;
}

void StackTrace::Resolve() noexcept {
    if (_resolved || _count == 0) return;
    EnsureSymbols();

    AcquireSRWLockExclusive(&g_sym_lock);
    HANDLE proc = GetCurrentProcess();

    alignas(SYMBOL_INFO) byte sym_buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 255;

    IMAGEHLP_LINE64 line {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    for (u32 i = 0; i < _count; ++i) {
        StackFrame& f = _frames[i];
        f.address  = _addrs[i];
        f.symbol[0] = 0;
        f.file[0]   = 0;
        f.line      = 0;

        DWORD64 addr = reinterpret_cast<DWORD64>(_addrs[i]);
        DWORD64 disp64 = 0;
        if (SymFromAddr(proc, addr, &disp64, sym)) {
            ::strncpy_s(f.symbol, sym->Name, sizeof(f.symbol) - 1);
        } else {
            ::snprintf(f.symbol, sizeof(f.symbol), "??? @ 0x%p", _addrs[i]);
        }
        DWORD disp32 = 0;
        if (SymGetLineFromAddr64(proc, addr, &disp32, &line) && line.FileName) {
            ::strncpy_s(f.file, line.FileName, sizeof(f.file) - 1);
            f.line = line.LineNumber;
        }
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
    _resolved = true;
}

void StackTrace::Print(Sink sink, void* user) const noexcept {
    char buf[640];
    for (u32 i = 0; i < _count; ++i) {
        const StackFrame& f = _frames[i];
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
