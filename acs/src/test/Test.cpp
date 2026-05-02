#include "test/Test.h"
#include "foundation/Platform.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"

#include <cstdarg>
#include <cstdio>

namespace acs::test {

namespace {
TestCase* g_head = nullptr;
TestCase* g_tail = nullptr;
Mutex     g_reg_lock;
thread_local int g_current_failures = 0;
}

void Register(TestCase* tc) noexcept {
    ScopedLock lk(g_reg_lock);
    if (!g_head) g_head = tc;
    else         g_tail->next = tc;
    g_tail = tc;
}

void RecordFailure(SourceLoc loc, const char* expr, const char* fmt, ...) noexcept {
    ++g_current_failures;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    ::vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    ::fprintf(stderr, "    [FAIL] %s:%u (%s)\n      expr: %s\n      msg : %s\n",
              loc.File(), loc.Line(), loc.Function(), expr, msg);
}

void RecordInfo(SourceLoc loc, const char* fmt, ...) noexcept {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    ::vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    ::fprintf(stderr, "    [INFO] %s:%u %s\n", loc.File(), loc.Line(), msg);
}

int RunAll() noexcept {
    u32 total = 0, passed = 0, failed = 0;
    for (TestCase* tc = g_head; tc; tc = tc->next) ++total;

    ::printf("[ACS Test] running %u tests\n", total);
    for (TestCase* tc = g_head; tc; tc = tc->next) {
        ::printf("  %s.%s ... ", tc->suite, tc->name);
        ::fflush(stdout);
        g_current_failures = 0;
        tc->fn();
        if (g_current_failures == 0) {
            ::printf("OK\n");
            ++passed;
        } else {
            ::printf("FAIL (%d failures)\n", g_current_failures);
            ++failed;
        }
    }
    ::printf("[ACS Test] passed=%u failed=%u\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace acs::test
