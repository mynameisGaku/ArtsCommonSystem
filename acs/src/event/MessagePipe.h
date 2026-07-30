// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "container/Array.h"
#include "event/MessagePipePolicy.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#include "threading/ConditionVar.h"

#include <type_traits>

namespace acs {

/**
 * 用途別に同期方式を選べるスレッド間 FIFO。
 *
 * @tparam T キューで受け渡す値型。
 * @tparam Policy 同期方式。既定は従来互換の MPMC。
 * @tparam Capacity SPSC の固定容量。MPMC では 0 のまま使う。
 */
template<typename T,
         EMessagePipePolicy Policy = EMessagePipePolicy::Mpmc,
         usize Capacity = 0>
class TMessagePipe;

/**
 * mutex と条件変数を使う複数 producer / consumer 対応 FIFO。
 *
 * @details
 * 読み取り位置を保持するため、1 件取り出すたびの全要素シフトは行わない。消費済み領域が
 * 十分大きくなった時だけまとめて詰め直すので、通常の Push/TryPop は償却 O(1) となる。
 * max_depth を指定すると論理要素数を上限以内に保ち、過負荷時の無制限な増加を防げる。
 */
template<typename T, usize Capacity>
class TMessagePipe<T, EMessagePipePolicy::Mpmc, Capacity> {
    static_assert(Capacity == 0, "MPMC パイプの固定容量テンプレート引数は 0 にしてください");

public:
    /**
     * 空のパイプを構築する。
     *
     * @param max_depth 0 は無制限、それ以外は保持できる論理要素数の上限。
     */
    explicit TMessagePipe(usize max_depth = 0) noexcept
        : m_MaxDepth(max_depth)
    {
    }

    /** Close() してから破棄し、待機中の Pop を解放する。 */
    ~TMessagePipe() noexcept { Close(); }

    /** コピー禁止。 */
    TMessagePipe(const TMessagePipe&) = delete;

    /** コピー代入禁止。 */
    TMessagePipe& operator=(const TMessagePipe&) = delete;

    /**
     * 値を FIFO の末尾に積む。
     *
     * @param value 追加する値。
     * @return 追加できた場合は true。Close 済み、上限到達、確保失敗なら false。
     */
    bool Push(T value) noexcept {
        /** 待機中 consumer へ通知するか。 */
        bool notify = false;
        {
            /** キュー操作中に保持するロック。 */
            FScopedLock lock(m_Mtx);
            if (m_Closed || IsFullLocked()) return false;
            CompactLocked(false);
            if (!m_Queue.TryPushBack(Move(value))) return false;
            notify = m_WaiterCount != 0;
        }
        if (notify) m_Cv.NotifyOne();
        return true;
    }

    /**
     * 複数の値を 1 回の lock 取得で FIFO へ積む。
     *
     * @details values の各要素は追加できた分だけムーブされる。
     * @param values 入力配列。count が 0 でなければ非 null が必要。
     * @param count 入力要素数。
     * @return 実際に追加した要素数。
     */
    usize PushBatch(T* values, usize count) noexcept {
        if (!values && count != 0) return 0;
        /** 実際に追加した要素数。 */
        usize pushed = 0;
        /** ロック取得時点の待機 consumer 数。 */
        usize waiters = 0;
        {
            /** バッチ追加中に保持するロック。 */
            FScopedLock lock(m_Mtx);
            if (m_Closed) return 0;
            CompactLocked(false);
            while (pushed < count && !IsFullLocked()) {
                if (!m_Queue.TryPushBack(Move(values[pushed]))) break;
                ++pushed;
            }
            waiters = m_WaiterCount;
        }
        if (pushed != 0 && waiters != 0) {
            if (pushed == 1 || waiters == 1) m_Cv.NotifyOne();
            else m_Cv.NotifyAll();
        }
        return pushed;
    }

    /**
     * 値を 1 つ取り出す。
     *
     * @param out 取り出した値の格納先。
     * @return 取り出せた場合は true、空なら false。
     */
    bool TryPop(T& out) noexcept {
        /** キュー操作中に保持するロック。 */
        FScopedLock lock(m_Mtx);
        return PopOneLocked(out);
    }

    /**
     * 最大 capacity 件を 1 回の lock 取得で取り出す。
     *
     * @param out 出力配列。capacity が 0 でなければ非 null が必要。
     * @param capacity 出力できる最大要素数。
     * @return 実際に取り出した要素数。
     */
    usize TryPopBatch(T* out, usize capacity) noexcept {
        if (!out && capacity != 0) return 0;
        /** バッチ取り出し中に保持するロック。 */
        FScopedLock lock(m_Mtx);
        /** 実際に取り出した要素数。 */
        usize count = 0;
        while (count < capacity && LogicalSizeLocked() != 0) {
            out[count++] = Move(m_Queue[m_Head++]);
        }
        CompactLocked(true);
        return count;
    }

    /**
     * 値が届くか Close されるまで待って 1 件取り出す。
     *
     * @param out 取り出した値の格納先。
     * @return 値を取り出せた場合は true。closed かつ空なら false。
     */
    bool Pop(T& out) noexcept {
        /** 待機と取り出し中に保持するロック。 */
        FScopedLock lock(m_Mtx);
        while (LogicalSizeLocked() == 0 && !m_Closed) {
            ++m_WaiterCount;
            m_Cv.Wait(m_Mtx);
            --m_WaiterCount;
        }
        return PopOneLocked(out);
    }

    /** クローズし、待機中の全 Pop を起こす。繰り返し呼んでも安全。 */
    void Close() noexcept {
        /** 待機中 consumer へ通知するか。 */
        bool notify = false;
        {
            /** close 状態更新中に保持するロック。 */
            FScopedLock lock(m_Mtx);
            if (!m_Closed) {
                m_Closed = true;
                notify = m_WaiterCount != 0;
            }
        }
        if (notify) m_Cv.NotifyAll();
    }

    /** クローズ済みなら true を返す。 */
    bool IsClosed() const noexcept {
        /** close 状態参照中に保持するロック。 */
        FScopedLock lock(m_Mtx);
        return m_Closed;
    }

    /** 現在保持している論理要素数を返す。 */
    usize Size() const noexcept {
        /** 要素数参照中に保持するロック。 */
        FScopedLock lock(m_Mtx);
        return LogicalSizeLocked();
    }

    /** 設定された最大要素数を返す。0 は無制限。 */
    usize MaxDepth() const noexcept { return m_MaxDepth; }

private:
    /** lock 保持中の論理要素数を返す。 */
    usize LogicalSizeLocked() const noexcept {
        return m_Queue.Size() - m_Head;
    }

    /** lock 保持中に上限へ達しているかを返す。 */
    bool IsFullLocked() const noexcept {
        return m_MaxDepth != 0 && LogicalSizeLocked() >= m_MaxDepth;
    }

    /** lock 保持中に 1 件取り出す。 */
    bool PopOneLocked(T& out) noexcept {
        if (LogicalSizeLocked() == 0) return false;
        out = Move(m_Queue[m_Head++]);
        CompactLocked(true);
        return true;
    }

    /**
     * 消費済み先頭領域をまとめて除去する。
     *
     * @param allow_empty_clear true なら空になった配列を即座に論理クリアする。
     */
    void CompactLocked(bool allow_empty_clear) noexcept {
        /** 未消費の論理要素数。 */
        const usize live = LogicalSizeLocked();
        if (live == 0) {
            if (allow_empty_clear || m_Head >= 64) {
                m_Queue.Clear();
                m_Head = 0;
            }
            return;
        }
        if (m_Head < 64 || m_Head < live) return;
        /** 未消費要素を詰め直す位置。 */
        for (usize i = 0; i < live; ++i) {
            m_Queue[i] = Move(m_Queue[m_Head + i]);
        }
        while (m_Queue.Size() > live) m_Queue.PopBack();
        m_Head = 0;
    }

    /** キュー全体を保護する mutex。 */
    mutable FMutex m_Mtx;

    /** 値到着と Close を通知する条件変数。 */
    FConditionVar m_Cv;

    /** 消費済み先頭領域を含む物理配列。 */
    TArray<T> m_Queue;

    /** 次に読み出す物理配列 index。 */
    usize m_Head = 0;

    /** 0 なら無制限の論理要素数上限。 */
    usize m_MaxDepth = 0;

    /** 条件変数で待機中の consumer 数。 */
    usize m_WaiterCount = 0;

    /** クローズ済みフラグ。 */
    bool m_Closed = false;
};

/**
 * 1 producer / 1 consumer 専用の固定容量 lock-free FIFO。
 *
 * @details producer は Push 系、consumer は TryPop 系だけを呼ぶ。head と tail は単調増加し、
 * 配列 index だけをビットマスクで折り返す。Close は最後の Push 完了後に呼び、
 * Push と並行実行しない。consumer は Close 観測後も残件を空になるまで取り出せる。
 * 型と容量の誤用、例外を送出し得る値操作はコンパイル時に拒否する。
 */
template<typename T, usize Capacity>
class TMessagePipe<T, EMessagePipePolicy::Spsc, Capacity> {
    static_assert(kIsValidMessagePipeCapacity<Capacity>, "SPSC パイプ容量は 2 以上の 2 の累乗である必要があります");
    static_assert(std::is_nothrow_default_constructible_v<T>, "SPSC パイプの値型は noexcept 既定構築可能である必要があります");
    static_assert(std::is_nothrow_move_constructible_v<T>, "SPSC パイプの値型は noexcept ムーブ構築可能である必要があります");
    static_assert(std::is_nothrow_move_assignable_v<T>, "SPSC パイプの値型は noexcept ムーブ代入可能である必要があります");
    static_assert(std::is_nothrow_destructible_v<T>, "SPSC パイプの値型は noexcept 破棄可能である必要があります");

public:
    /** 空の固定容量パイプを構築する。 */
    TMessagePipe() noexcept = default;

    /** クローズして破棄する。 */
    ~TMessagePipe() noexcept { Close(); }

    /** コピー禁止。 */
    TMessagePipe(const TMessagePipe&) = delete;

    /** コピー代入禁止。 */
    TMessagePipe& operator=(const TMessagePipe&) = delete;

    /**
     * producer スレッドから値を 1 件追加する。
     *
     * @param value 追加する値。
     * @return 追加できた場合は true。満杯または Close 済みなら false。
     */
    bool Push(T value) noexcept {
        if (m_Closed.Load(EMemoryOrder::Acquire) != 0) return false;
        /** producer が次に書き込む単調増加位置。 */
        const usize tail = m_Tail.Load(EMemoryOrder::Relaxed);
        /** consumer が次に読み取る単調増加位置。 */
        const usize head = m_Head.Load(EMemoryOrder::Acquire);
        if (tail - head >= Capacity) return false;
        m_Buffer[tail & (Capacity - 1)] = Move(value);
        m_Tail.Store(tail + 1, EMemoryOrder::Release);
        return true;
    }

    /**
     * producer スレッドから複数件を追加する。
     *
     * @param values 追加する値配列。
     * @param count 入力要素数。
     * @return 実際に追加した要素数。
     */
    usize PushBatch(T* values, usize count) noexcept {
        if (!values && count != 0) return 0;
        /** 実際に追加した要素数。 */
        usize pushed = 0;
        while (pushed < count && Push(Move(values[pushed]))) ++pushed;
        return pushed;
    }

    /**
     * consumer スレッドから値を 1 件取り出す。
     *
     * @param out 取り出した値の格納先。
     * @return 取り出せた場合は true、空なら false。
     */
    bool TryPop(T& out) noexcept {
        /** consumer が次に読み取る単調増加位置。 */
        const usize head = m_Head.Load(EMemoryOrder::Relaxed);
        /** producer が次に書き込む単調増加位置。 */
        const usize tail = m_Tail.Load(EMemoryOrder::Acquire);
        if (head == tail) return false;
        out = Move(m_Buffer[head & (Capacity - 1)]);
        m_Head.Store(head + 1, EMemoryOrder::Release);
        return true;
    }

    /**
     * consumer スレッドから最大 capacity 件を取り出す。
     *
     * @param out 取り出した値の格納先配列。
     * @param capacity 出力できる最大要素数。
     * @return 実際に取り出した要素数。
     */
    usize TryPopBatch(T* out, usize capacity) noexcept {
        if (!out && capacity != 0) return 0;
        /** 実際に取り出した要素数。 */
        usize popped = 0;
        while (popped < capacity && TryPop(out[popped])) ++popped;
        return popped;
    }

    /**
     * producer 側を閉じ、以後の Push を拒否する。
     *
     * @details 最後の Push が完了してから呼ぶ。Push と Close の
     * 並行実行は契約外。既に積まれた値は consumer が引き続き取り出せる。
     */
    void Close() noexcept {
        m_Closed.Store(1, EMemoryOrder::Release);
    }

    /** クローズ済みなら true を返す。 */
    bool IsClosed() const noexcept {
        return m_Closed.Load(EMemoryOrder::Acquire) != 0;
    }

    /** 現在保持している要素数を返す。 */
    usize Size() const noexcept {
        /** consumer が次に読み取る単調増加位置。 */
        const usize head = m_Head.Load(EMemoryOrder::Acquire);
        /** producer が次に書き込む単調増加位置。 */
        const usize tail = m_Tail.Load(EMemoryOrder::Acquire);
        return tail - head;
    }

    /** 固定容量を返す。 */
    static constexpr usize MaxDepth() noexcept { return Capacity; }

private:
    /** 固定容量の値領域。producer が書き consumer が読む。 */
    T m_Buffer[Capacity]{};

    /** consumer だけが更新する単調増加 read index。 */
    alignas(64) TAtomic<usize> m_Head{0};

    /** producer だけが更新する単調増加 write index。 */
    alignas(64) TAtomic<usize> m_Tail{0};

    /** 以後の Push を拒否するフラグ。 */
    TAtomic<u32> m_Closed{0};
};

} // namespace acs
