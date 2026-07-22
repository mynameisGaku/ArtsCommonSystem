// SPDX-License-Identifier: Apache-2.0
// TMessagePipe<T> — スレッド間 MPMC キュー (mutex + condvar 実装)
//
// 使い方:
//   TMessagePipe<FDamageEvent> pipe;
//
//   // producer thread:
//   pipe.Push(FDamageEvent{enemy, 25.0f});
//
//   // consumer thread (毎フレーム):
//   FDamageEvent e;
//   while (pipe.TryPop(e)) {
//       ApplyDamage(e);
//   }
//
//   // または block 待機 (consumer 専用スレッドなど):
//   if (pipe.Pop(e)) { ApplyDamage(e); }
//
// 設計:
//   ・mutex + condvar の素直な実装。性能要件が出てきたら lock-free MPSC に
//     差し替える可能性あり。
//   ・FMessageBroker (同期 pub/sub) と対照: あちらは publisher が直接 handler を
//     呼び、こちらは値をキューに積んで別スレッドが取りに来る。
//   ・破棄時に block 待ちが居たら Close() してから抜ける必要がある。
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "container/Array.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#include "threading/ConditionVar.h"

namespace acs {

/**
 * スレッド間 MPMC キュー (mutex + condvar 実装)。
 *
 * @details
 * 値をキューに積み、別スレッドが取りに来る方式。FMessageBroker (同期 pub/sub) と対照的に、
 * publisher は handler を直接呼ばず値を渡すだけ。TryPop は非ブロッキング、Pop は値が来るか
 * Close されるまで block する。先頭取り出しは O(N) シフトで実装する単純な配列キュー。
 * @tparam T キューで受け渡す値型 (ムーブで出し入れする)。
 */
template<typename T>
class TMessagePipe {
public:
    /** 空のパイプを構築する。 */
    TMessagePipe() noexcept = default;

    /** Close() してから破棄する (block 待ち中の Pop を解放する)。 */
    ~TMessagePipe() noexcept { Close(); }

    /** コピー禁止 (mutex/condvar を抱えるため)。 */
    TMessagePipe(const TMessagePipe&) = delete;

    /** コピー代入も禁止。 */
    TMessagePipe& operator=(const TMessagePipe&) = delete;

    /**
     * 値を末尾に積み、待機中の consumer を 1 つ起こす。
     *
     * @param value 積む値 (ムーブで取り込む)。
     * @return 追加できたら true、Close() 済みなら false。
     */
    bool Push(T value) noexcept {
        {
            FScopedLock lock(m_Mtx);
            if (_closed) return false;
            m_Q.PushBack(Move(value));
        }
        m_Cv.NotifyOne();
        return true;
    }

    /**
     * 値を 1 つ取り出す (非ブロッキング)。
     *
     * @param out 取り出した値の書き戻し先。
     * @return 取り出せたら true、キューが空なら false。
     */
    bool TryPop(T& out) noexcept {
        FScopedLock lock(m_Mtx);
        if (m_Q.Size() == 0) return false;
        out = Move(m_Q[0]);
        // 簡易: 先頭削除は O(N) だが小規模ならコスト無視。
        // 性能必要なら ring buffer 化する。
        for (usize i = 1; i < m_Q.Size(); ++i) {
            m_Q[i - 1] = Move(m_Q[i]);
        }
        m_Q.PopBack();
        return true;
    }

    /**
     * 値が来るまで block して 1 つ取り出す。
     *
     * @details Close() 済みでキューが空のまま起こされた場合は false を返す。
     * @param out 取り出した値の書き戻し先。
     * @return 取り出せたら true、closed かつ空なら false。
     */
    bool Pop(T& out) noexcept {
        FScopedLock lock(m_Mtx);
        while (m_Q.Size() == 0 && !_closed) {
            m_Cv.Wait(m_Mtx);  // FScopedLock 内部の mutex を unlock+wait+relock
        }
        if (m_Q.Size() == 0) return false;     // closed && empty
        out = Move(m_Q[0]);
        for (usize i = 1; i < m_Q.Size(); ++i) {
            m_Q[i - 1] = Move(m_Q[i]);
        }
        m_Q.PopBack();
        return true;
    }

    /** クローズし、待機中の全 Pop を起こして false で抜けさせる。 */
    void Close() noexcept {
        {
            FScopedLock lock(m_Mtx);
            _closed = true;
        }
        m_Cv.NotifyAll();
    }

    /**
     * クローズ済みかを返す。
     *
     * @return Close() 済みなら true。
     */
    bool IsClosed() const noexcept {
        FScopedLock lock(m_Mtx);
        return _closed;
    }

    /**
     * 現在キューに溜まっている要素数を返す。
     *
     * @return キューの要素数。
     */
    usize Size() const noexcept {
        FScopedLock lock(m_Mtx);
        return m_Q.Size();
    }

private:
    /** キュー全体を保護する mutex (const メソッドからも触るため mutable)。 */
    mutable FMutex   m_Mtx;

    /** 値到着・クローズを通知する条件変数。 */
    FConditionVar    m_Cv;

    /** 値を保持する FIFO キュー。 */
    TArray<T>        m_Q;

    /** クローズ済みフラグ。 */
    bool            _closed = false;
};

} // namespace acs
