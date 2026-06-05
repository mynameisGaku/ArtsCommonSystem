// SPDX-License-Identifier: Apache-2.0
// ACS Threading — Reader/Writer Lock（Win32 SRWLOCK ベース）
//
// SRWLOCK の Shared / Exclusive モードを両方使用した R/W ロック。
// 読み取りが多く書き込みが少ないデータ（設定値、リソースカタログ等）で
// スループットを向上させる。std::shared_mutex 相当。
//
// 注意:
//   - Shared と Exclusive を同じスレッドで再帰取得することはできない
//   - 公平性は OS 任せ（書き込み starvation の可能性あり）
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

/**
 * Win32 SRWLOCK の Shared / Exclusive 両モードによる読み書きロック (std::shared_mutex 代替)。
 *
 * @details
 * 複数 reader の同時取得を許し writer は排他にする。読み取りが多く書き込みが少ない
 * データのスループット向上に使う。Shared と Exclusive の再帰取得は不可で、公平性は
 * OS 任せ (書き込み starvation の可能性あり)。コピー不可。
 */
class RwLock {
public:
    /** SRWLOCK を初期化して構築する。 */
    RwLock() noexcept;

    /** 破棄する (SRWLOCK は明示的解放不要)。 */
    ~RwLock() noexcept = default;

    /** コピー禁止。 */
    RwLock(const RwLock&) = delete;

    /** コピー代入も禁止。 */
    RwLock& operator=(const RwLock&) = delete;

    /** 共有 (読み取り) ロックを取得する (取得できるまでブロックする)。 */
    void LockShared()    noexcept;

    /**
     * 共有 (読み取り) ロックの取得を試みる (ブロックしない)。
     *
     * @return 取得できたら true、できなければ false。
     */
    bool TryLockShared() noexcept;

    /** 保持している共有 (読み取り) ロックを解放する。 */
    void UnlockShared()  noexcept;

    /** 排他 (書き込み) ロックを取得する (取得できるまでブロックする)。 */
    void LockExclusive()    noexcept;

    /**
     * 排他 (書き込み) ロックの取得を試みる (ブロックしない)。
     *
     * @return 取得できたら true、できなければ false。
     */
    bool TryLockExclusive() noexcept;

    /** 保持している排他 (書き込み) ロックを解放する。 */
    void UnlockExclusive()  noexcept;

private:
    /** SRWLOCK 実体。<windows.h> をヘッダで取り込まないため void* で持つ。 */
    void* m_Srw[1];
};

} // namespace acs
