// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "container/Array.h"
#include "threading/ThreadAffinity.h"

namespace acs {

/**
 * TObservable の購読を識別するハンドル。
 *
 * @details
 * Subscribe が返し Unsubscribe に渡す。id と generation の組で、
 * スロット再利用後の古いハンドルによる誤った解除を防ぐ。
 */
struct FObservableHandle {
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
    bool operator==(const FObservableHandle& o) const noexcept {
        return id == o.id && generation == o.generation;
    }
};

/** 無効を表す既定構築の購読ハンドル。 */
inline constexpr FObservableHandle kInvalidObservable{};

namespace mvvm_detail {

/**
 * callback 実行中の owner 寿命を stack 上だけで監視する侵入型 guard。
 *
 * @details
 * owner は guard の先頭ポインタだけをメンバに持ち、callback 冒頭で本 guard を積む。
 * owner の destructor は InvalidateChain を最初に呼び、実行中 guard を dead にする。
 * callback は外部呼出しから復帰した直後に IsAlive だけを読み、dead なら owner へ
 * 二度と触れず return する。同じ storage に owner が placement-new されても、
 * old guard は head への参照を破棄済みなので新実体の chain を変更しない。
 * heap allocation は行わない。
 */
class FCallbackLifetimeGuard {
public:
    /**
     * owner が持つ guard chain の先頭へ自身を積む。
     *
     * @param head owner 内の guard chain 先頭。
     */
    explicit FCallbackLifetimeGuard(FCallbackLifetimeGuard*& head) noexcept
        : m_Head(&head),
          m_Previous(head) {
        head = this;
    }

    /** 生存中なら owner の guard chain から自身を外す。 */
    ~FCallbackLifetimeGuard() noexcept {
        if (!m_Alive) return;
        *m_Head = m_Previous;
    }

    FCallbackLifetimeGuard(const FCallbackLifetimeGuard&) = delete;
    FCallbackLifetimeGuard& operator=(const FCallbackLifetimeGuard&) = delete;
    FCallbackLifetimeGuard(FCallbackLifetimeGuard&&) = delete;
    FCallbackLifetimeGuard& operator=(FCallbackLifetimeGuard&&) = delete;

    /**
     * callback owner がまだ同じ寿命で生存しているか返す。
     *
     * @return owner destructor が chain を無効化していなければ true。
     */
    bool IsAlive() const noexcept { return m_Alive; }

    /**
     * owner 上で実行中の guard をすべて dead にし、chain を空にする。
     *
     * @details owner destructor の最初に呼ぶ。同じ storage の新 owner が構築する
     * chain と old stack frame を寿命分離するため、各 guard の head 参照も null にする。
     * @param head owner 内の guard chain 先頭。
     */
    static void InvalidateChain(FCallbackLifetimeGuard*& head) noexcept {
        FCallbackLifetimeGuard* guard = head;
        while (guard) {
            guard->m_Alive = false;
            guard->m_Head = nullptr;
            guard = guard->m_Previous;
        }
        head = nullptr;
    }

private:
    /** owner が保持する chain 先頭への参照。owner 破棄時は null にする。 */
    FCallbackLifetimeGuard** m_Head = nullptr;

    /** 1 つ外側の callback guard。 */
    FCallbackLifetimeGuard* m_Previous = nullptr;

    /** owner が同じ寿命で生存している間だけ true。 */
    bool m_Alive = true;
};

} // namespace mvvm_detail

/**
 * 値の変更を監視できる単一値ホルダ (MVVM の中核)。
 *
 * @details
 * Set で値を書き換えると、前回と異なる場合のみ登録済みリスナへ通知が走る
 * (T::operator== で同値判定)。リスナは関数ポインタ + void* user の形式で保持する。
 * Subscribe/Unsubscribe は通知中でも安全で、通知中の解除はスロットを inactive 化し
 * 通知ループ終了後に遅延回収する。listener が所有 object ごとこの TObservable を同期破棄
 * した場合は通知を即時停止し、残りの listener を呼ばない。UI スレッド専用
 * (スレッドセーフではない)。owner を破棄する listener は、その破棄を callback の最後の
 * 操作として行い、以後 new_value、user、owner のいずれも再参照せず直ちに return すること。
 *
 * @tparam T 保持する値の型 (operator== を持つこと)。
 */
template<typename T>
class TObservable {
public:
    /**
     * 値変更時に呼ばれるリスナ型 (新値と Subscribe 時の user を受け取る)。
     *
     * @details callback が owner を同期破棄する場合、その破棄は最後の操作にする。破棄後は
     * new_value 参照、user、owner を一切再参照せず、直ちに callback から return する。
     */
    using Listener = void (*)(const T& new_value, void* user);

    /** 値を T{} で初期化して構築する。 */
    TObservable() noexcept = default;

    /**
     * 初期値を指定して構築する。
     *
     * @param initial 初期値 (ムーブで取り込む)。
     */
    explicit TObservable(T initial) noexcept : m_Value(Move(initial)) {}

    /** コピー禁止 (購読スロットを単独所有するため)。 */
    TObservable(const TObservable&) = delete;

    /** コピー代入も禁止。 */
    TObservable& operator=(const TObservable&) = delete;

    /**
     * 実行中の Notify guard をすべて dead にしてからメンバを破棄する。
     *
     * @details UI thread 専用。listener がこの TObservable を所有 object ごと同期破棄しても、
     * 復帰先 Notify は stack guard だけを確認して安全に終了できる。
     */
    ~TObservable() noexcept {
        mvvm_detail::FCallbackLifetimeGuard::InvalidateChain(
            m_NotifyLifetimeGuards);
    }

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
     * cb が owner を破棄する場合は破棄を最後に行い、引数・user・owner を再参照せず return する。
     * @param cb 値変更時に呼ぶリスナ (null なら登録しない)。
     * @param user リスナへそのまま渡される任意ポインタ。
     * @return 登録した購読を指すハンドル (cb が null なら kInvalidObservable)。
     */
    FObservableHandle Subscribe(Listener cb, void* user) noexcept {
        ACS_THREAD_AFFINITY_CHECK();
        if (!cb) return kInvalidObservable;
        u32 idx;
        if (m_FreeIndices.Num() > 0) {
            idx = m_FreeIndices[m_FreeIndices.Num() - 1];
            m_FreeIndices.Pop();
        } else {
            idx = static_cast<u32>(m_Slots.Num());
            m_Slots.Add(FSlot{});
        }
        FSlot& s = m_Slots[idx];
        if (s.id == 0) s.id = m_NextId++;
        s.generation++;
        s.active = true;
        s.cb     = cb;
        s.user   = user;
        return FObservableHandle{ s.id, s.generation };
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
    bool Unsubscribe(FObservableHandle h) noexcept {
        if (!h.IsValid()) return false;
        for (usize i = 0; i < m_Slots.Num(); ++i) {
            FSlot& s = m_Slots[i];
            if (s.id == h.id && s.generation == h.generation && s.active) {
                if (m_NotifyDepth > 0) {
                    s.active = false;
                    m_PendingCancel.Add(static_cast<u32>(i));
                } else {
                    s.active = false;
                    s.cb     = nullptr;
                    s.user   = nullptr;
                    m_FreeIndices.Add(static_cast<u32>(i));
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
        for (usize i = 0; i < m_Slots.Num(); ++i) if (m_Slots[i].active) ++n;
        return n;
    }

private:
    /**
     * 全アクティブリスナへ現在値を通知する。
     *
     * @details
     * m_NotifyDepth で再入を数え、通知ループ中の Unsubscribe で積まれた
     * m_PendingCancel を最外ループ終了時にまとめて回収する。listener がこの TObservable を
     * 同期破棄した場合は stack guard だけを確認して即時 return し、破棄済み member に触れない。
     */
    void Notify() noexcept {
        mvvm_detail::FCallbackLifetimeGuard lifetime_guard(
            m_NotifyLifetimeGuards);

        ++m_NotifyDepth;
        const usize n = m_Slots.Num();
        for (usize i = 0; i < n; ++i) {
            FSlot& s = m_Slots[i];
            if (!s.active || !s.cb) continue;
            s.cb(m_Value, s.user);
            if (!lifetime_guard.IsAlive()) return;
        }
        --m_NotifyDepth;
        if (m_NotifyDepth == 0 && m_PendingCancel.Num() > 0) {
            for (usize i = 0; i < m_PendingCancel.Num(); ++i) {
                const u32 idx = m_PendingCancel[i];
                if (idx < m_Slots.Num()) {
                    FSlot& s = m_Slots[idx];
                    s.cb   = nullptr;
                    s.user = nullptr;
                    m_FreeIndices.Add(idx);
                }
            }
            m_PendingCancel.Reset();
        }
    }

    /** 1 リスナ分の購読スロット。 */
    struct FSlot {
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
    TArray<FSlot>  m_Slots;

    /** 空きスロットのインデックス (再利用候補)。 */
    TArray<u32>   m_FreeIndices;

    /** 通知中に解除されたスロットの遅延回収待ちインデックス。 */
    TArray<u32>   m_PendingCancel;

    /** 次に割り当てるスロット ID (1 から増加)。 */
    u32          m_NextId      = 1;

    /** 通知の再入深度 (>0 の間は解除を遅延する)。 */
    i32          m_NotifyDepth = 0;

    /** 現在この TObservable 上で実行中の Notify stack guard 先頭。 */
    mvvm_detail::FCallbackLifetimeGuard* m_NotifyLifetimeGuards = nullptr;

    /** スレッドアフィニティ検証用フィールド (UI スレッド外アクセスを Debug で検出)。 */
    ACS_THREAD_AFFINITY_FIELD();
};

} // namespace acs
