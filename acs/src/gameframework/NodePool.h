// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNodePool (FNode2D の generational pool)
//
// シーン全体で唯一の `FNode2D*` レジストリ。`FNode2D` インスタンス自体は親の
// `m_Children` (TUniquePtr<FNode2D>) が所有し続け、本 pool は **参照のみ** を
// 保持して安定した `FNodeId` を発行する。同時に発行済 `FNodeId` の **stale 検出**
// (= 既に Unregister されたハンドル) を提供する。
//
// 使い方:
//   FNodePool pool;
//   pool.Init(/*initial_capacity=*/512);
//
//   // FNode2D を新規生成して scene tree に attach した直後に登録:
//   FNode2D* enemy_ptr = scene.Root().AddChild(MakeUnique<EnemyNode>()).Get();
//   FNodeId enemy_id = pool.RegisterExistingNode(enemy_ptr);
//   // enemy_ptr->Id() == enemy_id が成立する (RegisterExistingNode 内で _SetId 済)
//
//   // 後で stale 検査付きで取り出し:
//   if (FNode2D* p = pool.Get(enemy_id)) {
//       p->Local().position += FVec2{1, 0};
//   }
//
//   // 破棄時:
//   pool.Unregister(enemy_id);   // slot 解放 + gen++ → 古い handle は invalid 化
//   enemy_ptr->Destroy();        // scene tree からは別途 reap される
//
// 設計選択 (Pillar B):
//   ・**non-owning**: 所有権は FNode2D の親 (=TUniquePtr<FNode2D>) 側にあり、本 pool
//     は raw ポインタだけ持つ。TPool の破棄や ClearAll は FNode2D を delete しない。
//   ・**Slot = {ptr, gen, active}**: FCollisionWorld2D::Slot と同じパターン。
//     index 0 は予約 (= invalid handle と一致させる)、有効 slot は 1..N。
//   ・**free_indices stack**: 空き slot を O(1) で再利用。Unregister 時 push、
//     RegisterExistingNode 時 pop。stack が空なら slot を新規 PushBack。
//   ・**generation 0 はスキップ**: gen++ がラップアラウンドで 0 に戻った場合、
//     FNodeId(idx, 0) は IsValid() == false になってしまうため、ラップ時は 1 に
//     強制する (FCollisionWorld2D と完全に同じ挙動)。
//   ・**24bit index = 16,777,216 slot 上限**: FNodeId の pack 仕様に従い、これを
//     超える RegisterExistingNode は invalid FNodeId を返す (拒否)。実用上 1 シーン
//     で 16M Node を生成することはまずあり得ないが安全策として明示拒否。
//   ・**IdOf は線形探索**: ポインタ → FNodeId の逆引きは利用頻度が低い (基本は
//     RegisterExistingNode の戻り値を保持する) ため、専用 hash は持たない。
//     真に必要なら呼び出し側で THashMap<FNode2D*, FNodeId> を別途持てば良い。
//   ・**非コピー・非ムーブ**: pool 自体は固定オブジェクトとして scene が所有する想定。
//   ・**全 noexcept / STL 不使用 / `<string>` 禁止**: ACS 規約に厳格準拠。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/NodeId.h"

namespace acs::game {

class FNode2D;   // forward decl — full include は .cpp 側 (FNode2D::_SetId 呼出のため)

/**
 * FNode2D 群を pool で管理し、安定した FNodeId を発行 + stale 検出する。
 *
 * @details
 * FNode2D の所有権は持たない (FNode2D は親の TUniquePtr が所有)。本 pool は raw
 * ポインタだけを保持し、generational handle (FNodeId) の発行と stale 検出を担う。
 * Slot = {ptr, gen, active} 構成で、index 0 は invalid 用に予約、有効 slot は 1..N。
 * 空き slot は free stack で O(1) 再利用する。
 */
class FNodePool {
public:
    /** 空の pool を構築する (slot 配列は Init / 初回 Register で確保)。 */
    FNodePool()  noexcept = default;

    /** 破棄する (FNode2D は非所有なので何も delete しない)。 */
    ~FNodePool() noexcept = default;

    /** コピー禁止 (pool は scene が固定オブジェクトとして単独所有するため)。 */
    FNodePool(const FNodePool&)            = delete;

    /** コピー代入も禁止。 */
    FNodePool& operator=(const FNodePool&) = delete;

    /** ムーブ禁止。 */
    FNodePool(FNodePool&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FNodePool& operator=(FNodePool&&)      = delete;

    /**
     * 初期容量を予約する (再 alloc 回避用)。
     *
     * @details 複数回呼出可、縮小はしない。index 0 の dummy slot を未確保なら確保する。
     * @param initial_capacity 予約する slot 数 (0 なら dummy 確保のみで reserve しない)。
     */
    void Init(u32 initial_capacity = 256) noexcept;

    /**
     * 既存の生 FNode2D を pool に登録し、新しい FNodeId を発行する。
     *
     * @details 発行した FNodeId は node->_SetId() で node 自身にも書き込む。
     * @param node 登録する FNode2D (nullptr、または slot 数が 16M に達したときは登録しない)。
     * @return 発行した FNodeId。失敗時は invalid (この場合 node の Id は変更しない)。
     */
    FNodeId RegisterExistingNode(FNode2D* node) noexcept;

    /**
     * slot を free 化し、対応 FNode2D の Id を invalid にリセットする。
     *
     * @details 既に invalid / stale な id は何もしない (二重 Unregister は安全)。
     * generation は次の AcquireSlot で進むため、ここでは進めない。
     * @param id 解放する slot の FNodeId。
     */
    void Unregister(FNodeId id) noexcept;

    /**
     * id が指す slot が active かつ generation が一致するかを返す。
     *
     * @param id 検証する FNodeId。
     * @return slot が生きていて世代も一致すれば true。
     */
    bool IsValid(FNodeId id) const noexcept;

    /**
     * id 経由で FNode2D* を取り出す。
     *
     * @param id 取り出す FNodeId。
     * @return 対応する FNode2D。stale / invalid なら nullptr。
     */
    FNode2D* Get(FNodeId id) const noexcept;

    /**
     * node ポインタから FNodeId を逆引きする (線形探索 O(N))。
     *
     * @param node 逆引きする FNode2D。
     * @return 対応する FNodeId。node == nullptr または pool に存在しなければ invalid。
     */
    FNodeId IdOf(FNode2D* node) const noexcept;

    /**
     * 現在 active な slot 数を返す。
     *
     * @return active な slot 数。
     */
    u32 ActiveCount() const noexcept { return m_ActiveCount; }

    /**
     * 現在の slot 数を返す (内部配列長 - 1、index 0 予約分を除外)。
     *
     * @details 物理的に確保された FNode2D 個数の上限ではなく、過去に到達した最大値の指標。
     * @return index 0 の dummy を除いた slot 数。
     */
    u32 Capacity() const noexcept {
        const u32 sz = static_cast<u32>(m_Slots.Size());
        return sz > 0 ? sz - 1u : 0u;   // index 0 予約分を引く
    }

    /**
     * 全 slot を一括 free する。
     *
     * @details 各登録済 FNode2D の Id を invalid にリセットし、free_indices stack もクリアする。
     * FNode2D 自体は削除しない (非所有)。gen は維持するため、ClearAll 前の handle は再利用後も
     * 確実に stale 検出される。
     */
    void ClearAll() noexcept;

private:
    /**
     * 1 つの slot エントリ (登録された FNode2D の参照と世代)。
     */
    struct Slot {
        /** 登録された FNode2D (非所有。所有は親 TUniquePtr 側)。 */
        FNode2D* ptr    = nullptr;

        /** 世代カウンタ (0 は予約 = invalid handle と一致。有効 slot は 1..255)。 */
        u8      gen    = 0;

        /** この slot が現在使用中かどうか。 */
        bool    active = false;
    };

    /**
     * 空き slot を 1 つ取得する (free stack → 末尾追加の順)。
     *
     * @details index 0 は予約。16M 上限に到達した場合は 0 を返す。
     * @return 取得した slot の index。確保不能なら 0 (呼出側で invalid FNodeId 化)。
     */
    u32 AcquireSlot() noexcept;

    /** 24bit index 上限 (FNodeId pack 仕様に合わせる、= 16,777,215)。 */
    static constexpr u32 kMaxIndex = 0x00FFFFFFu;

    /** slot 配列 (index 0 は dummy = invalid 用予約)。 */
    TArray<Slot> m_Slots;

    /** 空き slot index の LIFO stack (pop → 再利用、empty → 末尾追加)。 */
    TArray<u32>  m_FreeIndices;

    /** 現在 active な slot 数。 */
    u32         m_ActiveCount = 0;
};

} // namespace acs::game
