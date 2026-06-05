// SPDX-License-Identifier: Apache-2.0
// ACS Threading — 排他 FMutex（Win32 SRWLOCK ベース）
//
// SRWLOCK の Exclusive モードを使用した最小限の FMutex。
// std::mutex 相当だが、より軽量（CRITICAL_SECTION 不使用、初期化フリー）。
//
// SRWLOCK のサイズは void* 1 個分なので、<windows.h> を露出させずに
// void* 配列として持つ。実装側でキャスト。
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

/**
 * Win32 SRWLOCK の Exclusive モードによる排他ミューテックス (std::mutex 代替)。
 *
 * @details
 * 軽量で初期化フリー (CRITICAL_SECTION 不使用)。再帰取得は不可。コピー不可。
 * RAII で扱う場合は FScopedLock を使う。
 */
class FMutex {
public:
    /** SRWLOCK を初期化して構築する。 */
    FMutex() noexcept;

    /** 破棄する (SRWLOCK は明示的解放不要)。 */
    ~FMutex() noexcept = default;

    /** コピー禁止 (std::mutex と同様)。 */
    FMutex(const FMutex&) = delete;

    /** コピー代入も禁止。 */
    FMutex& operator=(const FMutex&) = delete;

    /** 排他ロックを取得する (取得できるまでブロックする)。 */
    void Lock()    noexcept;

    /**
     * 排他ロックの取得を試みる (ブロックしない)。
     *
     * @return ロックを取得できたら true、取得できなければ false で即時帰還。
     */
    bool TryLock() noexcept;

    /** 保持している排他ロックを解放する。 */
    void Unlock()  noexcept;

private:
    /** SRWLOCK 実体。<windows.h> をヘッダで取り込まないため void* で持つ。 */
    void* m_Srw[1];
};

} // namespace acs
