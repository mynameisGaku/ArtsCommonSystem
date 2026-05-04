// =============================================================================
// ACS Threading — RwLock 実装
// -----------------------------------------------------------------------------
// Win32 SRWLOCK の Shared / Exclusive 両モードを直接呼び出す薄いラッパ。
// =============================================================================
#include "threading/RwLock.h"
#include "foundation/Platform.h"

namespace acs {

RwLock::RwLock() noexcept {
    InitializeSRWLock(reinterpret_cast<SRWLOCK*>(&_srw[0]));
}

// 共有（読み取り）ロック
void RwLock::LockShared()    noexcept { AcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&_srw[0])); }
bool RwLock::TryLockShared() noexcept { return TryAcquireSRWLockShared(reinterpret_cast<SRWLOCK*>(&_srw[0])) != 0; }
void RwLock::UnlockShared()  noexcept { ReleaseSRWLockShared(reinterpret_cast<SRWLOCK*>(&_srw[0])); }

// 排他（書き込み）ロック
void RwLock::LockExclusive()    noexcept { AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])); }
bool RwLock::TryLockExclusive() noexcept { return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])) != 0; }
void RwLock::UnlockExclusive()  noexcept { ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])); }

} // namespace acs
