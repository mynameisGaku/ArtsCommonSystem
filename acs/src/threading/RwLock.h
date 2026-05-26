// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — Reader/Writer Lock（Win32 SRWLOCK ベース）
// -----------------------------------------------------------------------------
// SRWLOCK の Shared / Exclusive モードを両方使用した R/W ロック。
// 読み取りが多く書き込みが少ないデータ（設定値、リソースカタログ等）で
// スループットを向上させる。std::shared_mutex 相当。
//
// 注意:
//   - Shared と Exclusive を同じスレッドで再帰取得することはできない
//   - 公平性は OS 任せ（書き込み starvation の可能性あり）
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

class FRwLock {
public:
    FRwLock() noexcept;
    ~FRwLock() noexcept = default;

    FRwLock(const FRwLock&) = delete;
    FRwLock& operator=(const FRwLock&) = delete;

    // ---- 共有ロック（読み取り）------------------------------------------
    void LockShared()    noexcept;       // 読み取りロック取得（ブロッキング）
    bool TryLockShared() noexcept;       // 試行
    void UnlockShared()  noexcept;       // 解除

    // ---- 排他ロック（書き込み）------------------------------------------
    void LockExclusive()    noexcept;    // 書き込みロック取得（ブロッキング）
    bool TryLockExclusive() noexcept;    // 試行
    void UnlockExclusive()  noexcept;    // 解除

private:
    void* _srw[1];                       // SRWLOCK 実体
};

} // namespace acs
