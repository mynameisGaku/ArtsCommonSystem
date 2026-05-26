// SPDX-License-Identifier: Apache-2.0
// Sparse Set: エンティティ → コンポーネント値の高速対応表
//
// 構造:
//   sparse[entity_index] = dense_index    （疎な参照表、ほとんどが無効値）
//   dense[dense_index]   = entity_index   （密配列、要素を順番に並べる）
//   data[dense_index]    = T              （実際のコンポーネント値）
//
// 効果:
//   ・Add / Remove / Contains が O(1)
//   ・全要素の走査がキャッシュフレンドリーな密配列の線形走査
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "container/Array.h"
#include "ecs/Entity.h"

namespace acs {

// 型消去 SparseSet 基底（型 T に依らない部分のみ）
class SparseSetBase {
public:
    static constexpr u32 kInvalid = 0xFFFFFFFFu;  // 未登録を表す番兵値

    // 仮想デストラクタ（World が型を知らずに delete できるよう必要）
    virtual ~SparseSetBase() noexcept = default;

    // 指定エンティティに対応する dense インデックスを取得（未登録なら kInvalid）
    u32 IndexOf(u32 entity_index) const noexcept {
        if (entity_index >= _sparse.Size()) return kInvalid;
        return _sparse[entity_index];
    }

    bool Contains(u32 entity_index) const noexcept { return IndexOf(entity_index) != kInvalid; }

    // 要素数（コンポーネントを持つエンティティの数）
    usize Size() const noexcept { return _dense.Size(); }

    // dense 列でエンティティ順に走査するための配列
    const u32* DenseEntities() const noexcept { return _dense.Data(); }

    // 型を知らずに「このエンティティの値」を削除する（World::Destroy で使う）
    virtual void RemoveErased(u32 entity_index) noexcept = 0;

protected:
    // sparse 配列を必要なら拡張する
    void EnsureSparseCapacity(u32 entity_index) noexcept {
        if (entity_index >= _sparse.Size()) {
            usize old = _sparse.Size();
            _sparse.Resize(entity_index + 1);
            for (usize i = old; i <= entity_index; ++i) _sparse[i] = kInvalid;
        }
    }

    TArray<u32> _sparse;   // entity_index -> dense_index
    TArray<u32> _dense;    // dense_index -> entity_index
};

// 型付き SparseSet（コンポーネント T 専用）
template<typename T>
class SparseSet : public SparseSetBase {
public:
    SparseSet() noexcept = default;
    ~SparseSet() noexcept override = default;

    // エンティティに値を追加（既に存在する場合は上書き）
    void Add(u32 entity_index, T value) noexcept {
        EnsureSparseCapacity(entity_index);
        u32 idx = _sparse[entity_index];
        if (idx != kInvalid) {
            _data[idx] = Move(value);  // 上書き
            return;
        }
        u32 new_idx = static_cast<u32>(_dense.Size());
        _sparse[entity_index] = new_idx;
        _dense.PushBack(entity_index);
        _data.PushBack(Move(value));
    }

    // エンティティから値を削除（末尾と入れ替えて O(1) 削除）
    void Remove(u32 entity_index) noexcept {
        u32 idx = IndexOf(entity_index);
        if (idx == kInvalid) return;

        u32 last = static_cast<u32>(_dense.Size() - 1);
        if (idx != last) {
            // 末尾要素を idx 位置に移動
            u32 last_entity = _dense[last];
            _dense[idx] = last_entity;
            _data[idx]  = Move(_data[last]);
            _sparse[last_entity] = idx;
        }
        _dense.PopBack();
        _data.PopBack();
        _sparse[entity_index] = kInvalid;
    }

    // 値ポインタ取得（未登録なら nullptr）
    T* Get(u32 entity_index) noexcept {
        u32 idx = IndexOf(entity_index);
        return idx == kInvalid ? nullptr : &_data[idx];
    }
    const T* Get(u32 entity_index) const noexcept {
        u32 idx = IndexOf(entity_index);
        return idx == kInvalid ? nullptr : &_data[idx];
    }

    // 全要素の生配列（dense 順）
    T*       Data()       noexcept { return _data.Data(); }
    const T* Data() const noexcept { return _data.Data(); }

    // 型消去 Remove（基底経由で呼ぶ用）
    void RemoveErased(u32 entity_index) noexcept override { Remove(entity_index); }

private:
    TArray<T> _data;
};

} // namespace acs
