// ACS Foundation — Panic handler.
//
// Panic is the terminal failure path. It:
//   1. Writes a structured message to stderr + OutputDebugString.
//   2. Captures + prints a stack trace.
//   3. Optionally invokes a user-installed pre-abort hook (e.g. flush log).
//   4. Triggers a debug break under a debugger, otherwise calls TerminateProcess.
//
// Thread-safe: the body is serialized behind an SRWLOCK to avoid interleaved
// output from concurrent panics.
#pragma once

#include "foundation/Types.h"
#include "foundation/SourceLoc.h"
#include "foundation/Compiler.h"

namespace acs {

using PanicHook = void (*)(void* user, const char* msg, usize len);

void SetPanicHook(PanicHook hook, void* user) noexcept;

ACS_NORETURN void Panic(SourceLoc loc, const char* expr, const char* fmt, ...) noexcept;

} // namespace acs
