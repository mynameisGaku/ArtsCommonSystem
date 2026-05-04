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

    // 値を末尾に積む (常に成功、上限なし)
    void Push(T value) noexcept {
        {
            ScopedLock lock(_mtx);
            if (_closed) return;          // closed 後は黙って捨てる
            _q.PushBack(Move(value));
        }
        _cv.NotifyOne();
    }

    // 即座に取り出し試行 (空なら false)
    bool TryPop(T& out) noexcept {
        ScopedLock lock(_mtx);
        if (_q.Size() == 0) return false;
        out = Move(_q[0]);
        // 簡易: 先頭削除は O(N) だが小規模ならコスト無視。
        // 性能必要なら ring buffer 化する。
        for (usize i = 1; i < _q.Size(); ++i) {
            _q[i - 1] = Move(_q[i]);
        }
        _q.PopBack();
        return true;
    }

    // 値が来るまで block。Close() 中に呼ばれた / 呼ばれた後なら false 返す。
    bool Pop(T& out) noexcept {
        ScopedLock lock(_mtx);
        while (_q.Size() == 0 && !_closed) {
            _cv.Wait(_mtx);  // ScopedLock 内部の mutex を unlock+wait+relock
        }
        if (_q.Size() == 0) return false;     // closed && empty
        out = Move(_q[0]);
        for (usize i = 1; i < _q.Size(); ++i) {
            _q[i - 1] = Move(_q[i]);
        }
        _q.PopBack();
        return true;
    }

    // 待機中のすべての Pop を解放 (false で抜けさせる)
    void Close() noexcept {
        {
            ScopedLock lock(_mtx);
            _closed = true;
        }
        _cv.NotifyAll();
    }

    bool IsClosed() const noexcept {
        ScopedLock lock(_mtx);
        return _closed;
    }

    usize Size() const noexcept {
        ScopedLock lock(_mtx);
        return _q.Size();
    }

private:
    mutable Mutex   _mtx;
    ConditionVar    _cv;
    Array<T>        _q;
    bool            _closed = false;
};

} // namespace acs
