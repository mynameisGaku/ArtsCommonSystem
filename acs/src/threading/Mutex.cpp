// SPDX-License-Identifier: Apache-2.0
// ACS Threading — FMutex 実装
//
// Win32 SRWLOCK の薄いラッパ。SRWLOCK は静的初期化フリーかつ非常に軽量。
#include "threading/Mutex.h"
#include "foundation/Platform.h"

// SRWLOCK は今のところ void* と同じサイズ。サイズ変更が将来あれば
// ストレージレイアウトを再検討する必要がある（コンパイル時検出）。
static_assert(sizeof(SRWLOCK) == sizeof(void*),
              "SRWLOCK shape changed — FMutex storage layout must be updated");

namespace acs {

/** SRWLOCK_INIT 相当の初期化を呼ぶ。 */
FMutex::FMutex() noexcept {
    InitializeSRWLock(reinterpret_cast<SRWLOCK*>(&m_Srw[0]));
}

/** 排他 (書き込み側) ロックを取得する。 */
void FMutex::Lock() noexcept {
    AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&m_Srw[0]));
}

/** 排他ロックの取得を試みる (取れたら true)。 */
bool FMutex::TryLock() noexcept {
    return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&m_Srw[0])) != 0;
}

/** 排他ロックを解除する。 */
void FMutex::Unlock() noexcept {
    ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&m_Srw[0]));
}

} // namespace acs
