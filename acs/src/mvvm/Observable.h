// SPDX-License-Identifier: Apache-2.0
// FObservable<T> — 監視可能な値 (MVVM の中核)
//
// 使い方:
//   FObservable<f32> hp { 100.0f };
//
//   // 監視
//   struct FCtx { ... };
//   FCtx c;
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
//   ・スレッドセーフではない (UI スレッドのみ前提、他スレッドからは FMessagePipe 経由)
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "container/Array.h"
#include "threading/ThreadAffinity.h"

namespace acs {

// 購読ハンドル
struct FObservableHandle {
    u32 id         = 0;    // 1-based、FSlot のインデックス 1..N
    u64 generation = 0;    // 64-bit にして wrap 起因の誤判定を実質ゼロに

    bool IsValid() const noexcept { return id != 0; }
    bool operator==(const FObservableHandle& o) const noexcept {
        return id == o.id && generation == o.generation;
    }
};

inline constexpr FObservableHandle kInvalidObservable{};

template<typename T>
class FObservable {
public:
    using Listener = void (*)(const T& new_value, void* user);

    FObservable() noexcept = default;
    explicit FObservable(T initial) noexcept : _value(Move(initial)) {}

    FObservable(const FObservable&) = delete;
    FObservable& operator=(const FObservable&) = delete;

    // 値の取得 / 設定
    const T& Get() const noexcept { return _value; }
    operator const T&() const noexcept { return _value; }

    // 値を変更し、変わった場合のみ全監視者に通知
    void Set(const T& v) noexcept {
        ACS_THREAD_AFFINITY_CHECK();
        if (_value == v) return;
        _value = v;
        Notify();
    }
    void Set(T&& v) noexcept {
        ACS_THREAD_AFFINITY_CHECK();
        if (_value == v) return;
        _value = Move(v);
        Notify();
    }

    // 強制通知 (値が同じでも発火、内部コンテナが書き換えられた場合等)
    void ForceNotify() noexcept { Notify(); }

    // 監視を追加
    FObservableHandle Subscribe(Listener cb, void* user) noexcept {
        ACS_THREAD_AFFINITY_CHECK();
        if (!cb) return kInvalidObservable;
        u32 idx;
        if (_free_indices.Size() > 0) {
            idx = _free_indices[_free_indices.Size() - 1];
            _free_indices.PopBack();
        } else {
            idx = static_cast<u32>(_slots.Size());
            _slots.PushBack(FSlot{});
        }
        FSlot& s = _slots[idx];
        if (s.id == 0) s.id = _next_id++;
        s.generation++;
        s.active = true;
        s.cb     = cb;
        s.user   = user;
        return FObservableHandle{ s.id, s.generation };
    }

    // 監視を解除
    bool Unsubscribe(FObservableHandle h) noexcept {
        if (!h.IsValid()) return false;
        for (usize i = 0; i < _slots.Size(); ++i) {
            FSlot& s = _slots[i];
            if (s.id == h.id && s.generation == h.generation && s.active) {
                if (_notify_depth > 0) {
                    s.active = false;
                    _pending_cancel.PushBack(static_cast<u32>(i));
                } else {
                    s.active = false;
                    s.cb     = nullptr;
                    s.user   = nullptr;
                    _free_indices.PushBack(static_cast<u32>(i));
                }
                return true;
            }
        }
        return false;
    }

    u32 SubscriberCount() const noexcept {
        u32 n = 0;
        for (usize i = 0; i < _slots.Size(); ++i) if (_slots[i].active) ++n;
        return n;
    }

private:
    void Notify() noexcept {
        ++_notify_depth;
        const usize n = _slots.Size();
        for (usize i = 0; i < n; ++i) {
            FSlot& s = _slots[i];
            if (!s.active || !s.cb) continue;
            s.cb(_value, s.user);
        }
        --_notify_depth;
        if (_notify_depth == 0 && _pending_cancel.Size() > 0) {
            for (usize i = 0; i < _pending_cancel.Size(); ++i) {
                u32 idx = _pending_cancel[i];
                if (idx < _slots.Size()) {
                    FSlot& s = _slots[idx];
                    s.cb   = nullptr;
                    s.user = nullptr;
                    _free_indices.PushBack(idx);
                }
            }
            _pending_cancel.Clear();
        }
    }

    struct FSlot {
        u32      id          = 0;
        u64      generation  = 0;
        bool     active      = false;
        Listener cb          = nullptr;
        void*    user        = nullptr;
    };

    T            _value{};
    TArray<FSlot>  _slots;
    TArray<u32>   _free_indices;
    TArray<u32>   _pending_cancel;
    u32          _next_id      = 1;
    i32          _notify_depth = 0;
    ACS_THREAD_AFFINITY_FIELD();
};

} // namespace acs
