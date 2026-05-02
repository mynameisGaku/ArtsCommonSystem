// ACS Foundation — Asynchronous, thread-safe logger.
//
// Architecture (Phase 1):
//   * Bounded MPMC ring (Dmitry Vyukov sequence-cell algorithm) sized at init.
//   * Producers format with vsnprintf into the cell payload and publish via
//     release-store of the cell sequence number.
//   * A single writer thread drains the ring, writing each record to:
//       - stdout (if cfg.console)
//       - OutputDebugString (if cfg.debug_output)
//       - log file (if cfg.file_path)
//   * Backpressure: when the ring is full the producer DROPS the record and
//     bumps an atomic "dropped" counter that the writer surfaces periodically.
//
// All atomics use _Interlocked* / _InterlockedExchangeAdd / acquire-release
// suffixed variants on ARM64. No STL, no exceptions.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"

namespace acs {

enum class LogSeverity : u8 {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5,
    Off   = 6,
};

constexpr const char* ToString(LogSeverity s) noexcept {
    switch (s) {
        case LogSeverity::Trace: return "TRACE";
        case LogSeverity::Debug: return "DEBUG";
        case LogSeverity::Info:  return "INFO";
        case LogSeverity::Warn:  return "WARN";
        case LogSeverity::Error: return "ERROR";
        case LogSeverity::Fatal: return "FATAL";
        case LogSeverity::Off:   return "OFF";
    }
    return "?";
}

struct LogConfig {
    const wchar_t* file_path     = nullptr;            // nullptr = no file
    LogSeverity    min_severity  = LogSeverity::Info;
    u32            ring_capacity = 4096;               // must be power of 2
    bool           console       = true;
    bool           debug_output  = true;               // OutputDebugStringA
};

class Logger {
public:
    static void Init(const LogConfig& cfg) noexcept;
    static void Shutdown() noexcept;
    static void Flush() noexcept;

    static void SetMinSeverity(LogSeverity s) noexcept;
    static bool Enabled(LogSeverity s) noexcept;

    static u64  DroppedCount() noexcept;

    // Producer hot path. Format is printf-style (we defer fmt::format style
    // to a Phase 2 binary-serialized backend).
    ACS_NEVERINLINE static void Write(LogSeverity sev,
                                      SourceLoc   loc,
                                      const char* fmt,
                                      ...) noexcept;
};

} // namespace acs

// ---- Per-severity macros ---------------------------------------------------
#define ACS_LOG(sev, fmt, ...)                                                 \
    do {                                                                       \
        if (::acs::Logger::Enabled(sev))                                       \
            ::acs::Logger::Write(sev, ::acs::SourceLoc::Current(),             \
                                 fmt, ##__VA_ARGS__);                          \
    } while (0)

#define ACS_LOG_TRACE(fmt, ...) ACS_LOG(::acs::LogSeverity::Trace, fmt, ##__VA_ARGS__)
#define ACS_LOG_DEBUG(fmt, ...) ACS_LOG(::acs::LogSeverity::Debug, fmt, ##__VA_ARGS__)
#define ACS_LOG_INFO(fmt, ...)  ACS_LOG(::acs::LogSeverity::Info,  fmt, ##__VA_ARGS__)
#define ACS_LOG_WARN(fmt, ...)  ACS_LOG(::acs::LogSeverity::Warn,  fmt, ##__VA_ARGS__)
#define ACS_LOG_ERROR(fmt, ...) ACS_LOG(::acs::LogSeverity::Error, fmt, ##__VA_ARGS__)
#define ACS_LOG_FATAL(fmt, ...) ACS_LOG(::acs::LogSeverity::Fatal, fmt, ##__VA_ARGS__)
