// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "memory/New.h"
#include "container/Array.h"
#include "container/HashMap.h"
#include "ecs/Entity.h"
#include "ecs/ComponentId.h"
#include "ecs/SparseSet.h"

namespace acs {

/**
 * エンティティとコンポーネントを管理する ECS の中心 (sparse-set ストレージ)。
 *
 * @details
 * エンティティは世代付きスロットで管理し、Destroy したスロットは世代を進めて
 * フリーリストに戻して再利用する (古い FEntityId は世代不一致で無効化)。コンポーネント
 * 型ごとに TSparseSet を 1 つ持ち、FComponentTypeId を添字に引く。Query で複数
 * コンポーネントを横断走査する。non-copy 型。
 */
class CWorld {
public:
    /** 空の CWorld を構築する。 */
    CWorld() noexcept;

    /** CWorld を破棄する (全 TSparseSet を解放)。 */
    ~CWorld() noexcept;

    /** コピー禁止 (TSparseSet を所有するため)。 */
    CWorld(const CWorld&) = delete;

    /** コピー代入も禁止。 */
    CWorld& operator=(const CWorld&) = delete;

    /**
     * エンティティを生成する。
     *
     * @details フリースロットがあれば再利用し (その際スロット世代は既に進んでいる)、
     * 無ければ新規スロットを確保する。
     * @return 生成したエンティティの FEntityId。
     */
    FEntityId Create() noexcept;

    /**
     * エンティティを削除する (全コンポーネントを除去し、スロットを再利用待ちへ戻す)。
     *
     * @details スロットの世代を +1 して、削除前に得た FEntityId を以後無効化する。
     * 既に無効な ID なら何もしない。
     * @param e 削除するエンティティ。
     */
    void Destroy(FEntityId e) noexcept;

    /**
     * 全エンティティとコンポーネントストレージを解放し、空の CWorld に戻す。
     *
     * @details MemorySystem の終了前に、実行中に選ばれた既定アロケータを使う
     * TSparseSet を確実に破棄するためにも使用する。繰り返し呼んでも安全。
     */
    void Clear() noexcept;

    /**
     * src の完全な複製をこの CWorld に作る (snapshot / rollback 用)。
     *
     * @details
     * エンティティスロット (世代含む)・フリーリスト・全 TSparseSet の値をコピーする。
     * 世代までコピーするため、snapshot 時に取った FEntityId は復元後もそのまま有効で、
     * snapshot 後に生成した FEntityId は復元で無効になる (rollback netcode の要件)。
     *
     *   CWorld backup;
     *   backup.CopyFrom(world);    // フレーム N の状態を退避
     *   ...                        // 予測実行でフレーム N+k まで進める
     *   world.CopyFrom(backup);    // 権威入力が届いたらフレーム N へ巻き戻す
     *
     * 全コンポーネント型がコピー構築可能である必要がある。非コピー型の TSparseSet が
     * あるか OOM の場合は false を返し、this は空 (Clear 済み) の状態になる
     * (部分複製は残さない)。this == &src は何もせず true。
     * @param src 複製元の CWorld。
     * @return 完全に複製できたら true。
     */
    bool CopyFrom(const CWorld& src) noexcept;

    /**
     * エンティティが現在も生存しているかを返す (世代チェック)。
     *
     * @param e 判定するエンティティ。
     * @return スロットが生存中かつ世代が一致すれば true。
     */
    bool IsAlive(FEntityId e) const noexcept;

    /**
     * エンティティにコンポーネントを追加する (既に存在すれば上書き)。
     *
     * @details e が生存していなければ何もしない。型 T の TSparseSet が無ければ生成する。
     * @tparam T 追加するコンポーネント型。
     * @param e 追加先のエンティティ。
     * @param value 格納する値 (ムーブで取り込む)。
     */
    template<typename T>
    void Add(FEntityId e, T value) noexcept {
        if (!IsAlive(e)) return;
        TSparseSet<T>& set = GetOrCreateSet<T>();
        set.Add(e.index, Move(value));
    }

    /**
     * エンティティの T コンポーネントへの可変ポインタを返す。
     *
     * @details 返したポインタは当該 TSparseSet への Add/Remove (再確保・swap-remove) で
     * 無効化され得るため、構造変更をまたいで保持しない。
     * @tparam T 取得するコンポーネント型。
     * @param e 取得対象のエンティティ。
     * @return T へのポインタ (e が無効・型未登録・未保持なら nullptr)。
     */
    template<typename T>
    T* Get(FEntityId e) noexcept {
        if (!IsAlive(e)) return nullptr;
        TSparseSet<T>* set = TryGetSet<T>();
        return set ? set->Get(e.index) : nullptr;
    }

    /**
     * エンティティの T コンポーネントへの const ポインタを返す。
     *
     * @tparam T 取得するコンポーネント型。
     * @param e 取得対象のエンティティ。
     * @return T への const ポインタ (e が無効・型未登録・未保持なら nullptr)。
     */
    template<typename T>
    const T* Get(FEntityId e) const noexcept {
        if (!IsAlive(e)) return nullptr;
        const TSparseSet<T>* set = const_cast<CWorld*>(this)->TryGetSet<T>();
        return set ? set->Get(e.index) : nullptr;
    }

    /**
     * エンティティが T コンポーネントを持つかを返す。
     *
     * @tparam T 判定するコンポーネント型。
     * @param e 判定対象のエンティティ。
     * @return 持っていれば true。
     */
    template<typename T>
    bool Has(FEntityId e) const noexcept { return Get<T>(e) != nullptr; }

    /**
     * エンティティから T コンポーネントを除去する (無くても安全)。
     *
     * @tparam T 除去するコンポーネント型。
     * @param e 除去対象のエンティティ。
     */
    template<typename T>
    void Remove(FEntityId e) noexcept {
        if (!IsAlive(e)) return;
        TSparseSet<T>* set = TryGetSet<T>();
        if (set) set->Remove(e.index);
    }

    /**
     * 複数コンポーネントを横断走査するクエリビューを返す。
     *
     * @details 実装は Query.h (TQueryView を生成して返す)。
     * @tparam Comps 同時に要求するコンポーネント型。
     * @return TQueryView<Comps...>。
     */
    template<typename... Comps>
    auto Query() noexcept;  // Query.h で定義

    /**
     * 型 T の TSparseSet を返す (無ければ生成する)。内部用。
     *
     * @tparam T 取得・生成する TSparseSet のコンポーネント型。
     * @return 型 T の TSparseSet への参照。
     */
    template<typename T>
    TSparseSet<T>& GetOrCreateSet() noexcept {
        const FComponentTypeId id = GetComponentTypeId<T>();
        if (id >= m_Sets.Num()) m_Sets.SetNum(id + 1);
        if (!m_Sets[id]) {
            // m_Sets の allocator で sparse set を確保する。ASparseSetBase の
            // 仮想デストラクタで型ごとの破棄が走るため、解放は CWorld::Clear の Delete で型消去できる。
            TSparseSet<T>* const set = New<TSparseSet<T>>(*m_Sets.GetAllocator());
            ACS_CHECKF(set != nullptr, "World::GetOrCreateSet: SparseSet 確保失敗 (id=%u)", id);
            m_Sets[id] = static_cast<ASparseSetBase*>(set);
        }
        return *static_cast<TSparseSet<T>*>(m_Sets[id]);
    }

    /**
     * 型 T の TSparseSet を返す (未生成なら nullptr)。内部用。
     *
     * @tparam T 取得する TSparseSet のコンポーネント型。
     * @return 型 T の TSparseSet へのポインタ (未生成なら nullptr)。
     */
    template<typename T>
    TSparseSet<T>* TryGetSet() noexcept {
        const FComponentTypeId id = GetComponentTypeId<T>();
        if (id >= m_Sets.Num()) return nullptr;
        return static_cast<TSparseSet<T>*>(m_Sets[id]);
    }

    /**
     * 生存中のエンティティ数を返す。
     *
     * @return 現在生きているエンティティの数。
     */
    u32 EntityCount() const noexcept { return m_AliveCount; }

    /**
     * スロット index に現在の世代を付けて FEntityId を復元する (Query 内部用)。
     *
     * @param index 復元するスロット番号。
     * @return そのスロットの現世代を付けた FEntityId (範囲外なら kInvalidEntity)。
     */
    FEntityId MakeIdFromIndex(u32 index) const noexcept {
        if (index >= m_Slots.Num()) return kInvalidEntity;
        return FEntityId{ index, m_Slots[index].generation };
    }

private:
    /**
     * 1 エンティティスロット (世代と生存フラグを保持)。
     */
    struct FSlot {
        /** スロットの現世代 (Destroy のたびに +1)。 */
        u32  generation = 0;

        /** スロットが生存中かのフラグ。 */
        bool alive      = false;
    };

    /** スロット配列 (index → FSlot)。 */
    TArray<FSlot>           m_Slots;

    /** 解放済みで再利用待ちのスロット番号。 */
    TArray<u32>            m_FreeIndices;

    /** コンポーネント型ごとの TSparseSet (FComponentTypeId → ASparseSetBase*、所有権を持つ)。 */
    TArray<ASparseSetBase*> m_Sets;

    /** 生存中のエンティティ数。 */
    u32                   m_AliveCount = 0;

    /** Clear 前の FEntityId が再生成後に一致しないよう、新規スロットへ与える世代。 */
    u32 m_GenerationSeed = 0;
};

/** 移行期間中に旧名を受け付ける互換別名。 */
using FWorld = CWorld;

} // namespace acs
