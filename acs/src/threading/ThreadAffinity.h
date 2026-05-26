// SPDX-License-Identifier: Apache-2.0
// ThreadAffinity — クラスごとに「どのスレッドから呼べるか」を Debug ビルドで検証する
//
// 使い方:
//   class MyView {
//       ACS_THREAD_AFFINITY_FIELD();
//   public:
//       void Set(int v) noexcept {
//           ACS_THREAD_AFFINITY_CHECK();   // 最初の呼び出し時のスレッドに固定
//           // ...
//       }
//   };
//
// ACS_THREAD_AFFINITY_FIELD: クラス内に検査用メンバを生やす (Debug only、Release で 0 byte)
// ACS_THREAD_AFFINITY_CHECK: 現在のスレッドが初回スレッドと違ったら panic
//
// Release ビルドでは全部 no-op。
#pragma once

#include "foundation/Compiler.h"
#include "foundation/Assert.h"

#if ACS_ASSERTS_ENABLED

#include "threading/ThreadId.h"

namespace acs::detail {

class FThreadAffinityGuard {
public:
    FThreadAffinityGuard() noexcept = default;

    void Check() noexcept {
        const u64 cur = static_cast<u64>(acs::CurrentThreadId().raw);
        u64 expected = _expected_thread;
        if (expected == 0) {
            // 初回: 現在のスレッドを固定
            _expected_thread = cur;
        } else {
            ACS_ASSERTF(expected == cur,
                        "thread affinity violated (expected tid=%llu, got tid=%llu)",
                        (unsigned long long)expected, (unsigned long long)cur);
        }
    }

    // 明示的にリセット (テスト等で再固定したい場合)
    void Reset() noexcept { _expected_thread = 0; }

private:
    u64 _expected_thread = 0;
};

} // namespace acs::detail

#define ACS_THREAD_AFFINITY_FIELD() \
    mutable ::acs::detail::FThreadAffinityGuard _acs_affinity_guard

#define ACS_THREAD_AFFINITY_CHECK() \
    _acs_affinity_guard.Check()

#define ACS_THREAD_AFFINITY_RESET() \
    _acs_affinity_guard.Reset()

#else  // Release: no-op (0 byte field, empty check)

#define ACS_THREAD_AFFINITY_FIELD()
#define ACS_THREAD_AFFINITY_CHECK() ((void)0)
#define ACS_THREAD_AFFINITY_RESET() ((void)0)

#endif
