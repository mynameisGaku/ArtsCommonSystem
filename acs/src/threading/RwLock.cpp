// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — FRwLock 実装
// -----------------------------------------------------------------------------
// Win32 SRWLOCK の Shared / Exclusive 両モードを直接呼び出す薄いラッパ。
// =============================================================================
#include "threading/RwLock.h"
#include "foundation/Platform.h"

namespace acs {

FRwLock::FRwLock() noexcept {
    InitializeSRWLock(reinterpret_cast<SRWLOCK*>(&_srw[0]));
}

// 共有（読み取り）ロック
void FRwLock::LockShared()    noexcept { AcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&_srw[0])); }
bool FRwLock::TryLockShared() noexcept { return TryAcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&_srw[0])) != 0; }
void FRwLock::UnlockShared()  noexcept { ReleaseSRWLockShared(reinterpret_cast<SRWLOCK*>(&_srw[0])); }

// 排他（書き込み）ロック
void FRwLock::LockExclusive()    noexcept { AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])); }
bool FRwLock::TryLockExclusive() noexcept { return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])) != 0; }
void FRwLock::UnlockExclusive()  noexcept { ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])); }

} // namespace acs
