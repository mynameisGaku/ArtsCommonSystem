// SPDX-License-Identifier: Apache-2.0
// ACS Threading — FRwLock 実装
//
// Win32 SRWLOCK の Shared / Exclusive 両モードを直接呼び出す薄いラッパ。
#include "threading/RwLock.h"
#include "foundation/Platform.h"

namespace acs {

/** SRWLOCK_INIT 相当の初期化を呼ぶ。 */
FRwLock::FRwLock() noexcept {
    InitializeSRWLock(reinterpret_cast<SRWLOCK*>(&m_Srw[0]));
}

/** 共有 (読み取り) ロックを取得する。 */
void FRwLock::LockShared()    noexcept { AcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&m_Srw[0])); }

/** 共有 (読み取り) ロックの取得を試みる (取れたら true)。 */
bool FRwLock::TryLockShared() noexcept { return TryAcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&m_Srw[0])) != 0; }

/** 共有 (読み取り) ロックを解放する。 */
void FRwLock::UnlockShared()  noexcept { ReleaseSRWLockShared(reinterpret_cast<SRWLOCK*>(&m_Srw[0])); }

/** 排他 (書き込み) ロックを取得する。 */
void FRwLock::LockExclusive()    noexcept { AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&m_Srw[0])); }

/** 排他 (書き込み) ロックの取得を試みる (取れたら true)。 */
bool FRwLock::TryLockExclusive() noexcept { return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&m_Srw[0])) != 0; }

/** 排他 (書き込み) ロックを解放する。 */
void FRwLock::UnlockExclusive()  noexcept { ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&m_Srw[0])); }

} // namespace acs
