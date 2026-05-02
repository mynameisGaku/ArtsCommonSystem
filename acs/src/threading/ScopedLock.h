// =============================================================================
// ACS Threading — RAII ロックガード（<mutex> の lock_guard / shared_lock 代替）
// -----------------------------------------------------------------------------
// スコープ脱出時に自動的にロックを解放するヘルパ。
// 関数の早期 return / 例外なしパス / Panic 経路でも確実に解放される。
//
// 使い方:
//   { ScopedLock lk(mutex); ... }                  // Mutex 排他
//   { ScopedSharedLock lk(rwlock); ... }            // RwLock 共有
//   { ScopedExclusiveLock lk(rwlock); ... }         // RwLock 排他
// =============================================================================
#pragma once

#include "threading/Mutex.h"
#include "threading/RwLock.h"

namespace acs {

// ---- Mutex 用 RAII ガード ------------------------------------------------
class ScopedLock {
public:
    explicit ScopedLock(Mutex& m) noexcept : _m(m) { _m.Lock(); }
    ~ScopedLock() noexcept { _m.Unlock(); }

    // ガードはコピー / ムーブ不可
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
private:
    Mutex& _m;
};

// ---- RwLock 共有（読み取り）用 RAII ガード -----------------------------
class ScopedSharedLock {
public:
    explicit ScopedSharedLock(RwLock& r) noexcept : _r(r) { _r.LockShared(); }
    ~ScopedSharedLock() noexcept { _r.UnlockShared(); }
    ScopedSharedLock(const ScopedSharedLock&) = delete;
    ScopedSharedLock& operator=(const ScopedSharedLock&) = delete;
private:
    RwLock& _r;
};

// ---- RwLock 排他（書き込み）用 RAII ガード -----------------------------
class ScopedExclusiveLock {
public:
    explicit ScopedExclusiveLock(RwLock& r) noexcept : _r(r) { _r.LockExclusive(); }
    ~ScopedExclusiveLock() noexcept { _r.UnlockExclusive(); }
    ScopedExclusiveLock(const ScopedExclusiveLock&) = delete;
    ScopedExclusiveLock& operator=(const ScopedExclusiveLock&) = delete;
private:
    RwLock& _r;
};

} // namespace acs
