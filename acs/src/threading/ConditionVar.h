// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — 条件変数（Win32 CONDITION_VARIABLE ベース）
// -----------------------------------------------------------------------------
// std::condition_variable 相当。Mutex と組み合わせて「条件が満たされるまで
// スレッドを停止し、別スレッドからの NotifyOne / NotifyAll で起こす」。
//
// 典型的な使用例（生産者・消費者）:
//   ScopedLock lk(m);
//   while (queue.IsEmpty()) cv.Wait(m);
//   auto item = queue.Pop();
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "threading/Mutex.h"

namespace acs {

class ConditionVar {
public:
    ConditionVar() noexcept;
    ~ConditionVar() noexcept = default;

    ConditionVar(const ConditionVar&) = delete;
    ConditionVar& operator=(const ConditionVar&) = delete;

    // 待機。呼び出し前に Mutex を Lock しておくこと。
    // ブロック中は内部で一時 Unlock し、起こされたら再 Lock してから返る。
    void Wait(Mutex& m) noexcept;

    // タイムアウト付き待機。timeout_ms ミリ秒待っても起こされなければ false。
    // 起こされたら true。
    bool WaitFor(Mutex& m, u32 timeout_ms) noexcept;

    void NotifyOne() noexcept;       // 待機中の 1 スレッドを起こす
    void NotifyAll() noexcept;       // 待機中の全スレッドを起こす

private:
    void* _cv[1];                    // CONDITION_VARIABLE 実体
};

} // namespace acs
