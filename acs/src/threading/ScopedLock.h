// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — RAII ロックガード（<mutex> の lock_guard / shared_lock 代替）
// -----------------------------------------------------------------------------
// スコープ脱出時に自動的にロックを解放するヘルパ。
// 関数の早期 return / 例外なしパス / Panic 経路でも確実に解放される。
//
// 使い方:
//   { FScopedLock lk(mutex); ... }                  // FMutex 排他
//   { FScopedSharedLock lk(rwlock); ... }            // FRwLock 共有
//   { FScopedExclusiveLock lk(rwlock); ... }         // FRwLock 排他
// =============================================================================
#pragma once

#include "threading/Mutex.h"
#include "threading/RwLock.h"

namespace acs {

// ---- FMutex 用 RAII ガード ------------------------------------------------
class FScopedLock {
public:
    explicit FScopedLock(FMutex& m) noexcept : _m(m) { _m.Lock(); }
    ~FScopedLock() noexcept { _m.Unlock(); }

    // ガードはコピー / ムーブ不可
    FScopedLock(const FScopedLock&) = delete;
    FScopedLock& operator=(const FScopedLock&) = delete;
private:
    FMutex& _m;
};

// ---- FRwLock 共有（読み取り）用 RAII ガード -----------------------------
class FScopedSharedLock {
public:
    explicit FScopedSharedLock(FRwLock& r) noexcept : _r(r) { _r.LockShared(); }
    ~FScopedSharedLock() noexcept { _r.UnlockShared(); }
    FScopedSharedLock(const FScopedSharedLock&) = delete;
    FScopedSharedLock& operator=(const FScopedSharedLock&) = delete;
private:
    FRwLock& _r;
};

// ---- FRwLock 排他（書き込み）用 RAII ガード -----------------------------
class FScopedExclusiveLock {
public:
    explicit FScopedExclusiveLock(FRwLock& r) noexcept : _r(r) { _r.LockExclusive(); }
    ~FScopedExclusiveLock() noexcept { _r.UnlockExclusive(); }
    FScopedExclusiveLock(const FScopedExclusiveLock&) = delete;
    FScopedExclusiveLock& operator=(const FScopedExclusiveLock&) = delete;
private:
    FRwLock& _r;
};

} // namespace acs
