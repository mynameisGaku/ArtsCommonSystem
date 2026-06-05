// SPDX-License-Identifier: Apache-2.0
// ACS Threading — 条件変数（Win32 CONDITION_VARIABLE ベース）
//
// std::condition_variable 相当。FMutex と組み合わせて「条件が満たされるまで
// スレッドを停止し、別スレッドからの NotifyOne / NotifyAll で起こす」。
//
// 典型的な使用例（生産者・消費者）:
//   FScopedLock lk(m);
//   while (queue.IsEmpty()) cv.Wait(m);
//   auto item = queue.Pop();
#pragma once

#include "foundation/Types.h"
#include "threading/Mutex.h"

namespace acs {

/**
 * Win32 CONDITION_VARIABLE による条件変数 (std::condition_variable 代替)。
 *
 * @details
 * FMutex と組み合わせ、条件が満たされるまでスレッドを停止し、別スレッドからの
 * NotifyOne / NotifyAll で起こす。spurious wakeup があり得るため、待機は条件を
 * 検査する while ループ内で行うこと。コピー不可。
 */
class ConditionVar {
public:
    /** CONDITION_VARIABLE を初期化して構築する。 */
    ConditionVar() noexcept;

    /** 破棄する (CONDITION_VARIABLE は明示的解放不要)。 */
    ~ConditionVar() noexcept = default;

    /** コピー禁止。 */
    ConditionVar(const ConditionVar&) = delete;

    /** コピー代入も禁止。 */
    ConditionVar& operator=(const ConditionVar&) = delete;

    /**
     * 起こされるまで無限に待機する。
     *
     * @details
     * 呼び出し前に m を Lock しておくこと。ブロック中は内部で m を一時 Unlock し、
     * 起こされたら m を再 Lock してから返る。
     * @param m この条件変数と紐づく、呼び出し時にロック済みの FMutex。
     */
    void Wait(FMutex& m) noexcept;

    /**
     * タイムアウト付きで待機する。
     *
     * @details 呼び出し前に m を Lock しておくこと。Wait と同様に待機中は m を一時開放する。
     * @param m この条件変数と紐づく、呼び出し時にロック済みの FMutex。
     * @param timeout_ms 最大待機時間 (ミリ秒)。
     * @return 起こされたら true、タイムアウトしたら false。
     */
    bool WaitFor(FMutex& m, u32 timeout_ms) noexcept;

    /** 待機中のスレッドを 1 つ起こす。 */
    void NotifyOne() noexcept;

    /** 待機中の全スレッドを起こす。 */
    void NotifyAll() noexcept;

private:
    /** CONDITION_VARIABLE 実体。<windows.h> をヘッダで取り込まないため void* で持つ。 */
    void* m_Cv[1];
};

} // namespace acs
