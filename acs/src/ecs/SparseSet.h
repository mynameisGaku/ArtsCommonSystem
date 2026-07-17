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
#include "foundation/TypeTraits.h"   // IsCopyConstructibleV (CloneErased の可否判定)
#include "container/Array.h"
#include "memory/New.h"              // New/Delete (CloneErased の複製確保)
#include "memory/Memory.h"           // MemCopy
#include "ecs/Entity.h"

namespace acs {

/**
 * 型 T に依らない SparseSet の型消去基底。
 *
 * @details
 * sparse (entity_index → dense_index) と dense (dense_index → entity_index) の対応を
 * 保持し、走査・存在判定・型消去削除を提供する。実際のコンポーネント値配列 m_Data は
 * 派生 SparseSet<T> が持つ。World が型を知らずにポリモーフィックに扱えるようにする層。
 */
class SparseSetBase {
public:
    /** dense インデックスが未登録であることを表す番兵値。 */
    static constexpr u32 kInvalid = 0xFFFFFFFFu;

    /** 派生 SparseSet<T> を型を知らず正しく破棄するための仮想デストラクタ。 */
    virtual ~SparseSetBase() noexcept = default;

    /**
     * 指定エンティティに対応する dense インデックスを返す。
     *
     * @param entity_index 問い合わせるエンティティのスロット番号。
     * @return 対応する dense インデックス (未登録なら kInvalid)。
     */
    u32 IndexOf(u32 entity_index) const noexcept {
        if (entity_index >= m_Sparse.Size()) return kInvalid;
        return m_Sparse[entity_index];
    }

    /**
     * 指定エンティティがこの集合に含まれるかを返す。
     *
     * @param entity_index 問い合わせるエンティティのスロット番号。
     * @return 含まれていれば true。
     */
    bool Contains(u32 entity_index) const noexcept { return IndexOf(entity_index) != kInvalid; }

    /**
     * 要素数 (コンポーネントを持つエンティティの数) を返す。
     *
     * @return dense 配列の要素数。
     */
    usize Size() const noexcept { return m_Dense.Size(); }

    /**
     * dense 順に並んだエンティティ index の生配列を返す。
     *
     * @details Add/Remove で再確保され得るため、構造変更をまたいでポインタを保持しない。
     * @return dense_index → entity_index 配列の先頭 (要素数は Size())。
     */
    const u32* DenseEntities() const noexcept { return m_Dense.Data(); }

    /**
     * 型を知らずに指定エンティティの値を削除する (World::Destroy から呼ぶ)。
     *
     * @param entity_index 削除するエンティティのスロット番号。
     */
    virtual void RemoveErased(u32 entity_index) noexcept = 0;

    /**
     * 型を知らずにこの集合の完全な複製を確保して返す (World::CopyFrom から呼ぶ)。
     *
     * @details sparse/dense と値配列をすべてコピーした新しい集合を alloc で確保する。
     * T が非コピー構築型の場合と OOM 時は nullptr を返す (部分複製は返さない)。
     * @param alloc 複製の確保に使うアロケータ。
     * @return 複製した集合 (失敗なら nullptr)。所有権は呼び出し側へ移る。
     */
    virtual SparseSetBase* CloneErased(FAllocator& alloc) const noexcept = 0;

protected:
    /**
     * 基底部分 (sparse/dense) を other からコピーする (CloneErased の実装用)。
     *
     * @param other コピー元。
     * @return 両配列をコピーできたら true (OOM なら false)。
     */
    bool CopyBaseFrom(const SparseSetBase& other) noexcept {
        if (!m_Sparse.TryResize(other.m_Sparse.Size())) return false;
        if (!m_Dense.TryResize(other.m_Dense.Size())) return false;
        if (other.m_Sparse.Size() > 0) {
            MemCopy(m_Sparse.Data(), other.m_Sparse.Data(), other.m_Sparse.Size() * sizeof(u32));
        }
        if (other.m_Dense.Size() > 0) {
            MemCopy(m_Dense.Data(), other.m_Dense.Data(), other.m_Dense.Size() * sizeof(u32));
        }
        return true;
    }

    /**
     * entity_index を格納できるよう sparse 配列を必要なら拡張する。
     *
     * @details 拡張した領域は kInvalid で初期化する。
     * @param entity_index 収まる必要のあるスロット番号。
     */
    void EnsureSparseCapacity(u32 entity_index) noexcept {
        if (entity_index >= m_Sparse.Size()) {
            const usize old = m_Sparse.Size();
            m_Sparse.Resize(entity_index + 1);
            for (usize i = old; i <= entity_index; ++i) m_Sparse[i] = kInvalid;
        }
    }

    /** 疎な参照表 (entity_index → dense_index、未登録は kInvalid)。 */
    TArray<u32> m_Sparse;

    /** 密配列 (dense_index → entity_index、要素を詰めて並べる)。 */
    TArray<u32> m_Dense;
};

/**
 * コンポーネント T 専用の型付き SparseSet。
 *
 * @details
 * 基底の sparse/dense に加えて値配列 m_Data を持ち、Add/Remove/Contains を O(1)、
 * 全走査を密配列の線形走査 (キャッシュフレンドリ) で提供する。Remove は末尾要素を
 * 穴に詰める swap-remove のため、削除すると dense 順 (= 走査順) が入れ替わる。
 * @tparam T 格納するコンポーネント型。
 */
template<typename T>
class SparseSet : public SparseSetBase {
public:
    /** 空の集合を構築する。 */
    SparseSet() noexcept = default;

    /** 集合を破棄する (基底の仮想デストラクタを override)。 */
    ~SparseSet() noexcept override = default;

    /**
     * エンティティに値を追加する (既に存在すれば上書き)。
     *
     * @details 新規追加は dense 末尾に積む。新規追加すると Size() が増え dense が
     * 再確保され得るため、走査中の追加は DenseEntities() ポインタを無効化し得る。
     * @param entity_index 値を結びつけるエンティティのスロット番号。
     * @param value 格納する値 (ムーブで取り込む)。
     */
    void Add(u32 entity_index, T value) noexcept {
        EnsureSparseCapacity(entity_index);
        const u32 idx = m_Sparse[entity_index];
        if (idx != kInvalid) {
            m_Data[idx] = Move(value);  // 上書き
            return;
        }
        const u32 new_idx = static_cast<u32>(m_Dense.Size());
        m_Sparse[entity_index] = new_idx;
        m_Dense.PushBack(entity_index);
        m_Data.PushBack(Move(value));
    }

    /**
     * エンティティから値を削除する (末尾と入れ替える O(1) swap-remove)。
     *
     * @details 未登録なら何もしない。穴に末尾要素を詰めるため、削除した位置に
     * 別エンティティが移り dense 順が入れ替わる (走査中の削除は順序を壊す)。
     * @param entity_index 削除するエンティティのスロット番号。
     */
    void Remove(u32 entity_index) noexcept {
        const u32 idx = IndexOf(entity_index);
        if (idx == kInvalid) return;

        const u32 last = static_cast<u32>(m_Dense.Size() - 1);
        if (idx != last) {
            // 末尾要素を idx 位置に移動
            const u32 last_entity = m_Dense[last];
            m_Dense[idx] = last_entity;
            m_Data[idx]  = Move(m_Data[last]);
            m_Sparse[last_entity] = idx;
        }
        m_Dense.PopBack();
        m_Data.PopBack();
        m_Sparse[entity_index] = kInvalid;
    }

    /**
     * エンティティの値への可変ポインタを返す。
     *
     * @param entity_index 取得するエンティティのスロット番号。
     * @return 値へのポインタ (未登録なら nullptr)。
     */
    T* Get(u32 entity_index) noexcept {
        const u32 idx = IndexOf(entity_index);
        return idx == kInvalid ? nullptr : &m_Data[idx];
    }

    /**
     * エンティティの値への const ポインタを返す。
     *
     * @param entity_index 取得するエンティティのスロット番号。
     * @return 値への const ポインタ (未登録なら nullptr)。
     */
    const T* Get(u32 entity_index) const noexcept {
        const u32 idx = IndexOf(entity_index);
        return idx == kInvalid ? nullptr : &m_Data[idx];
    }

    /**
     * 全値を dense 順に並べた可変生配列を返す。
     *
     * @details DenseEntities() と同じ並び。Add/Remove で再確保・並べ替えされ得る。
     * @return 値配列の先頭 (要素数は Size())。
     */
    T*       Data()       noexcept { return m_Data.Data(); }

    /**
     * 全値を dense 順に並べた const 生配列を返す。
     *
     * @return 値配列の先頭 (要素数は Size())。
     */
    const T* Data() const noexcept { return m_Data.Data(); }

    /**
     * 型消去経由で値を削除する (基底 RemoveErased の override、Remove に委譲)。
     *
     * @param entity_index 削除するエンティティのスロット番号。
     */
    void RemoveErased(u32 entity_index) noexcept override { Remove(entity_index); }

    /**
     * この集合の完全な複製を確保して返す (基底 CloneErased の override)。
     *
     * @details T がコピー構築可能な場合のみ複製できる (非コピー型は nullptr)。
     * OOM 時も nullptr (部分複製は返さない)。
     * @param alloc 複製の確保に使うアロケータ。
     * @return 複製した集合 (失敗なら nullptr)。所有権は呼び出し側へ移る。
     */
    SparseSetBase* CloneErased(FAllocator& alloc) const noexcept override {
        if constexpr (!IsCopyConstructibleV<T>) {
            return nullptr;   // 非コピー型の snapshot は不可 (World::CopyFrom が false を返す)
        } else {
            SparseSet<T>* const clone = New<SparseSet<T>>(alloc);
            if (clone == nullptr) return nullptr;
            bool ok = clone->CopyBaseFrom(*this) && clone->m_Data.TryReserve(m_Data.Size());
            for (usize i = 0; ok && i < m_Data.Size(); ++i) {
                ok = clone->m_Data.TryPushBack(m_Data[i]);   // T のコピー構築
            }
            if (!ok) {
                Delete(alloc, clone);
                return nullptr;
            }
            return clone;
        }
    }

private:
    /** コンポーネント値配列 (dense 順、m_Dense と添字を共有)。 */
    TArray<T> m_Data;
};

} // namespace acs
