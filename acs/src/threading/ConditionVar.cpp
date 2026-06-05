// SPDX-License-Identifier: Apache-2.0
// ACS Threading — ConditionVar 実装
//
// Win32 CONDITION_VARIABLE は SRWLOCK ペアで動作する。FMutex も内部で
// SRWLOCK を持っているため、`SleepConditionVariableSRW` を直接利用可能。
#include "threading/ConditionVar.h"
#include "foundation/Platform.h"

// CONDITION_VARIABLE は今のところ void* と同サイズ。
static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void*),
              "CONDITION_VARIABLE shape changed — storage layout must be updated");

namespace acs {

/** CONDITION_VARIABLE を初期化する。 */
ConditionVar::ConditionVar() noexcept {
    InitializeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&m_Cv[0]));
}

/** 無限待機する。m は Wait 中に一時開放され、復帰時に再取得される。 */
void ConditionVar::Wait(FMutex& m) noexcept {
    SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(&m_Cv[0]),
                              reinterpret_cast<SRWLOCK*>(&m), INFINITE, 0);
}

/** タイムアウト付きで待機する (true=起こされた、false=タイムアウト)。 */
bool ConditionVar::WaitFor(FMutex& m, u32 timeout_ms) noexcept {
    const BOOL ok = SleepConditionVariableSRW(reinterpret_cast<CONDITION_VARIABLE*>(&m_Cv[0]),
                                        reinterpret_cast<SRWLOCK*>(&m),
                                        static_cast<DWORD>(timeout_ms), 0);
    return ok != 0;
}

/** 待機中のスレッドを 1 つ起こす。 */
void ConditionVar::NotifyOne() noexcept { WakeConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&m_Cv[0])); }

/** 待機中の全スレッドを起こす。 */
void ConditionVar::NotifyAll() noexcept { WakeAllConditionVariable(reinterpret_cast<CONDITION_VARIABLE*>(&m_Cv[0])); }

} // namespace acs
