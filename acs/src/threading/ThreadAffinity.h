// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Compiler.h"
#include "foundation/Assert.h"

#if ACS_ASSERTS_ENABLED

#include "threading/ThreadId.h"

namespace acs::detail {

/**
 * 初回 Check 時のスレッドにアフィニティを固定し、以後の別スレッド呼び出しを検出するガード。
 *
 * @details
 * Debug ビルド (ACS_ASSERTS_ENABLED) でのみ実体を持ち、Release では完全に no-op。
 * ACS_THREAD_AFFINITY_FIELD / ACS_THREAD_AFFINITY_CHECK マクロ経由で使う。
 */
class FThreadAffinityGuard {
public:
    /** 未固定状態 (m_ExpectedThread == 0) で構築する。 */
    FThreadAffinityGuard() noexcept = default;

    /**
     * 現在のスレッドが固定スレッドと一致するか検証する。
     *
     * @details
     * 初回呼び出し時は現在のスレッドを固定対象として記録する。以降の呼び出しで
     * 別スレッドから来た場合は ACS_ASSERTF で panic する。
     */
    void Check() noexcept {
        const u64 cur = static_cast<u64>(acs::CurrentThreadId().raw);
        u64 expected = m_ExpectedThread;
        if (expected == 0) {
            // 初回: 現在のスレッドを固定
            m_ExpectedThread = cur;
        } else {
            ACS_ASSERTF(expected == cur,
                        "thread affinity violated (expected tid=%llu, got tid=%llu)",
                        (unsigned long long)expected, (unsigned long long)cur);
        }
    }

    /** 固定を解除し、次の Check で改めて固定し直せるようにする (テスト等で再固定したい場合)。 */
    void Reset() noexcept { m_ExpectedThread = 0; }

private:
    /** 固定済みスレッド ID (0 = 未固定)。 */
    u64 m_ExpectedThread = 0;
};

} // namespace acs::detail

/** クラス内にアフィニティ検査用メンバを生やす (Debug 版、mutable な guard を宣言)。 */
#define ACS_THREAD_AFFINITY_FIELD() \
    mutable ::acs::detail::FThreadAffinityGuard m_AcsAffinityGuard

/** 現在のスレッドが初回スレッドと一致するか検証する (Debug 版、不一致で panic)。 */
#define ACS_THREAD_AFFINITY_CHECK() \
    m_AcsAffinityGuard.Check()

/** アフィニティ固定を解除する (Debug 版、次の CHECK で再固定)。 */
#define ACS_THREAD_AFFINITY_RESET() \
    m_AcsAffinityGuard.Reset()

#else  // Release: no-op (0 byte field, empty check)

/** クラス内メンバ生成 (Release 版、0 byte の no-op)。 */
#define ACS_THREAD_AFFINITY_FIELD()

/** スレッド検証 (Release 版、空の no-op)。 */
#define ACS_THREAD_AFFINITY_CHECK() ((void)0)

/** アフィニティ固定解除 (Release 版、空の no-op)。 */
#define ACS_THREAD_AFFINITY_RESET() ((void)0)

#endif
