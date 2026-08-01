// SPDX-License-Identifier: Apache-2.0
// TObservableArray<T> — 要素の追加・削除・変更を通知する配列
//
// 使い方:
//   TObservableArray<int> inv;
//
//   inv.Subscribe([](EArrayChange kind, usize idx, const int* v, void*){
//       switch (kind) {
//           case EArrayChange::Inserted: ACS_LOG_INFO("Add[%zu] = %d", idx, *v); break;
//           case EArrayChange::Removed:  ACS_LOG_INFO("Remove[%zu]", idx);       break;
//           case EArrayChange::Changed:  ACS_LOG_INFO("Set[%zu] = %d", idx, *v); break;
//           case EArrayChange::Cleared:  ACS_LOG_INFO("Cleared all");            break;
//       }
//   }, nullptr);
//
//   inv.PushBack(7);     // → Inserted, idx=0, v=7
//   inv.SetAt(0, 99);    // → Changed,  idx=0, v=99
//   inv.RemoveAt(0);     // → Removed,  idx=0
//   inv.Clear();         // → Cleared
//
// 設計:
//   ・通知は 1 種類の callback で kind を分岐 (UE5 の per-event fan-out より易い)
//   ・listener 中は PushBack/PopBack/Remove/RemoveAt/SetAt/Clear による全変更を禁止する
//     (Debug は assert 検出。assert 無効の Release では検出されず処理が実行されるため、
//      呼び出した時点で caller の契約違反)
//     → どうしても要るなら listener が処理キューに積んで後で実行
//   ・要素の T は同値判定 (operator==) を持つこと推奨 (Set で同値スキップ)
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "foundation/Assert.h"
#include "container/Array.h"

namespace acs {

namespace mvvm_test {
struct FObservableArrayLifetimeTestAccess;
} // namespace mvvm_test

/** 配列の変更内容を表す通知種別。 */
enum class EArrayChange : u8 {
    /** index に value を挿入した。 */
    Inserted,

    /** index にあった要素を削除した (削除済みのため通知時の value は nullptr)。 */
    Removed,

    /** index の要素を別の値に書き換えた (value = 新値)。 */
    Changed,

    /** 全要素を削除した (index=0, value=nullptr)。 */
    Cleared,
};

/**
 * TObservableArray の購読を識別するハンドル (TObservable<T> と同仕様)。
 *
 * @details id と generation の組でスロット再利用後の古いハンドルを無効化する。
 */
struct FArrayObserverHandle {
    /** スロットの 1-based インデックス (0 は無効)。 */
    u32 id         = 0;

    /** スロット世代 (再利用のたびに増加)。 */
    u32 generation = 0;

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
    bool operator==(const FArrayObserverHandle& o) const noexcept {
        return id == o.id && generation == o.generation;
    }
};

/** 無効を表す既定構築の配列購読ハンドル。 */
inline constexpr FArrayObserverHandle kInvalidArrayObserver{};

/**
 * 要素の挿入・削除・変更・全消去を通知する観測可能配列。
 *
 * @details
 * 各変更操作は単一のリスナを EArrayChange の種別付きで呼ぶ (per-event の fan-out はしない)。
 * 通知中 (Notify 内) は PushBack/PopBack/Remove/RemoveAt/SetAt/Clear による全変更を禁止する。
 * Debug ビルドでは assert で検出する。assert を無効化した Release ビルドでは検出されず
 * 変更処理が実行されるため、呼び出した時点で caller の契約違反となる。
 * SetAt は通知外でも operator== で同値ならスキップする。
 * リスナが配列自身または配列を所有する object を破棄した場合、実行中の通知を直ちに打ち切り、
 * 解放済みの購読スロットへ戻らない。破棄を行ったリスナ自身も、value / user / owner が
 * 以後無効になり得るため、破棄操作を最後の処理として直ちに return しなければならない。
 *
 * @tparam T 要素型 (operator== を持つこと推奨)。
 */
template<typename T>
class TObservableArray {
public:
    /** 変更通知リスナ型 (種別・index・該当値ポインタ・user を受け取る)。 */
    using Listener = void (*)(EArrayChange kind, usize index, const T* value, void* user);

    /** 空配列を構築する。 */
    TObservableArray() noexcept = default;

    /**
     * 配列を破棄する。
     *
     * @details 通知 callback から自身が破棄された場合は、stack 上の全 Notify frame を
     * dead にして、callback 復帰後の通知処理が this を再参照しないようにする。
     */
    ~TObservableArray() noexcept {
        InvalidateNotifyFrames();
    }

    /** コピー禁止 (購読スロットを単独所有するため)。 */
    TObservableArray(const TObservableArray&)            = delete;

    /** コピー代入も禁止。 */
    TObservableArray& operator=(const TObservableArray&) = delete;

    /**
     * 末尾に要素を追加し Inserted を通知する。
     *
     * @param v 追加する要素 (ムーブで取り込む)。
     */
    void PushBack(T v) noexcept {
        AssertMutationOK();
        const usize idx = m_Items.Size();
        m_Items.PushBack(Move(v));
        Notify(EArrayChange::Inserted, idx, &m_Items[idx]);
    }

    /**
     * 末尾要素を削除し Removed を通知する (空なら何もしない)。
     */
    void PopBack() noexcept {
        if (m_Items.Size() == 0) return;
        AssertMutationOK();
        const usize idx = m_Items.Size() - 1;
        // PopBack 後は要素アクセスできないので value=nullptr で通知
        m_Items.PopBack();
        Notify(EArrayChange::Removed, idx, nullptr);
    }

    /**
     * 指定 index の要素を削除し Removed を通知する。
     *
     * @details 末尾入れ替えではなく後続要素を前詰めして順序を保持する。範囲外なら何もしない。
     * @param index 削除する要素の位置。
     */
    void RemoveAt(usize index) noexcept {
        if (index >= m_Items.Size()) return;
        AssertMutationOK();
        for (usize i = index + 1; i < m_Items.Size(); ++i) {
            m_Items[i - 1] = Move(m_Items[i]);
        }
        m_Items.PopBack();
        Notify(EArrayChange::Removed, index, nullptr);
    }

    /**
     * value と == で一致する最初の要素を、順序を保って削除する。
     *
     * @details 削除できた場合だけ Removed を一致位置で 1 回通知する。通知中の
     * 再変更は禁止し、見つからない場合は通知も行わない。
     * @param value 削除する値。配列内の要素自身を渡してもよい。
     * @return 削除できたら true。見つからなければ配列を変えず false。
     */
    bool Remove(const T& value) noexcept {
        // 最初に一致した削除位置。
        const usize index = m_Items.IndexOf(value);
        if (index == TArray<T>::kNpos) return false;
        RemoveAt(index);
        return true;
    }

    /**
     * 指定 index の要素を書き換え Changed を通知する。
     *
     * @details 範囲外なら何もしない。既存値と operator== で同値なら通知せずスキップする。
     * @param index 書き換える要素の位置。
     * @param v 設定する新しい値 (ムーブで取り込む)。
     */
    void SetAt(usize index, T v) noexcept {
        if (index >= m_Items.Size()) return;
        if (m_Items[index] == v) return;
        AssertMutationOK();
        m_Items[index] = Move(v);
        Notify(EArrayChange::Changed, index, &m_Items[index]);
    }

    /**
     * 全要素を削除し Cleared を通知する (空なら何もしない)。
     */
    void Clear() noexcept {
        if (m_Items.Size() == 0) return;
        AssertMutationOK();
        m_Items.Clear();
        Notify(EArrayChange::Cleared, 0, nullptr);
    }

    /**
     * 要素数を返す。
     *
     * @return 現在の要素数。
     */
    usize Size() const noexcept { return m_Items.Size(); }

    /**
     * 空かどうかを返す。
     *
     * @return 要素が無ければ true。
     */
    bool  Empty() const noexcept { return m_Items.Size() == 0; }

    /**
     * 指定 index の要素への const 参照を返す。
     *
     * @param index 取得する要素の位置 (範囲チェックなし)。
     * @return 要素への const 参照。
     */
    const T& At(usize index) const noexcept { return m_Items[index]; }

    /**
     * 指定 index の要素への可変参照を返す。
     *
     * @details 直接書き換えても通知は走らないので、変更通知が要るなら SetAt を使う。
     * @param index 取得する要素の位置 (範囲チェックなし)。
     * @return 要素への可変参照。
     */
    T&       At(usize index)       noexcept { return m_Items[index]; }

    /**
     * 内部配列先頭への const ポインタを返す。
     *
     * @return 要素配列の先頭 (空なら実装依存)。
     */
    const T* Data() const noexcept { return m_Items.Data(); }

    /**
     * 内部配列先頭への可変ポインタを返す。
     *
     * @return 要素配列の先頭 (空なら実装依存)。
     */
    T*       Data()       noexcept { return m_Items.Data(); }

    /**
     * 変更通知リスナを登録する。
     *
     * @details 空きスロットを再利用し、無ければ追加する。cb が null なら無効ハンドルを返す。
     * @param cb 変更時に呼ぶリスナ (null なら登録しない)。
     * @param user リスナへそのまま渡される任意ポインタ。
     * @return 登録した購読を指すハンドル (cb が null なら kInvalidArrayObserver)。
     */
    FArrayObserverHandle Subscribe(Listener cb, void* user) noexcept {
        if (!cb) return kInvalidArrayObserver;
        u32 idx;
        if (m_FreeIndices.Size() > 0) {
            idx = m_FreeIndices[m_FreeIndices.Size() - 1];
            m_FreeIndices.PopBack();
        } else {
            idx = static_cast<u32>(m_Slots.Size());
            m_Slots.PushBack(FSlot{});
        }
        FSlot& s = m_Slots[idx];
        if (s.id == 0) s.id = m_NextId++;
        s.generation++;
        s.active = true;
        s.cb     = cb;
        s.user   = user;
        return FArrayObserverHandle{ s.id, s.generation };
    }

    /**
     * 変更通知リスナを解除する。
     *
     * @details
     * 通知中 (m_NotifyDepth>0) ならスロットを inactive にして遅延回収へ積み、
     * 通知外なら即座に空きにして再利用へ回す。
     * @param h Subscribe が返したハンドル。
     * @return 該当購読を解除できたら true、無効・未登録なら false。
     */
    bool Unsubscribe(FArrayObserverHandle h) noexcept {
        if (!h.IsValid()) return false;
        for (usize i = 0; i < m_Slots.Size(); ++i) {
            FSlot& s = m_Slots[i];
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
    /** 寿命安全テストから private Notify frame chain だけを検証するための内部 seam。 */
    friend struct mvvm_test::FObservableArrayLifetimeTestAccess;

    /**
     * 実行中 Notify の stack frame。
     *
     * @details
     * frame 自身は呼び出し側 stack に置き、確保を行わない。配列のデストラクタは
     * m_Owner を nullptr にするだけで、破棄後に frame が解放済み this を触らない。
     * 同一配列の nested Notify は変更禁止契約に反するが、assert 無効ビルドでは caller の
     * 契約違反を検出できない。その経路で self 破棄されても外側 frame を残さないよう、
     * m_Previous で実行中 chain 全体を保持する。
     */
    struct FNotifyFrame {
        explicit FNotifyFrame(TObservableArray& owner) noexcept
            : m_Owner(&owner),
              m_Previous(owner.m_ActiveNotifyFrame) {
            owner.m_ActiveNotifyFrame = this;
        }

        ~FNotifyFrame() noexcept {
            if (!m_Owner) return;
            ACS_ASSERT(m_Owner->m_ActiveNotifyFrame == this);
            m_Owner->m_ActiveNotifyFrame = m_Previous;
        }

        FNotifyFrame(const FNotifyFrame&) = delete;
        FNotifyFrame& operator=(const FNotifyFrame&) = delete;

        /** owner が callback 中に破棄されていなければ true。 */
        bool IsOwnerAlive() const noexcept { return m_Owner != nullptr; }

        TObservableArray* m_Owner = nullptr;
        FNotifyFrame* m_Previous = nullptr;
    };

    /**
     * 破棄中の配列を待っている全 Notify frame を dead にする。
     *
     * @details frame はすべて現在 thread の生存 stack 上にある。各 frame の owner だけを
     * 無効化し、配列の member storage を解放した後に this へ戻らないようにする。
     */
    void InvalidateNotifyFrames() noexcept {
        FNotifyFrame* frame = m_ActiveNotifyFrame;
        while (frame) {
            FNotifyFrame* const previous = frame->m_Previous;
            frame->m_Owner = nullptr;
            frame = previous;
        }
        m_ActiveNotifyFrame = nullptr;
    }

    /**
     * 通知中の変更操作が行われていないかを検証する。
     *
     * @details
     * リスナ (Notify) 実行中の PushBack/PopBack/Remove/RemoveAt/SetAt/Clear は、
     * 要素配列の再配置、通知の再入、通知対象の途中変更を招くためすべて禁止する。
     * Debug ビルドでは m_NotifyDepth==0 を assert する。assert を無効化した Release
     * ビルドでは検出されず変更処理が続くため、caller が契約を守らなければならない。
     */
    void AssertMutationOK() const noexcept {
        // listener 中の全変更を Debug で検出する。assert 無効構成では処理を止めないため、
        // callback 側が必ず通知後の処理キューへ遅延する。
        ACS_ASSERTF(m_NotifyDepth == 0,
                    "ObservableArray: mutation during Notify is not allowed (depth=%d)",
                    m_NotifyDepth);
    }

    /**
     * 全アクティブリスナへ 1 件の変更を通知する。
     *
     * @details
     * m_NotifyDepth で再入を数え、通知中に積まれた遅延解除を最外ループ終了時に回収する。
     * リスナ呼び出し前に関数と user をローカルへ退避し、復帰直後は stack 上の
     * FNotifyFrame だけを確認する。owner が破棄済みなら this を参照せず return する。
     * @param kind 通知する変更種別。
     * @param index 変更が起きた要素の位置。
     * @param value 該当値へのポインタ (削除/全消去では nullptr)。
     */
    void Notify(EArrayChange kind, usize index, const T* value) noexcept {
        FNotifyFrame notify_frame(*this);
        ++m_NotifyDepth;
        const usize n = m_Slots.Size();
        for (usize i = 0; i < n; ++i) {
            FSlot& s = m_Slots[i];
            if (!s.active || !s.cb) continue;
            const Listener callback = s.cb;
            void* const user = s.user;
            callback(kind, index, value, user);
            if (!notify_frame.IsOwnerAlive()) return;
        }
        --m_NotifyDepth;
        if (m_NotifyDepth == 0 && m_PendingCancel.Size() > 0) {
            for (usize i = 0; i < m_PendingCancel.Size(); ++i) {
                const u32 idx = m_PendingCancel[i];
                if (idx < m_Slots.Size()) {
                    FSlot& s = m_Slots[idx];
                    s.cb   = nullptr;
                    s.user = nullptr;
                    m_FreeIndices.PushBack(idx);
                }
            }
            m_PendingCancel.Clear();
        }
    }

    /** 1 リスナ分の購読スロット。 */
    struct FSlot {
        /** スロット ID (ハンドル照合に使う)。 */
        u32      id          = 0;

        /** 世代カウンタ (再利用のたびに増やしハンドルを陳腐化させる)。 */
        u32      generation  = 0;

        /** このスロットが現在有効かを示す。 */
        bool     active      = false;

        /** 通知時に呼ぶリスナ関数。 */
        Listener cb          = nullptr;

        /** リスナへ渡す user ポインタ。 */
        void*    user        = nullptr;
    };

    /** 保持している要素列。 */
    TArray<T>     m_Items;

    /** 購読スロット配列 (空き分は再利用される)。 */
    TArray<FSlot>  m_Slots;

    /** 空きスロットのインデックス (再利用候補)。 */
    TArray<u32>   m_FreeIndices;

    /** 通知中に解除されたスロットの遅延回収待ちインデックス。 */
    TArray<u32>   m_PendingCancel;

    /** 次に割り当てるスロット ID (1 から増加)。 */
    u32          m_NextId      = 1;

    /** 通知の再入深度 (>0 の間は解除を遅延し変更を禁止する)。 */
    i32          m_NotifyDepth = 0;

    /** 現在実行中の最内 Notify frame (未通知時は nullptr、非所有)。 */
    FNotifyFrame* m_ActiveNotifyFrame = nullptr;
};

} // namespace acs
