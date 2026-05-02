#include "foundation/Log.h"
#include "foundation/Platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace acs {

namespace {

// ---- Cell --------------------------------------------------------------
// Vyukov bounded MPMC: each slot carries its own sequence number plus the
// payload. Producer claims by CAS-incrementing the global enqueue cursor
// when sequence == cursor; finalizes by storing seq = cursor + 1.
// Consumer reads when sequence == cursor + 1; finalizes by storing
// seq = cursor + capacity.
constexpr u32 kMessageMax = 480;

struct alignas(64) Cell {
    volatile LONG64 sequence;
    LogSeverity     severity;
    u8              _pad0[7];
    SourceLoc       loc;
    DWORD           thread_id;
    LARGE_INTEGER   timestamp;        // QPC ticks
    u16             message_len;
    char            message[kMessageMax];
};

static_assert(sizeof(Cell) % 64 == 0 || sizeof(Cell) >= 64, "Cell should be at least one cache line");

// ---- Global state ------------------------------------------------------
struct LoggerState {
    Cell*        ring          = nullptr;
    u32          capacity      = 0;
    u32          mask          = 0;

    // Padded to avoid false sharing between producer and consumer cursors.
    ACS_CACHELINE_ALIGN volatile LONG64 enqueue_pos = 0;
    ACS_CACHELINE_ALIGN volatile LONG64 dequeue_pos = 0;
    ACS_CACHELINE_ALIGN volatile LONG64 dropped     = 0;

    // Severity gate — relaxed atomic load on hot path.
    ACS_CACHELINE_ALIGN volatile LONG min_severity = static_cast<LONG>(LogSeverity::Info);

    // Writer thread state.
    HANDLE              writer_thread     = nullptr;
    volatile LONG       running           = 0;
    SRWLOCK             wake_lock         = SRWLOCK_INIT;
    CONDITION_VARIABLE  wake_cv           = CONDITION_VARIABLE_INIT;

    // Sinks.
    HANDLE  out_console = INVALID_HANDLE_VALUE;
    HANDLE  out_file    = INVALID_HANDLE_VALUE;
    bool    use_console = false;
    bool    use_dbgout  = false;

    // QPC calibration to wallclock.
    LARGE_INTEGER qpc_freq {};
    LARGE_INTEGER qpc_origin {};
    FILETIME      ft_origin {};
};

LoggerState g_state;
volatile LONG g_inited = 0;

ACS_FORCEINLINE bool IsPowerOfTwo(u32 v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

void WriteAll(HANDLE h, const char* p, usize n) noexcept {
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    while (n > 0) {
        DWORD chunk = static_cast<DWORD>(n > 0xFFFF0000u ? 0xFFFF0000u : n);
        DWORD wrote = 0;
        if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) return;
        p += wrote;
        n -= wrote;
    }
}

void FormatTimestamp(const LARGE_INTEGER& qpc, char* out, usize cap) noexcept {
    // Convert QPC delta to a 100-ns delta, add to ft_origin -> SystemTime.
    LONGLONG delta = qpc.QuadPart - g_state.qpc_origin.QuadPart;
    LONGLONG ns100 = (delta * 10000000LL) / g_state.qpc_freq.QuadPart;

    ULARGE_INTEGER ft;
    ft.LowPart  = g_state.ft_origin.dwLowDateTime;
    ft.HighPart = g_state.ft_origin.dwHighDateTime;
    ft.QuadPart += static_cast<ULONGLONG>(ns100);

    FILETIME    ftloc;
    SYSTEMTIME  st;
    FILETIME    ftnow {};
    ftnow.dwLowDateTime  = ft.LowPart;
    ftnow.dwHighDateTime = ft.HighPart;
    ::FileTimeToLocalFileTime(&ftnow, &ftloc);
    ::FileTimeToSystemTime(&ftloc, &st);

    ::snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void EmitOne(const Cell& c) noexcept {
    char ts[32];
    FormatTimestamp(c.timestamp, ts, sizeof(ts));

    char line[1024];
    int n = ::snprintf(line, sizeof(line),
        "[%s] [%-5s] [tid=%lu] %s:%u (%s) | %.*s\n",
        ts, ToString(c.severity), static_cast<unsigned long>(c.thread_id),
        c.loc.File(), c.loc.Line(), c.loc.Function(),
        static_cast<int>(c.message_len), c.message);
    if (n < 0) return;
    usize len = static_cast<usize>(n) < sizeof(line) ? static_cast<usize>(n) : sizeof(line) - 1;

    if (g_state.use_console) WriteAll(g_state.out_console, line, len);
    if (g_state.out_file != INVALID_HANDLE_VALUE) WriteAll(g_state.out_file, line, len);
    if (g_state.use_dbgout) ::OutputDebugStringA(line);
}

DWORD WINAPI WriterThreadProc(LPVOID) noexcept {
    while (true) {
        // Drain as many cells as available in this pass.
        for (;;) {
            LONG64 pos = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
            Cell& cell = g_state.ring[pos & g_state.mask];
            LONG64 seq = ::_InterlockedExchangeAdd64(&cell.sequence, 0);
            LONG64 dif = seq - (pos + 1);
            if (dif == 0) {
                if (::_InterlockedCompareExchange64(&g_state.dequeue_pos, pos + 1, pos) == pos) {
                    EmitOne(cell);
                    // Mark slot ready for the next producer cycle.
                    ::_InterlockedExchange64(&cell.sequence, pos + g_state.capacity);
                }
            } else if (dif < 0) {
                break; // empty
            }
            // dif > 0: another consumer raced (we have only one but be defensive).
        }

        // Surface dropped-count once per drain pass.
        LONG64 dropped = ::_InterlockedExchange64(&g_state.dropped, 0);
        if (dropped > 0) {
            char warn[160];
            int n = ::snprintf(warn, sizeof(warn),
                "[acs::Logger] WARNING: dropped %lld log records due to ring overflow\n",
                static_cast<long long>(dropped));
            if (n > 0) {
                if (g_state.use_console) WriteAll(g_state.out_console, warn, static_cast<usize>(n));
                if (g_state.out_file != INVALID_HANDLE_VALUE) WriteAll(g_state.out_file, warn, static_cast<usize>(n));
                if (g_state.use_dbgout) ::OutputDebugStringA(warn);
            }
        }

        if (!::_InterlockedExchangeAdd(&g_state.running, 0)) {
            // Drain once more on shutdown then exit.
            LONG64 head = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
            LONG64 tail = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
            if (tail >= head) break;
            // else loop and drain
            continue;
        }

        // Sleep until producer signals — bounded by 100ms for periodic flushing.
        AcquireSRWLockExclusive(&g_state.wake_lock);
        ::SleepConditionVariableSRW(&g_state.wake_cv, &g_state.wake_lock, 100, 0);
        ReleaseSRWLockExclusive(&g_state.wake_lock);
    }

    if (g_state.out_file != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(g_state.out_file);
    }
    return 0;
}

} // namespace

// ---- Public API -----------------------------------------------------------

void Logger::Init(const LogConfig& cfg) noexcept {
    if (::_InterlockedCompareExchange(&g_inited, 1, 0) != 0) return;

    u32 cap = cfg.ring_capacity;
    if (!IsPowerOfTwo(cap)) {
        // Round up to next power of two — safer than crashing.
        u32 v = 1; while (v < cap) v <<= 1; cap = v;
    }
    if (cap < 16) cap = 16;

    void* mem = ::VirtualAlloc(nullptr, sizeof(Cell) * cap,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        // Last-ditch: write to stderr and bail.
        ::OutputDebugStringA("[acs::Logger] FATAL: ring allocation failed\n");
        ::_InterlockedExchange(&g_inited, 0);
        return;
    }
    g_state.ring     = static_cast<Cell*>(mem);
    g_state.capacity = cap;
    g_state.mask     = cap - 1;
    for (u32 i = 0; i < cap; ++i) {
        g_state.ring[i].sequence = i;
    }
    g_state.enqueue_pos = 0;
    g_state.dequeue_pos = 0;
    g_state.dropped     = 0;

    g_state.use_console = cfg.console;
    g_state.use_dbgout  = cfg.debug_output;
    if (cfg.console) g_state.out_console = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (cfg.file_path) {
        g_state.out_file = ::CreateFileW(cfg.file_path, GENERIC_WRITE, FILE_SHARE_READ,
                                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    ::QueryPerformanceFrequency(&g_state.qpc_freq);
    ::QueryPerformanceCounter(&g_state.qpc_origin);
    ::GetSystemTimeAsFileTime(&g_state.ft_origin);

    ::_InterlockedExchange(&g_state.min_severity, static_cast<LONG>(cfg.min_severity));
    ::_InterlockedExchange(&g_state.running, 1);

    g_state.writer_thread = ::CreateThread(nullptr, 0, &WriterThreadProc, nullptr, 0, nullptr);
    ::SetThreadDescription(g_state.writer_thread, L"acs::Logger writer");
}

void Logger::Shutdown() noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;

    ::_InterlockedExchange(&g_state.running, 0);
    ::WakeAllConditionVariable(&g_state.wake_cv);
    if (g_state.writer_thread) {
        ::WaitForSingleObject(g_state.writer_thread, INFINITE);
        ::CloseHandle(g_state.writer_thread);
        g_state.writer_thread = nullptr;
    }
    if (g_state.out_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_state.out_file);
        g_state.out_file = INVALID_HANDLE_VALUE;
    }
    if (g_state.ring) {
        ::VirtualFree(g_state.ring, 0, MEM_RELEASE);
        g_state.ring = nullptr;
    }
    ::_InterlockedExchange(&g_inited, 0);
}

void Logger::Flush() noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;
    // Spin briefly until the writer has caught up.
    for (int i = 0; i < 1000; ++i) {
        LONG64 head = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
        LONG64 tail = ::_InterlockedExchangeAdd64(&g_state.dequeue_pos, 0);
        if (tail >= head) break;
        ::WakeAllConditionVariable(&g_state.wake_cv);
        ::Sleep(1);
    }
    if (g_state.out_file != INVALID_HANDLE_VALUE) ::FlushFileBuffers(g_state.out_file);
}

void Logger::SetMinSeverity(LogSeverity s) noexcept {
    ::_InterlockedExchange(&g_state.min_severity, static_cast<LONG>(s));
}

bool Logger::Enabled(LogSeverity s) noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return false;
    return static_cast<LONG>(s) >= ::_InterlockedExchangeAdd(&g_state.min_severity, 0);
}

u64 Logger::DroppedCount() noexcept {
    return static_cast<u64>(::_InterlockedExchangeAdd64(&g_state.dropped, 0));
}

void Logger::Write(LogSeverity sev, SourceLoc loc, const char* fmt, ...) noexcept {
    if (!::_InterlockedExchangeAdd(&g_inited, 0)) return;
    if (static_cast<LONG>(sev) < ::_InterlockedExchangeAdd(&g_state.min_severity, 0)) return;

    // Reserve a slot via Vyukov MPMC enqueue.
    LONG64 pos = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
    Cell* cell = nullptr;
    while (true) {
        cell = &g_state.ring[pos & g_state.mask];
        LONG64 seq = ::_InterlockedExchangeAdd64(&cell->sequence, 0);
        LONG64 dif = seq - pos;
        if (dif == 0) {
            if (::_InterlockedCompareExchange64(&g_state.enqueue_pos, pos + 1, pos) == pos) break;
        } else if (dif < 0) {
            // Ring full — drop.
            ::_InterlockedIncrement64(&g_state.dropped);
            return;
        } else {
            pos = ::_InterlockedExchangeAdd64(&g_state.enqueue_pos, 0);
        }
    }

    cell->severity  = sev;
    cell->loc       = loc;
    cell->thread_id = ::GetCurrentThreadId();
    ::QueryPerformanceCounter(&cell->timestamp);

    va_list ap;
    va_start(ap, fmt);
    int n = ::vsnprintf(cell->message, kMessageMax, fmt ? fmt : "(null)", ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (static_cast<u32>(n) >= kMessageMax) n = static_cast<int>(kMessageMax - 1);
    cell->message_len = static_cast<u16>(n);

    // Publish — release on ARM64, ordinary store-with-fence on x64.
    ::_InterlockedExchange64(&cell->sequence, pos + 1);

    // Wake writer if it's parked.
    ::WakeConditionVariable(&g_state.wake_cv);
}

} // namespace acs
