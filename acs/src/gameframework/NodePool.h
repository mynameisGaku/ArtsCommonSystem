// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNodePool (ANode の generational pool)
//
// シーン全体で唯一の `ANode*` レジストリ。`ANode` インスタンス自体は親の
// `m_Children` (TObjectPtr<ANode>) が所有し続け、本 pool は **参照のみ** を
// 保持して安定した `FNodeId` を発行する。同時に発行済 `FNodeId` の **stale 検出**
// (= 既に Unregister されたハンドル) を提供する。
//
// 使い方:
//   FNodePool pool;
//   pool.Init(/*initial_capacity=*/512);
//
//   // ANode を新規生成して scene tree に attach した直後に登録:
//   ANode& enemy = scene.Root().AddChild(NewObject<EnemyNode>());
//   FNodeId enemy_id = pool.RegisterExistingNode(&enemy);
//   // enemy.Id() == enemy_id が成立する (RegisterExistingNode 内で _SetId 済)
//
//   // 後で stale 検査付きで取り出し:
//   if (ANode* p = pool.Get(enemy_id)) {
//       p->SetPosition2D(p->Position2D() + FVec2{1, 0});
//   }
//
//   // 破棄時:
//   pool.Unregister(enemy_id);   // slot 解放 + gen++ → 古い handle は invalid 化
//   enemy.Destroy();              // scene tree からは別途 reap される
//
// 設計選択 (Pillar B):
//   ・**non-owning**: 所有権は ANode の親 (=TObjectPtr<ANode>) 側にあり、本 pool
//     は raw ポインタだけ持つ。TPool の破棄や ClearAll は ANode を delete しない。
//   ・**FSlot = {ptr, gen, active}**: FCollisionWorld2D::FSlot と同じパターン。
//     index 0 は予約 (= invalid handle と一致させる)、有効 slot は 1..N。
//   ・**free_indices stack**: 空き slot を O(1) で再利用。Unregister 時 push、
//     TryRegisterExistingNode 時 pop。stack が空なら slot を新規 TryPushBack。
//   ・**generation 0 はスキップ**: gen++ がラップアラウンドで 0 に戻った場合、
//     FNodeId(idx, 0) は IsValid() == false になってしまうため、ラップ時は 1 に
//     強制する (FCollisionWorld2D と完全に同じ挙動)。
//   ・**24bit index = 16,777,216 slot 上限**: FNodeId の pack 仕様に従い、これを
//     超える RegisterExistingNode は invalid FNodeId を返す (拒否)。実用上 1 シーン
//     で 16M ANode を生成することはまずあり得ないが安全策として明示拒否。
//   ・**IdOf は線形探索**: ポインタ → FNodeId の逆引きは利用頻度が低い (基本は
//     RegisterExistingNode の戻り値を保持する) ため、専用 hash は持たない。
//     真に必要なら呼び出し側で THashMap<ANode*, FNodeId> を別途持てば良い。
//   ・**非コピー・非ムーブ**: pool 自体は固定オブジェクトとして scene が所有する想定。
//   ・**全 noexcept / STL 不使用 / `<string>` 禁止**: ACS 規約に厳格準拠。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/NodeId.h"

namespace acs::game {

class ANode;   // forward decl — full include は .cpp 側 (ANode::_SetId 呼出のため)

/** FNodePool への checked 登録が返す状態。 */
enum class ENodePoolRegisterError : u8 {
    None = 0,
    NullNode,
    AlreadyRegistered,
    RegisteredByAnotherPool,
    IndexLimitExceeded,
    AllocationFailure,
};

/** checked ノード登録結果。AlreadyRegistered の場合も既存 Id を返す。 */
struct FNodePoolRegisterResult {
    FNodeId Id{};
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
class FNodePool {
public:
    /** 空の pool を構築する (slot 配列は Init / 初回 Register で確保)。 */
    FNodePool()  noexcept = default;

    /** 破棄する (ANode は非所有なので何も delete しない)。 */
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
        const u32 sz = static_cast<u32>(m_Slots.Size());
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

} // namespace acs::game
