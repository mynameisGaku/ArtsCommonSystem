// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/NodeId.h"

namespace acs::game {

class ANode;   // forward decl — full include は .cpp 側 (ANode::_SetId 呼出のため)

/** CNodePool への checked 登録が返す状態。 */
enum class ENodePoolRegisterError : u8 {
    /** 登録に成功。 */
    None = 0,
    /** 入力ノードが null。 */
    NullNode,
    /** 同じ pool に登録済み。 */
    AlreadyRegistered,
    /** 別の pool が発行した有効 Id を保持中。 */
    RegisteredByAnotherPool,
    /** FNodeId が表現できる index 上限を超過。 */
    IndexLimitExceeded,
    /** slot または空き index の保持領域を確保できない。 */
    AllocationFailure,
};

/** checked ノード登録結果。AlreadyRegistered の場合も既存 Id を返す。 */
struct FNodePoolRegisterResult {
    /** 登録済みまたは新規発行したノード Id。 */
    FNodeId Id{};
    /** 登録処理の状態。 */
    ENodePoolRegisterError Error = ENodePoolRegisterError::None;

    bool Succeeded() const noexcept {
        return Error == ENodePoolRegisterError::None && Id.IsValid();
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * ANode 群を pool で管理し、安定した FNodeId を発行 + stale 検出する。
 *
 * @details
 * ANode の所有権は持たない (ANode は親の TObjectPtr が所有)。本 pool は raw
 * ポインタだけを保持し、generational handle (FNodeId) の発行と stale 検出を担う。
 * FSlot = {ptr, gen, active} 構成で、index 0 は invalid 用に予約、有効 slot は 1..N。
 * 空き slot は free stack で O(1) 再利用する。
 */
class CNodePool {
public:
    /** 空の pool を構築する (slot 配列は Init / 初回 Register で確保)。 */
    CNodePool()  noexcept = default;

    /** 破棄する (ANode は非所有なので何も delete しない)。 */
    ~CNodePool() noexcept = default;

    /** コピー禁止 (pool は scene が固定オブジェクトとして単独所有するため)。 */
    CNodePool(const CNodePool&)            = delete;

    /** コピー代入も禁止。 */
    CNodePool& operator=(const CNodePool&) = delete;

    /** ムーブ禁止。 */
    CNodePool(CNodePool&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CNodePool& operator=(CNodePool&&)      = delete;

    /** Exchange complete registry state without allocation. */
    void Swap(CNodePool& other) noexcept {
        acs::Swap(m_Slots, other.m_Slots);
        acs::Swap(m_FreeIndices, other.m_FreeIndices);
        acs::Swap(m_ActiveCount, other.m_ActiveCount);
    }

    /**
     * 初期容量を予約する (再 alloc 回避用)。
     *
     * @details 複数回呼出可、縮小はしない。index 0 の dummy slot を未確保なら確保する。
     * @param initial_capacity 予約する slot 数 (0 なら dummy 確保のみで reserve しない)。
     */
    void Init(u32 initial_capacity = 256) noexcept;

    /**
     * 既存ノードを重複なく登録する。
     *
     * @details
     * 同一 pool ですでに登録済みなら新しい slot を作らず AlreadyRegistered と既存 Id を返す。
     * 別 pool の有効 Id を持つノードは、双方のレジストリを不整合にしないよう拒否する。
     * 確保・index 上限失敗時は node と active slot 数を変更しない。
     */
    FNodePoolRegisterResult TryRegisterExistingNode(ANode* node) noexcept;

    /**
     * 既存の生 ANode を pool に登録し、新しい FNodeId を発行する。
     *
     * @details
     * 互換用の簡易 API。同一 pool ですでに登録済みなら既存 Id を返す。
     * 詳細な失敗理由が必要なら TryRegisterExistingNode を使う。
     * @param node 登録する ANode (nullptr、または slot 数が 16M に達したときは登録しない)。
     * @return 発行した FNodeId。失敗時は invalid (この場合 node の Id は変更しない)。
     */
    FNodeId RegisterExistingNode(ANode* node) noexcept;

    /**
     * slot を free 化し、対応 ANode の Id を invalid にリセットする。
     *
     * @details 既に invalid / stale な id は何もしない (二重 Unregister は安全)。
     * generation は次の TryAcquireSlot で進むため、ここでは進めない。
     * @param id 解放する slot の FNodeId。
     */
    void Unregister(FNodeId id) noexcept;

    /**
     * 登録済みノードのうち Destroy() 済みのものと、その子孫を一括 Unregister する。
     *
     * @details
     * 親のDestroyでは子孫自身のpending flagは立たないが、親の所有参照解放で子孫も
     * 破棄される。各ノードの祖先chainも確認してResolveStructuralChanges前に外し、
     * poolに子孫のダングリング参照が残らないようにする。
     * @return Unregister したノード数。
     */
    u32 PurgePendingDestroy() noexcept;

    /**
     * id が指す slot が active かつ generation が一致するかを返す。
     *
     * @param id 検証する FNodeId。
     * @return slot が生きていて世代も一致すれば true。
     */
    bool IsValid(FNodeId id) const noexcept;

    /**
     * id 経由で ANode* を取り出す。
     *
     * @param id 取り出す FNodeId。
     * @return 対応する ANode。stale / invalid なら nullptr。
     */
    ANode* Get(FNodeId id) const noexcept;

    /**
     * node ポインタから FNodeId を逆引きする (線形探索 O(N))。
     *
     * @param node 逆引きする ANode。
     * @return 対応する FNodeId。node == nullptr または pool に存在しなければ invalid。
     */
    FNodeId IdOf(ANode* node) const noexcept;

    /**
     * 現在 active な slot 数を返す。
     *
     * @return active な slot 数。
     */
    u32 ActiveCount() const noexcept { return m_ActiveCount; }

    /**
     * 現在の slot 数を返す (内部配列長 - 1、index 0 予約分を除外)。
     *
     * @details 物理的に確保された ANode 個数の上限ではなく、過去に到達した最大値の指標。
     * @return index 0 の dummy を除いた slot 数。
     */
    u32 Capacity() const noexcept {
        const u32 sz = static_cast<u32>(m_Slots.Num());
        return sz > 0 ? sz - 1u : 0u;   // index 0 予約分を引く
    }

    /**
     * 全 slot を一括 free する。
     *
     * @details 各登録済 ANode の Id を invalid にリセットし、全物理 slot をfree stackへ
     * ちょうど1回ずつ戻す。ANode 自体は削除しない (非所有)。gen は維持するため、
     * ClearAll 前の handle は再利用後も確実に stale 検出される。
     */
    void ClearAll() noexcept;

private:
    /**
     * 1 つの slot エントリ (登録された ANode の参照と世代)。
     */
    struct FSlot {
        /** 登録された ANode (非所有。所有は親 TObjectPtr 側)。 */
        ANode* ptr    = nullptr;

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
    ENodePoolRegisterError TryAcquireSlot(u32& out_index) noexcept;

    /** 24bit index 上限 (FNodeId pack 仕様に合わせる、= 16,777,215)。 */
    static constexpr u32 kMaxIndex = 0x00FFFFFFu;

    /** slot 配列 (index 0 は dummy = invalid 用予約)。 */
    TArray<FSlot> m_Slots;

    /** 空き slot index の LIFO stack (pop → 再利用、empty → 末尾追加)。 */
    TArray<u32>  m_FreeIndices;

    /** 現在 active な slot 数。 */
    u32         m_ActiveCount = 0;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FNodePool = CNodePool;

} // namespace acs::game
