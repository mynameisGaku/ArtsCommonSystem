#include "foundation/Panic.h"
#include "foundation/Platform.h"
#include "foundation/StackTrace.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace acs {

namespace {

SRWLOCK   g_panic_lock = SRWLOCK_INIT;
PanicHook g_hook = nullptr;
void*     g_hook_user = nullptr;

void WriteAll(HANDLE h, const char* buf, usize n) noexcept {
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    DWORD written = 0;
    ::WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr);
}

void Emit(const char* buf, usize n) noexcept {
    HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    WriteAll(err, buf, n);
    ::OutputDebugStringA(buf);
    if (g_hook) g_hook(g_hook_user, buf, n);
}

void StackSink(void* /*user*/, const char* line, usize len) noexcept {
    Emit(line, len);
}

} // namespace

void SetPanicHook(PanicHook hook, void* user) noexcept {
    AcquireSRWLockExclusive(&g_panic_lock);
    g_hook = hook;
    g_hook_user = user;
    ReleaseSRWLockExclusive(&g_panic_lock);
}

ACS_NORETURN void Panic(SourceLoc loc, const char* expr, const char* fmt, ...) noexcept {
    AcquireSRWLockExclusive(&g_panic_lock);

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

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    int mn = ::vsnprintf(msg, sizeof(msg), fmt ? fmt : "(no message)", ap);
    va_end(ap);
    if (mn < 0) mn = 0;
    Emit(msg, static_cast<usize>(mn));
    Emit("\n stack    :\n", 13);

    StackTrace st;
    st.Capture(/*skip*/ 2);
    st.Resolve();
    st.Print(&StackSink, nullptr);

    Emit("===================================================\n", 52);

    ReleaseSRWLockExclusive(&g_panic_lock);

    if (::IsDebuggerPresent()) {
        ACS_DEBUGBREAK();
    }
    ::FlushFileBuffers(::GetStdHandle(STD_ERROR_HANDLE));
    ::TerminateProcess(::GetCurrentProcess(), 3);
    // Unreachable — keep [[noreturn]] contract.
    for (;;) {}
}

} // namespace acs
