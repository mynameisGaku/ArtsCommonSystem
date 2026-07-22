// SPDX-License-Identifier: Apache-2.0
// `_CrtDumpMemoryLeaks` のプロセス全体判定を、静的登録を持つ unit-test runner から
// 分離して検証する。通常のテスト実行ファイルには終了時まで生存するレジストリ等が
// あるため、プロセス全体の clean 判定専用バイナリとして保つ。
#include "memory/CrtDebugHeapDiagnostics.h"

#include <cstdlib>

namespace {

bool TextEquals(const char* left, const char* right) noexcept
{
    if (!left || !right) return false;
    while (*left != '\0' && *right != '\0') {
        if (*left++ != *right++) return false;
    }
    return *left == '\0' && *right == '\0';
}

int RunProbe(bool create_intentional_leak) noexcept
{
    void* intentional_leak = nullptr;
    if (create_intentional_leak) {
        intentional_leak = ::malloc(64u);
        if (!intentional_leak) return 76;
    }

    const acs::FCrtDebugHeapProcessLeakReport report =
        acs::FCrtDebugHeapDiagnostics::DumpProcessMemoryLeaks(!create_intentional_leak);
    if (intentional_leak) ::free(intentional_leak);

    if (!report.bSupported || !report.bInspectionSucceeded) return 77;
    if (report.bLeakDetected != create_intentional_leak) return 78;
    return 0;
}

} // namespace

int main(int argument_count, char** arguments)
{
    if (argument_count != 2) return 79;
    if (TextEquals(arguments[1], "--clean")) return RunProbe(false);
    if (TextEquals(arguments[1], "--positive")) return RunProbe(true);
    return 80;
}
