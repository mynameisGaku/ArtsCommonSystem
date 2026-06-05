// SPDX-License-Identifier: Apache-2.0
// Observable<T> — 監視可能な値 (MVVM の中核)
//
// 使い方:
//   Observable<f32> hp { 100.0f };
//
//   // 監視
//   struct Ctx { ... };
//   Ctx c;
//   auto h = hp.Subscribe([](const f32& v, void* user){
//       // (関数ポインタ用法。lambda は捕獲なしのもの限定)
//       ACS_LOG_INFO("HP changed: %.1f", v);
//   }, &c);
//
//   // 値変更 → 全監視者に通知
//   hp.Set(75.0f);
//
//   // 監視解除
//   hp.Unsubscribe(h);
//
// 設計:
//   ・コールバックは関数ポインタ + void* user (STL 不依存方針)
//   ・Set 時のみ通知、同値設定では発火しない (T::operator==)
//   ・Subscribe / Unsubscribe は通知中でも安全 (遅延キャンセル)
//   ・スレッドセーフではない (UI スレッドのみ前提、他スレッドからは MessagePipe 経由)
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "container/Array.h"
#include "threading/ThreadAffinity.h"

namespace acs {

/**
 * Observable の購読を識別するハンドル。
 *
 * @details
 * Subscribe が返し Unsubscribe に渡す。id と generation の組で、
 * スロット再利用後の古いハンドルによる誤った解除を防ぐ。
 */
struct ObservableHandle {
    /** スロットの 1-based インデックス (1..N、0 は無効)。 */
    u32 id         = 0;

    /** スロット世代。再利用のたびに増え、wrap 起因の誤判定を実質ゼロにするため 64-bit。 */
    u64 generation = 0;

    /**
     * ハンドルが有効かを返す。
     *
     * @return id が 0 でなければ true。
     */
    bool IsValid() const noexcept { return id != 0; }

    /**
     * 2 つのハンドルが同一購読を指すかを比較する。
     *
     * @param o 比較相手のハンドル。
     * @return id と generation が一致すれば true。
     */
    bool operator==(const ObservableHandle& o) const noexcept {
        return id == o.id && generation == o.generation;
    }
};

/** 無効を表す既定構築の購読ハンドル。 */
inline constexpr ObservableHandle kInvalidObservable{};

/**
 * 値の変更を監視できる単一値ホルダ (MVVM の中核)。
 *
 * @details
 * Set で値を書き換えると、前回と異なる場合のみ登録済みリスナへ通知が走る
 * (T::operator== で同値判定)。リスナは関数ポインタ + void* user の形式 (STL 非依存)。
 * Subscribe/Unsubscribe は通知中でも安全で、通知中の解除はスロットを inactive 化し
 * 通知ループ終了後に遅延回収する。UI スレッド専用 (スレッドセーフではない)。
 *
 * @tparam T 保持する値の型 (operator== を持つこと)。
 */
template<typename T>
class Observable {
public:
    /** 値変更時に呼ばれるリスナ型 (新値と Subscribe 時の user を受け取る)。 */
    using Listener = void (*)(const T& new_value, void* user);

    /** 値を T{} で初期化して構築する。 */
    Observable() noexcept = default;

    /**
     * 初期値を指定して構築する。
     *
     * @param initial 初期値 (ムーブで取り込む)。
     */
    explicit Observable(T initial) noexcept : m_Value(Move(initial)) {}

    /** コピー禁止 (購読スロットを単独所有するため)。 */
    Observable(const Observable&) = delete;

    /** コピー代入も禁止。 */
    Observable& operator=(const Observable&) = delete;

    /**
     * 現在値への const 参照を返す。
     *
     * @return 保持している値への const 参照。
     */
    const T& Get() const noexcept { return m_Value; }

    /**
     * 現在値への const 参照に暗黙変換する。
     *
     * @return 保持している値への const 参照。
     */
    operator const T&() const noexcept { return m_Value; }

    /**
     * 値を設定し、前回と異なる場合のみ全リスナへ通知する。
     *
     * @param v 設定する新しい値 (const 参照、コピーで取り込む)。
     */
    void Set(const T& v) noexcept {
        ACS_THREAD_AFFINITY_CHECK();
        if (m_Value == v) return;
        m_Value = v;
        Notify();
    }

    /**
     * 値を設定し、前回と異なる場合のみ全リスナへ通知する (ムーブ版)。
     *
     * @param v 設定する新しい値 (ムーブで取り込む)。
     */
    void Set(T&& v) noexcept {
        ACS_THREAD_AFFINITY_CHECK();
        if (m_Value == v) return;
        m_Value = Move(v);
        Notify();
    }

    /**
     * 値が同じでも強制的に全リスナへ通知する。
     *
     * @details 内部コンテナ等を Get() 経由で in-place に書き換えた後に使う。
     */
    void ForceNotify() noexcept { Notify(); }

    /**
     * リスナを登録する。
     *
     * @details 空きスロットを再利用し、無ければ追加する。cb が null なら無効ハンドルを返す。
     * @param cb 値変更時に呼ぶリスナ (null なら登録しない)。
     * @param user リスナへそのまま渡される任意ポインタ。
     * @return 登録した購読を指すハンドル (cb が null なら kInvalidObservable)。
     */
    ObservableHandle Subscribe(Listener cb, void* user) noexcept {
        ACS_THREAD_AFFINITY_CHECK();
        if (!cb) return kInvalidObservable;
        u32 idx;
        if (m_FreeIndices.Size() > 0) {
            idx = m_FreeIndices[m_FreeIndices.Size() - 1];
            m_FreeIndices.PopBack();
        } else {
            idx = static_cast<u32>(m_Slots.Size());
            m_Slots.PushBack(Slot{});
        }
        Slot& s = m_Slots[idx];
        if (s.id == 0) s.id = m_NextId++;
        s.generation++;
        s.active = true;
        s.cb     = cb;
        s.user   = user;
        return ObservableHandle{ s.id, s.generation };
    }

    /**
     * リスナを解除する。
     *
     * @details
     * 通知中 (m_NotifyDepth>0) ならスロットを inactive にして m_PendingCancel へ積み、
     * 通知ループ終了後に回収する。通知外なら即座にスロットを空きにして再利用へ回す。
     * @param h Subscribe が返したハンドル。
     * @return 該当購読を解除できたら true、無効・未登録なら false。
     */
    bool Unsubscribe(ObservableHandle h) noexcept {
        if (!h.IsValid()) return false;
        for (usize i = 0; i < m_Slots.Size(); ++i) {
            Slot& s = m_Slots[i];
            if (s.id == h.id && s.generation == h.generation && s.active) {
                if (m_NotifyDepth > 0) {
                    s.active = false;
                    m_PendingCancel.PushBack(static_cast<u32>(i));
                } else {
                    s.active = false;
                    s.cb     = nullptr;
                    s.user   = nullptr;
                    m_FreeIndices.PushBack(static_cast<u32>(i));
                }
                return true;
            }
        }
        return false;
    }

    /**
     * 現在アクティブなリスナ数を返す。
     *
     * @return active なスロット数。
     */
    u32 SubscriberCount() const noexcept {
        u32 n = 0;
        for (usize i = 0; i < m_Slots.Size(); ++i) if (m_Slots[i].active) ++n;
        return n;
    }

private:
    /**
     * 全アクティブリスナへ現在値を通知する。
     *
     * @details
     * m_NotifyDepth で再入を数え、通知ループ中の Unsubscribe で積まれた
     * m_PendingCancel を最外ループ終了時にまとめて回収する。
     */
    void Notify() noexcept {
        ++m_NotifyDepth;
        const usize n = m_Slots.Size();
        for (usize i = 0; i < n; ++i) {
            Slot& s = m_Slots[i];
            if (!s.active || !s.cb) continue;
            s.cb(m_Value, s.user);
        }
        --m_NotifyDepth;
        if (m_NotifyDepth == 0 && m_PendingCancel.Size() > 0) {
            for (usize i = 0; i < m_PendingCancel.Size(); ++i) {
                const u32 idx = m_PendingCancel[i];
                if (idx < m_Slots.Size()) {
                    Slot& s = m_Slots[idx];
                    s.cb   = nullptr;
                    s.user = nullptr;
                    m_FreeIndices.PushBack(idx);
                }
            }
            m_PendingCancel.Clear();
        }
    }

    /** 1 リスナ分の購読スロット。 */
    struct Slot {
        /** スロット ID (割り当て後は固定、ハンドル照合に使う)。 */
        u32      id          = 0;

        /** 世代カウンタ (再利用のたびに増やしハンドルを陳腐化させる)。 */
        u64      generation  = 0;

        /** このスロットが現在有効かを示す。 */
        bool     active      = false;

        /** 通知時に呼ぶリスナ関数。 */
        Listener cb          = nullptr;

        /** リスナへ渡す user ポインタ。 */
        void*    user        = nullptr;
    };

    /** 監視対象の現在値。 */
    T            m_Value{};

    /** 購読スロット配列 (空き分は再利用される)。 */
    TArray<Slot>  m_Slots;

    /** 空きスロットのインデックス (再利用候補)。 */
    TArray<u32>   m_FreeIndices;

    /** 通知中に解除されたスロットの遅延回収待ちインデックス。 */
    TArray<u32>   m_PendingCancel;

    /** 次に割り当てるスロット ID (1 から増加)。 */
    u32          m_NextId      = 1;

    /** 通知の再入深度 (>0 の間は解除を遅延する)。 */
    i32          m_NotifyDepth = 0;

    /** スレッドアフィニティ検証用フィールド (UI スレッド外アクセスを Debug で検出)。 */
    ACS_THREAD_AFFINITY_FIELD();
};

} // namespace acs
