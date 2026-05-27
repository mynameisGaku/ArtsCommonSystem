// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — RAII ロックガード（<mutex> の lock_guard / shared_lock 代替）
// -----------------------------------------------------------------------------
// スコープ脱出時に自動的にロックを解放するヘルパ。
// 関数の早期 return / 例外なしパス / FPanic 経路でも確実に解放される。
//
// 使い方:
//   { FScopedLock lk(mutex); ... }                  // FMutex 排他
//   { ScopedSharedLock lk(rwlock); ... }            // RwLock 共有
//   { ScopedExclusiveLock lk(rwlock); ... }         // RwLock 排他
// =============================================================================
#pragma once

#include "threading/Mutex.h"
#include "threading/RwLock.h"

namespace acs {

// ---- FMutex 用 RAII ガード ------------------------------------------------
class FScopedLock {
public:
    explicit FScopedLock(FMutex& m) noexcept : m_M(m) { m_M.Lock(); }
    ~FScopedLock() noexcept { m_M.Unlock(); }

    // ガードはコピー / ムーブ不可
    FScopedLock(const FScopedLock&) = delete;
    FScopedLock& operator=(const FScopedLock&) = delete;
private:
    FMutex& m_M;
};

// ---- RwLock 共有（読み取り）用 RAII ガード -----------------------------
class ScopedSharedLock {
public:
    explicit ScopedSharedLock(RwLock& r) noexcept : m_R(r) { m_R.LockShared(); }
    ~ScopedSharedLock() noexcept { m_R.UnlockShared(); }
    ScopedSharedLock(const ScopedSharedLock&) = delete;
    ScopedSharedLock& operator=(const ScopedSharedLock&) = delete;
private:
    RwLock& m_R;
};

// ---- RwLock 排他（書き込み）用 RAII ガード -----------------------------
class ScopedExclusiveLock {
public:
    explicit ScopedExclusiveLock(RwLock& r) noexcept : m_R(r) { m_R.LockExclusive(); }
    ~ScopedExclusiveLock() noexcept { m_R.UnlockExclusive(); }
    ScopedExclusiveLock(const ScopedExclusiveLock&) = delete;
    ScopedExclusiveLock& operator=(const ScopedExclusiveLock&) = delete;
private:
    RwLock& m_R;
};

} // namespace acs
