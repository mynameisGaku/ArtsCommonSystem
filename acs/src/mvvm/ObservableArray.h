// SPDX-License-Identifier: Apache-2.0
// ObservableArray<T> — 要素の追加・削除・変更を通知する配列
//
// 使い方:
//   ObservableArray<int> inv;
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
//   ・listener 中に Add/Remove を呼ぶのは不正 (assert 検出、Release では無視)
//     → どうしても要るなら listener が処理キューに積んで後で実行
//   ・要素の T は同値判定 (operator==) を持つこと推奨 (Set で同値スキップ)
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "foundation/Assert.h"
#include "container/Array.h"

namespace acs {

enum class EArrayChange : u8 {
    Inserted,   // index に value を挿入した
    Removed,    // index にあった要素を削除した (通知時 value はもう存在しない → nullptr)
    Changed,    // index の要素を別の値に書き換えた (value = 新値)
    Cleared,    // 全削除 (index=0, value=nullptr)
};

// 通知ハンドルは Observable<T> と同じ仕様
struct FArrayObserverHandle {
    u32 id         = 0;
    u32 generation = 0;

    bool IsValid() const noexcept { return id != 0; }
    bool operator==(const FArrayObserverHandle& o) const noexcept {
        return id == o.id && generation == o.generation;
    }
};

inline constexpr FArrayObserverHandle kInvalidArrayObserver{};

template<typename T>
class ObservableArray {
public:
    using Listener = void (*)(EArrayChange kind, usize index, const T* value, void* user);

    ObservableArray() noexcept = default;
    ~ObservableArray() noexcept = default;

    ObservableArray(const ObservableArray&)            = delete;
    ObservableArray& operator=(const ObservableArray&) = delete;

    // ---- 要素操作 ----
    void PushBack(T v) noexcept {
        AssertMutationOK();
        usize idx = m_Items.Size();
        m_Items.PushBack(Move(v));
        Notify(EArrayChange::Inserted, idx, &m_Items[idx]);
    }

    // 末尾の要素を削除 (空なら no-op)
    void PopBack() noexcept {
        if (m_Items.Size() == 0) return;
        AssertMutationOK();
        usize idx = m_Items.Size() - 1;
        // PopBack 後は要素アクセスできないので value=nullptr で通知
        m_Items.PopBack();
        Notify(EArrayChange::Removed, idx, nullptr);
    }

    // 任意 index を削除 (末尾入れ替えではなく前詰め — 順序保持)
    void RemoveAt(usize index) noexcept {
        if (index >= m_Items.Size()) return;
        AssertMutationOK();
        for (usize i = index + 1; i < m_Items.Size(); ++i) {
            m_Items[i - 1] = Move(m_Items[i]);
        }
        m_Items.PopBack();
        Notify(EArrayChange::Removed, index, nullptr);
    }

    // 要素を書き換え (同値ならスキップ)
    void SetAt(usize index, T v) noexcept {
        if (index >= m_Items.Size()) return;
        if (m_Items[index] == v) return;
        AssertMutationOK();
        m_Items[index] = Move(v);
        Notify(EArrayChange::Changed, index, &m_Items[index]);
    }

    // 全削除
    void Clear() noexcept {
        if (m_Items.Size() == 0) return;
        AssertMutationOK();
        m_Items.Clear();
        Notify(EArrayChange::Cleared, 0, nullptr);
    }

    // ---- 読み取り ----
    usize Size() const noexcept { return m_Items.Size(); }
    bool  Empty() const noexcept { return m_Items.Size() == 0; }

    const T& At(usize index) const noexcept { return m_Items[index]; }
    T&       At(usize index)       noexcept { return m_Items[index]; }

    const T* Data() const noexcept { return m_Items.Data(); }
    T*       Data()       noexcept { return m_Items.Data(); }

    // ---- 購読 ----
    FArrayObserverHandle Subscribe(Listener cb, void* user) noexcept {
        if (!cb) return kInvalidArrayObserver;
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
        return FArrayObserverHandle{ s.id, s.generation };
    }

    bool Unsubscribe(FArrayObserverHandle h) noexcept {
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

    u32 SubscriberCount() const noexcept {
        u32 n = 0;
        for (usize i = 0; i < m_Slots.Size(); ++i) if (m_Slots[i].active) ++n;
        return n;
    }

private:
    void AssertMutationOK() const noexcept {
        // listener (Notify) 中に Add/Remove を呼ぶと要素配列の再配置で
        // 同じ listener ループに dangling pointer が渡される危険がある。
        // Debug ビルドで検出 (Release はコスト 0)。
        ACS_ASSERTF(m_NotifyDepth == 0,
                    "ObservableArray: mutation during Notify is not allowed (depth=%d)",
                    m_NotifyDepth);
    }

    void Notify(EArrayChange kind, usize index, const T* value) noexcept {
        ++m_NotifyDepth;
        const usize n = m_Slots.Size();
        for (usize i = 0; i < n; ++i) {
            Slot& s = m_Slots[i];
            if (!s.active || !s.cb) continue;
            s.cb(kind, index, value, s.user);
        }
        --m_NotifyDepth;
        if (m_NotifyDepth == 0 && m_PendingCancel.Size() > 0) {
            for (usize i = 0; i < m_PendingCancel.Size(); ++i) {
                u32 idx = m_PendingCancel[i];
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

    struct Slot {
        u32      id          = 0;
        u32      generation  = 0;
        bool     active      = false;
        Listener cb          = nullptr;
        void*    user        = nullptr;
    };

    TArray<T>     m_Items;
    TArray<Slot>  m_Slots;
    TArray<u32>   m_FreeIndices;
    TArray<u32>   m_PendingCancel;
    u32          m_NextId      = 1;
    i32          m_NotifyDepth = 0;
};

} // namespace acs
