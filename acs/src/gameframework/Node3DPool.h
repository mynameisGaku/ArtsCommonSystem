// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNode3DPool (FNode3D の generational pool)
//
// FNodePool (FNode2D 版) の 3D 版。FNode3D インスタンスは親の m_Children
// (TUniquePtr) が所有し続け、本 pool は **参照のみ** を保持して安定した FNodeId を
// 発行 + stale 検出する。設計は FNodePool と完全に同一 (index 0 予約 / generation 0
// スキップ / 24bit index 上限 / free stack で O(1) 再利用 / 非所有)。
//
// 追加: PurgePendingDestroy() — 登録済みノードのうち Destroy() 済み (IsPendingDestroy)
// のものを一括 Unregister する。FScene3D が ResolveStructuralChanges の «前» に呼ぶことで、
// どの経路で破棄されてもダングリング参照を残さない (self-healing)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/NodeId.h"

namespace acs::game {

class FNode3D;   // forward decl — full include は .cpp 側 (_SetId / IsPendingDestroy 呼出のため)

/**
 * FNode3D 群を pool で管理し、安定した FNodeId を発行 + stale 検出する (FNodePool の 3D 版)。
 *
 * @details
 * FNode3D の所有権は持たない (親の TUniquePtr が所有)。raw ポインタだけを保持し、
 * generational handle (FNodeId) の発行と stale 検出を担う。Slot = {ptr, gen, active}、
 * index 0 は invalid 用に予約、空き slot は free stack で O(1) 再利用する。
 */
class FNode3DPool {
public:
    /** 空の pool を構築する。 */
    FNode3DPool()  noexcept = default;

    /** 破棄する (FNode3D は非所有なので何も delete しない)。 */
    ~FNode3DPool() noexcept = default;

    /** コピー禁止。 */
    FNode3DPool(const FNode3DPool&)            = delete;

    /** コピー代入も禁止。 */
    FNode3DPool& operator=(const FNode3DPool&) = delete;

    /** ムーブ禁止。 */
    FNode3DPool(FNode3DPool&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FNode3DPool& operator=(FNode3DPool&&)      = delete;

    /**
     * 初期容量を予約する (index 0 dummy slot を確保し、配列を reserve する)。
     *
     * @param initial_capacity 予約する slot 数 (0 なら dummy 確保のみ)。
     */
    void Init(u32 initial_capacity = 256) noexcept;

    /**
     * 既存の生 FNode3D を pool に登録し、新しい FNodeId を発行する。
     *
     * @details 発行した FNodeId は node->_SetId() で node 自身にも書き込む。
     * @param node 登録する FNode3D (nullptr / 16M 超過時は登録しない)。
     * @return 発行した FNodeId。失敗時は invalid。
     */
    FNodeId RegisterExistingNode(FNode3D* node) noexcept;

    /**
     * slot を free 化し、対応 FNode3D の Id を invalid にリセットする (二重 Unregister 安全)。
     *
     * @param id 解放する slot の FNodeId。
     */
    void Unregister(FNodeId id) noexcept;

    /**
     * 登録済みノードのうち Destroy() 済み (IsPendingDestroy) のものを一括 Unregister する。
     *
     * @details FScene3D が ResolveStructuralChanges の «前» に呼ぶ。これによりノードが reap
     * (= メモリ解放) される前に pool 参照が外れ、ダングリングを残さない。
     * @return Unregister したノード数。
     */
    u32 PurgePendingDestroy() noexcept;

    /**
     * id が指す slot が active かつ generation 一致かを返す。
     *
     * @param id 検証する FNodeId。
     * @return 生きていて世代も一致すれば true。
     */
    bool IsValid(FNodeId id) const noexcept;

    /**
     * id 経由で FNode3D* を取り出す。
     *
     * @param id 取り出す FNodeId。
     * @return 対応する FNode3D。stale / invalid なら nullptr。
     */
    FNode3D* Get(FNodeId id) const noexcept;

    /**
     * node ポインタから FNodeId を逆引きする (線形探索 O(N))。
     *
     * @param node 逆引きする FNode3D。
     * @return 対応する FNodeId。存在しなければ invalid。
     */
    FNodeId IdOf(FNode3D* node) const noexcept;

    /**
     * 現在 active な slot 数を返す。
     *
     * @return active な slot 数。
     */
    u32 ActiveCount() const noexcept { return m_ActiveCount; }

    /**
     * 全 slot を一括 free する (各 FNode3D の Id を invalid に。gen は維持)。
     */
    void ClearAll() noexcept;

private:
    /** 1 つの slot エントリ (登録された FNode3D の参照と世代)。 */
    struct Slot {
        /** 登録された FNode3D (非所有)。 */
        FNode3D* ptr    = nullptr;

        /** 世代カウンタ (0 は予約 = invalid handle と一致)。 */
        u8      gen    = 0;

        /** この slot が現在使用中か。 */
        bool    active = false;
    };

    /**
     * 空き slot を 1 つ取得する (free stack → 末尾追加)。
     *
     * @return 取得した slot の index。確保不能なら 0。
     */
    u32 AcquireSlot() noexcept;

    /** 24bit index 上限 (= 16,777,215)。 */
    static constexpr u32 kMaxIndex = 0x00FFFFFFu;

    /** slot 配列 (index 0 は dummy = invalid 用予約)。 */
    TArray<Slot> m_Slots;

    /** 空き slot index の LIFO stack。 */
    TArray<u32>  m_FreeIndices;

    /** 現在 active な slot 数。 */
    u32         m_ActiveCount = 0;
};

} // namespace acs::game
