// ObservableArray<T> — 要素の追加・削除・変更を通知する配列
//
// 使い方:
//   ObservableArray<int> inv;
//
//   inv.Subscribe([](ArrayChange kind, usize idx, const int* v, void*){
//       switch (kind) {
//           case ArrayChange::Inserted: ACS_LOG_INFO("Add[%zu] = %d", idx, *v); break;
//           case ArrayChange::Removed:  ACS_LOG_INFO("Remove[%zu]", idx);       break;
//           case ArrayChange::Changed:  ACS_LOG_INFO("Set[%zu] = %d", idx, *v); break;
//           case ArrayChange::Cleared:  ACS_LOG_INFO("Cleared all");            break;
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
#include "container/Array.h"

namespace acs {

enum class ArrayChange : u8 {
    Inserted,   // index に value を挿入した
    Removed,    // index にあった要素を削除した (通知時 value はもう存在しない → nullptr)
    Changed,    // index の要素を別の値に書き換えた (value = 新値)
    Cleared,    // 全削除 (index=0, value=nullptr)
};

// 通知ハンドルは Observable<T> と同じ仕様
struct ArrayObserverHandle {
    u32 id         = 0;
    u32 generation = 0;

    bool IsValid() const noexcept { return id != 0; }
    bool operator==(const ArrayObserverHandle& o) const noexcept {
        return id == o.id && generation == o.generation;
    }
};

inline constexpr ArrayObserverHandle kInvalidArrayObserver{};

template<typename T>
class ObservableArray {
public:
    using Listener = void (*)(ArrayChange kind, usize index, const T* value, void* user);

    ObservableArray() noexcept = default;
    ~ObservableArray() noexcept = default;

    ObservableArray(const ObservableArray&)            = delete;
    ObservableArray& operator=(const ObservableArray&) = delete;

    // ---- 要素操作 ----
    void PushBack(T v) noexcept {
        AssertMutationOK();
        usize idx = _items.Size();
        _items.PushBack(Move(v));
        Notify(ArrayChange::Inserted, idx, &_items[idx]);
    }

    // 末尾の要素を削除 (空なら no-op)
    void PopBack() noexcept {
        if (_items.Size() == 0) return;
        AssertMutationOK();
        usize idx = _items.Size() - 1;
        // PopBack 後は要素アクセスできないので value=nullptr で通知
        _items.PopBack();
        Notify(ArrayChange::Removed, idx, nullptr);
    }

    // 任意 index を削除 (末尾入れ替えではなく前詰め — 順序保持)
    void RemoveAt(usize index) noexcept {
        if (index >= _items.Size()) return;
        AssertMutationOK();
        for (usize i = index + 1; i < _items.Size(); ++i) {
            _items[i - 1] = Move(_items[i]);
        }
        _items.PopBack();
        Notify(ArrayChange::Removed, index, nullptr);
    }

    // 要素を書き換え (同値ならスキップ)
    void SetAt(usize index, T v) noexcept {
        if (index >= _items.Size()) return;
        if (_items[index] == v) return;
        AssertMutationOK();
        _items[index] = Move(v);
        Notify(ArrayChange::Changed, index, &_items[index]);
    }

    // 全削除
    void Clear() noexcept {
        if (_items.Size() == 0) return;
        AssertMutationOK();
        _items.Clear();
        Notify(ArrayChange::Cleared, 0, nullptr);
    }

    // ---- 読み取り ----
    usize Size() const noexcept { return _items.Size(); }
    bool  Empty() const noexcept { return _items.Size() == 0; }

    const T& At(usize index) const noexcept { return _items[index]; }
    T&       At(usize index)       noexcept { return _items[index]; }

    const T* Data() const noexcept { return _items.Data(); }
    T*       Data()       noexcept { return _items.Data(); }

    // ---- 購読 ----
    ArrayObserverHandle Subscribe(Listener cb, void* user) noexcept {
        if (!cb) return kInvalidArrayObserver;
        u32 idx;
        if (_free_indices.Size() > 0) {
            idx = _free_indices[_free_indices.Size() - 1];
            _free_indices.PopBack();
        } else {
            idx = static_cast<u32>(_slots.Size());
            _slots.PushBack(Slot{});
        }
        Slot& s = _slots[idx];
        if (s.id == 0) s.id = _next_id++;
        s.generation++;
        s.active = true;
        s.cb     = cb;
        s.user   = user;
        return ArrayObserverHandle{ s.id, s.generation };
    }

    bool Unsubscribe(ArrayObserverHandle h) noexcept {
        if (!h.IsValid()) return false;
        for (usize i = 0; i < _slots.Size(); ++i) {
            Slot& s = _slots[i];
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
    void AssertMutationOK() const noexcept {
        // listener の中から Add/Remove を呼んだら一貫性が崩れる。Debug では assert。
        // Release では何もしない (= 自己責任)。
    }

    void Notify(ArrayChange kind, usize index, const T* value) noexcept {
        ++_notify_depth;
        const usize n = _slots.Size();
        for (usize i = 0; i < n; ++i) {
            Slot& s = _slots[i];
            if (!s.active || !s.cb) continue;
            s.cb(kind, index, value, s.user);
        }
        --_notify_depth;
        if (_notify_depth == 0 && _pending_cancel.Size() > 0) {
            for (usize i = 0; i < _pending_cancel.Size(); ++i) {
                u32 idx = _pending_cancel[i];
                if (idx < _slots.Size()) {
                    Slot& s = _slots[idx];
                    s.cb   = nullptr;
                    s.user = nullptr;
                    _free_indices.PushBack(idx);
                }
            }
            _pending_cancel.Clear();
        }
    }

    struct Slot {
        u32      id          = 0;
        u32      generation  = 0;
        bool     active      = false;
        Listener cb          = nullptr;
        void*    user        = nullptr;
    };

    Array<T>     _items;
    Array<Slot>  _slots;
    Array<u32>   _free_indices;
    Array<u32>   _pending_cancel;
    u32          _next_id      = 1;
    i32          _notify_depth = 0;
};

} // namespace acs
