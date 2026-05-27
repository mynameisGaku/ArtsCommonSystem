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

// 購読ハンドル
struct ObservableHandle {
    u32 id         = 0;    // 1-based、Slot のインデックス 1..N
    u64 generation = 0;    // 64-bit にして wrap 起因の誤判定を実質ゼロに

    bool IsValid() const noexcept { return id != 0; }
    bool operator==(const ObservableHandle& o) const noexcept {
        return id == o.id && generation == o.generation;
    }
};

inline constexpr ObservableHandle kInvalidObservable{};

template<typename T>
class Observable {
public:
    using Listener = void (*)(const T& new_value, void* user);

    Observable() noexcept = default;
    explicit Observable(T initial) noexcept : m_Value(Move(initial)) {}

    Observable(const Observable&) = delete;
    Observable& operator=(const Observable&) = delete;

    // 値の取得 / 設定
    const T& Get() const noexcept { return m_Value; }
    operator const T&() const noexcept { return m_Value; }

    // 値を変更し、変わった場合のみ全監視者に通知
    void Set(const T& v) noexcept {
        ACS_THREAD_AFFINITY_CHECK();
        if (m_Value == v) return;
        m_Value = v;
        Notify();
    }
    void Set(T&& v) noexcept {
        ACS_THREAD_AFFINITY_CHECK();
        if (m_Value == v) return;
        m_Value = Move(v);
        Notify();
    }

    // 強制通知 (値が同じでも発火、内部コンテナが書き換えられた場合等)
    void ForceNotify() noexcept { Notify(); }

    // 監視を追加
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

    // 監視を解除
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

    u32 SubscriberCount() const noexcept {
        u32 n = 0;
        for (usize i = 0; i < m_Slots.Size(); ++i) if (m_Slots[i].active) ++n;
        return n;
    }

private:
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
        u64      generation  = 0;
        bool     active      = false;
        Listener cb          = nullptr;
        void*    user        = nullptr;
    };

    T            m_Value{};
    TArray<Slot>  m_Slots;
    TArray<u32>   m_FreeIndices;
    TArray<u32>   m_PendingCancel;
    u32          m_NextId      = 1;
    i32          m_NotifyDepth = 0;
    ACS_THREAD_AFFINITY_FIELD();
};

} // namespace acs
