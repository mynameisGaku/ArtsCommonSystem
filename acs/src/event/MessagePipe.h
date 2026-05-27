// SPDX-License-Identifier: Apache-2.0
// MessagePipe<T> — スレッド間 MPMC キュー (mutex + condvar 実装)
//
// 使い方:
//   MessagePipe<DamageEvent> pipe;
//
//   // producer thread:
//   pipe.Push(DamageEvent{enemy, 25.0f});
//
//   // consumer thread (毎フレーム):
//   DamageEvent e;
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
//   ・MessageBroker (同期 pub/sub) と対照: あちらは publisher が直接 handler を
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

template<typename T>
class MessagePipe {
public:
    MessagePipe() noexcept = default;
    ~MessagePipe() noexcept { Close(); }

    MessagePipe(const MessagePipe&) = delete;
    MessagePipe& operator=(const MessagePipe&) = delete;

    // 値を末尾に積む。Close() 済みなら false、追加成功なら true。
    bool Push(T value) noexcept {
        {
            FScopedLock lock(m_Mtx);
            if (_closed) return false;
            m_Q.PushBack(Move(value));
        }
        m_Cv.NotifyOne();
        return true;
    }

    // 即座に取り出し試行 (空なら false)
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

    // 値が来るまで block。Close() 中に呼ばれた / 呼ばれた後なら false 返す。
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

    // 待機中のすべての Pop を解放 (false で抜けさせる)
    void Close() noexcept {
        {
            FScopedLock lock(m_Mtx);
            _closed = true;
        }
        m_Cv.NotifyAll();
    }

    bool IsClosed() const noexcept {
        FScopedLock lock(m_Mtx);
        return _closed;
    }

    usize Size() const noexcept {
        FScopedLock lock(m_Mtx);
        return m_Q.Size();
    }

private:
    mutable FMutex   m_Mtx;
    ConditionVar    m_Cv;
    TArray<T>        m_Q;
    bool            _closed = false;
};

} // namespace acs
