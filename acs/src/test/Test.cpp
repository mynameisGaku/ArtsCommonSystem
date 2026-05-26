// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Test — Test フレームワーク 実装
// -----------------------------------------------------------------------------
// グローバル単方向リストへの登録、全テスト実行、結果集計。
// 失敗カウンタは thread_local にして、並列テストにも対応可能。
// =============================================================================
#include "test/Test.h"
#include "foundation/Platform.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"

#include <cstdarg>
#include <cstdio>

namespace acs::test {

namespace {
FTestCase* g_head = nullptr;
FTestCase* g_tail = nullptr;
FMutex     g_reg_lock;
// テスト本体内の失敗数カウンタ（テストごとに 0 にリセット）
thread_local int g_current_failures = 0;
}

// ACS_TEST マクロから呼ばれる: テストケースを末尾に追加
void Register(FTestCase* tc) noexcept {
    FScopedLock lk(g_reg_lock);
    if (!g_head) g_head = tc;
    else         g_tail->next = tc;
    g_tail = tc;
}

// EXPECT_* マクロから呼ばれる失敗報告
void RecordFailure(FSourceLoc loc, const char* expr, const char* fmt, ...) noexcept {
    ++g_current_failures;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    ::vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    ::fprintf(stderr, "    [FAIL] %s:%u (%s)\n      expr: %s\n      msg : %s\n",
              loc.File(), loc.Line(), loc.Function(), expr, msg);
}

// 補助情報の出力（合否判定には影響しない）
void RecordInfo(FSourceLoc loc, const char* fmt, ...) noexcept {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    ::vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    ::fprintf(stderr, "    [INFO] %s:%u %s\n", loc.File(), loc.Line(), msg);
}

// 全テスト実行、結果サマリ出力
int RunAll() noexcept {
    u32 total = 0, passed = 0, failed = 0;
    for (FTestCase* tc = g_head; tc; tc = tc->next) ++total;

    ::printf("[ACS Test] running %u tests\n", total);
    for (FTestCase* tc = g_head; tc; tc = tc->next) {
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
