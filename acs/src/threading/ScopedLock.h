// SPDX-License-Identifier: Apache-2.0
// ACS Threading — RAII ロックガード（<mutex> の lock_guard / shared_lock 代替）
//
// スコープ脱出時に自動的にロックを解放するヘルパ。
// 関数の早期 return / 例外なしパス / FPanic 経路でも確実に解放される。
//
// 使い方:
//   { FScopedLock lk(mutex); ... }                  // FMutex 排他
//   { FScopedSharedLock lk(rwlock); ... }            // FRwLock 共有
//   { FScopedExclusiveLock lk(rwlock); ... }         // FRwLock 排他
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
 * FRwLock を共有 (読み取り) ロックする RAII ガード (shared_lock 代替)。
 *
 * @details 構築時に LockShared し、スコープ脱出時に UnlockShared する。コピー / ムーブ不可。
 */
class FScopedSharedLock {
public:
    /**
     * 構築と同時に共有 (読み取り) ロックを取得する。
     *
     * @param r ロック対象の FRwLock (ガードの寿命より長く生存すること)。
     */
    explicit FScopedSharedLock(FRwLock& r) noexcept : m_R(r) { m_R.LockShared(); }

    /** スコープ脱出時に共有ロックを解放する。 */
    ~FScopedSharedLock() noexcept { m_R.UnlockShared(); }

    /** コピー / ムーブ禁止。 */
    FScopedSharedLock(const FScopedSharedLock&) = delete;

    /** コピー代入も禁止。 */
    FScopedSharedLock& operator=(const FScopedSharedLock&) = delete;
private:
    /** ガード対象の FRwLock への参照。 */
    FRwLock& m_R;
};

/**
 * FRwLock を排他 (書き込み) ロックする RAII ガード。
 *
 * @details 構築時に LockExclusive し、スコープ脱出時に UnlockExclusive する。コピー / ムーブ不可。
 */
class FScopedExclusiveLock {
public:
    /**
     * 構築と同時に排他 (書き込み) ロックを取得する。
     *
     * @param r ロック対象の FRwLock (ガードの寿命より長く生存すること)。
     */
    explicit FScopedExclusiveLock(FRwLock& r) noexcept : m_R(r) { m_R.LockExclusive(); }

    /** スコープ脱出時に排他ロックを解放する。 */
    ~FScopedExclusiveLock() noexcept { m_R.UnlockExclusive(); }

    /** コピー / ムーブ禁止。 */
    FScopedExclusiveLock(const FScopedExclusiveLock&) = delete;

    /** コピー代入も禁止。 */
    FScopedExclusiveLock& operator=(const FScopedExclusiveLock&) = delete;
private:
    /** ガード対象の FRwLock への参照。 */
    FRwLock& m_R;
};

} // namespace acs
