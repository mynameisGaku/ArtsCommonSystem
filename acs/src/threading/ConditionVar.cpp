// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — FConditionVar 実装
// -----------------------------------------------------------------------------
// Win32 CONDITION_VARIABLE は SRWLOCK ペアで動作する。FMutex も内部で
// SRWLOCK を持っているため、`SleepConditionVariableSRW` を直接利用可能。
// =============================================================================
#include "threading/ConditionVar.h"
#include "foundation/Platform.h"

// CONDITION_VARIABLE は今のところ void* と同サイズ。
static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void*),
              "CONDITION_VARIABLE shape changed — storage layout must be updated");

namespace acs {

FConditionVar::FConditionVar() noexcept {
    InitializeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0]));
}

// 無限待機。FMutex は Wait 中に一時開放され、復帰時に再取得される。
void FConditionVar::Wait(FMutex& m) noexcept {
    SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0]),
                              reinterpret_cast<SRWLOCK*>(&m), INFINITE, 0);
}

// タイムアウト付き待機。戻り値: true=起こされた, false=タイムアウト
bool FConditionVar::WaitFor(FMutex& m, u32 timeout_ms) noexcept {
    BOOL ok = SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0]),
                                        reinterpret_cast<SRWLOCK*>(&m),
                                        static_cast<DWORD>(timeout_ms), 0);
    return ok != 0;
}

void FConditionVar::NotifyOne() noexcept { WakeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0])); }
void FConditionVar::NotifyAll() noexcept { WakeAllConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&_cv[0])); }

} // namespace acs
