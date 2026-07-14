// SPDX-License-Identifier: Apache-2.0
// Editor ABI DLL の生成・破棄契約を、GPU 接続なしで実 DLL 境界から検証する。
#include <windows.h>
#include <cstdio>

extern "C" __declspec(dllimport) void* acs_editor_create(void);
extern "C" __declspec(dllimport) void acs_editor_destroy(void* handle);
extern "C" __declspec(dllimport) int acs_editor_node_count(void* handle);

namespace {

/** 現在プロセスが保持する Win32 HANDLE 数を返す。 */
DWORD ProcessHandleCount() noexcept
{
    DWORD count = 0;
    return ::GetProcessHandleCount(::GetCurrentProcess(), &count) ? count : 0;
}

/** 1 ホストを生成し、デモシーンを確認して破棄する。 */
bool RunOneLifecycle() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    const bool scene_ready = acs_editor_node_count(host) > 0;
    acs_editor_destroy(host);
    return scene_ready;
}

} // namespace

int main()
{
    acs_editor_destroy(nullptr); // null 破棄は常に no-op であることも通す。

    // OS とランタイムの初回遅延初期化を基準値から除外する。
    if (!RunOneLifecycle()) return 1;
    const DWORD baseline_handles = ProcessHandleCount();
    if (baseline_handles == 0) return 2;

    for (int cycle = 0; cycle < 8; ++cycle) {
        if (!RunOneLifecycle()) return 3;
    }

    // 複数ホストの一方を先に破棄しても、共有基盤と残りのホストは生存する。
    void* const first = acs_editor_create();
    void* const second = acs_editor_create();
    if (first == nullptr || second == nullptr) {
        acs_editor_destroy(first);
        acs_editor_destroy(second);
        return 4;
    }
    if (acs_editor_node_count(first) <= 0 || acs_editor_node_count(second) <= 0) {
        acs_editor_destroy(first);
        acs_editor_destroy(second);
        return 5;
    }

    acs_editor_destroy(first);
    if (acs_editor_node_count(second) <= 0) {
        acs_editor_destroy(second);
        return 6;
    }
    acs_editor_destroy(second);

    const DWORD final_handles = ProcessHandleCount();
    if (final_handles > baseline_handles + 1u) {
        std::printf("Editor ABI handle leak: before=%lu after=%lu\n", static_cast<unsigned long>(baseline_handles),
                    static_cast<unsigned long>(final_handles));
        return 7;
    }
    return 0;
}
