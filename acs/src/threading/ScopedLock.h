// SPDX-License-Identifier: Apache-2.0
// ACS Threading — RAII ロックガード（<mutex> の lock_guard / shared_lock 代替）
//
// スコープ脱出時に自動的にロックを解放するヘルパ。
// 関数の早期 return / 例外なしパス / FPanic 経路でも確実に解放される。
//
// 使い方:
//   { FScopedLock lk(mutex); ... }                  // FMutex 排他
//   { ScopedSharedLock lk(rwlock); ... }            // RwLock 共有
//   { ScopedExclusiveLock lk(rwlock); ... }         // RwLock 排他
#pragma once

#include "threading/Mutex.h"
#include "threading/RwLock.h"

namespace acs {

/**
 * FMutex を排他ロックする RAII ガード (lock_guard 代替)。
 *
 * @details 構築時に Lock し、スコープ脱出時に Unlock する。コピー / ムーブ不可。
 */
class FScopedLock {
public:
    /**
     * 構築と同時に排他ロックを取得する。
     *
     * @param m ロック対象の FMutex (ガードの寿命より長く生存すること)。
     */
    explicit FScopedLock(FMutex& m) noexcept : m_M(m) { m_M.Lock(); }

    /** スコープ脱出時に排他ロックを解放する。 */
    ~FScopedLock() noexcept { m_M.Unlock(); }

    /** コピー / ムーブ禁止 (ロック所有はスコープに束縛)。 */
    FScopedLock(const FScopedLock&) = delete;

    /** コピー代入も禁止。 */
    FScopedLock& operator=(const FScopedLock&) = delete;
private:
    /** ガード対象の FMutex への参照。 */
    FMutex& m_M;
};

/**
 * RwLock を共有 (読み取り) ロックする RAII ガード (shared_lock 代替)。
 *
 * @details 構築時に LockShared し、スコープ脱出時に UnlockShared する。コピー / ムーブ不可。
 */
class ScopedSharedLock {
public:
    /**
     * 構築と同時に共有 (読み取り) ロックを取得する。
     *
     * @param r ロック対象の RwLock (ガードの寿命より長く生存すること)。
     */
    explicit ScopedSharedLock(RwLock& r) noexcept : m_R(r) { m_R.LockShared(); }

    /** スコープ脱出時に共有ロックを解放する。 */
    ~ScopedSharedLock() noexcept { m_R.UnlockShared(); }

    /** コピー / ムーブ禁止。 */
    ScopedSharedLock(const ScopedSharedLock&) = delete;

    /** コピー代入も禁止。 */
    ScopedSharedLock& operator=(const ScopedSharedLock&) = delete;
private:
    /** ガード対象の RwLock への参照。 */
    RwLock& m_R;
};

/**
 * RwLock を排他 (書き込み) ロックする RAII ガード。
 *
 * @details 構築時に LockExclusive し、スコープ脱出時に UnlockExclusive する。コピー / ムーブ不可。
 */
class ScopedExclusiveLock {
public:
    /**
     * 構築と同時に排他 (書き込み) ロックを取得する。
     *
     * @param r ロック対象の RwLock (ガードの寿命より長く生存すること)。
     */
    explicit ScopedExclusiveLock(RwLock& r) noexcept : m_R(r) { m_R.LockExclusive(); }

    /** スコープ脱出時に排他ロックを解放する。 */
    ~ScopedExclusiveLock() noexcept { m_R.UnlockExclusive(); }

    /** コピー / ムーブ禁止。 */
    ScopedExclusiveLock(const ScopedExclusiveLock&) = delete;

    /** コピー代入も禁止。 */
    ScopedExclusiveLock& operator=(const ScopedExclusiveLock&) = delete;
private:
    /** ガード対象の RwLock への参照。 */
    RwLock& m_R;
};

} // namespace acs
