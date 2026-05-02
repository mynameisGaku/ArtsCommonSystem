// ACS Foundation — Stack trace capture & symbolication.
// Backed by Win32 CaptureStackBackTrace + DbgHelp.
//
// CaptureStackBackTrace IS thread-safe; DbgHelp Sym* APIs are NOT — we serialize
// symbolication behind an internal SRWLOCK.
#pragma once

#include "foundation/Types.h"

namespace acs {

inline constexpr u32 kStackTraceMaxFrames = 64;

struct StackFrame {
    void*       address;
    u64         line;
    char        symbol[256];
    char        file[256];
};

class StackTrace {
public:
    StackTrace() noexcept = default;

    // Captures the current call stack. `skip` drops N frames from the top
    // (typically 1 to skip Capture itself).
    void Capture(u32 skip = 1) noexcept;

    // Resolves captured addresses to symbol/file/line. Thread-safe (internal lock).
    void Resolve() noexcept;

    u32              FrameCount() const noexcept { return _count; }
    const StackFrame& Frame(u32 i) const noexcept { return _frames[i]; }

    // Writes a textual dump to a callback (used by Panic + Logger).
    using Sink = void (*)(void* user, const char* line, usize len);
    void Print(Sink sink, void* user) const noexcept;

private:
    u32        _count = 0;
    bool       _resolved = false;
    void*      _addrs[kStackTraceMaxFrames] = {};
    StackFrame _frames[kStackTraceMaxFrames] = {};
};

} // namespace acs
