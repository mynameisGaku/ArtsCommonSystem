// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — CNodePool 実装
//
// ヘッダの設計コメント (CNodePool.h) と CCollisionWorld2D.cpp の slot+gen pool
// パターンに完全準拠する。所有権を持たないこと、index 0 予約、generation 0
// スキップ、24bit index 上限の 4 点が正しさの肝。
#include "gameframework/NodePool.h"
#include "gameframework/ANode.h"   // ANode::SetId_Internal / Id を呼ぶため full include

namespace acs::game {

/** 初期容量を予約する (index 0 dummy slot を確保し、配列を reserve する)。 */
void CNodePool::Init(u32 initial_capacity) noexcept {
    // index 0 は予約 (FNodeId{0,0} = invalid と衝突しない dummy slot)。
    // 既に Init 済 (= m_Slots.Size() > 0) の場合でも追加 reserve だけ行う。
    if (m_Slots.IsEmpty()) {
        m_Slots.Add(FSlot{});   // インデックス 0 はダミー
    }
    if (initial_capacity > 0) {
        // +1 して index 0 分も含めて予約 (再 alloc 回避)。
        m_Slots.Reserve(static_cast<usize>(initial_capacity) + 1u);
        m_FreeIndices.Reserve(static_cast<usize>(initial_capacity));
    }
}

/** 空き slot を 1 つ取得する。失敗時は active slot と node を変更しない。 */
ENodePoolRegisterError CNodePool::TryAcquireSlot(u32& out_index) noexcept {
    out_index = 0u;
    // free stack から再利用 slot を取得
    if (!m_FreeIndices.IsEmpty()) {
        out_index = m_FreeIndices.Last();
        m_FreeIndices.Pop();
        return ENodePoolRegisterError::None;
    }
    // 末尾追加。index 0 (dummy) が無ければ先に確保。
    if (m_Slots.IsEmpty()) {
        if (!m_Slots.TryAdd(FSlot{}))
            return ENodePoolRegisterError::AllocationFailure;
    }
    // 24bit index 上限チェック。次に追加されると Size() == m_Slots.Size()。
    // 既に kMaxIndex まで埋まっていたら拒否 (= 0 = invalid 表現)。
    if (m_Slots.Num() > static_cast<usize>(kMaxIndex)) {
        return ENodePoolRegisterError::IndexLimitExceeded;
    }
    if (!m_Slots.TryAdd(FSlot{}))
        return ENodePoolRegisterError::AllocationFailure;
    out_index = static_cast<u32>(m_Slots.Num()) - 1u;
    return ENodePoolRegisterError::None;
}

FNodePoolRegisterResult CNodePool::TryRegisterExistingNode(ANode* node) noexcept {
    if (node == nullptr)
        return FNodePoolRegisterResult{FNodeId{}, ENodePoolRegisterError::NullNode};

    const FNodeId existing = IdOf(node);
    if (existing.IsValid()) {
        // active slotを真実としてnode側の内部Idも自己修復する。
        if (node->Id() != existing) node->SetId_Internal(existing);
        return FNodePoolRegisterResult{
            existing, ENodePoolRegisterError::AlreadyRegistered
        };
    }
    if (node->Id().IsValid()) {
        return FNodePoolRegisterResult{
            FNodeId{}, ENodePoolRegisterError::RegisteredByAnotherPool
        };
    }

    u32 idx = 0u;
    const ENodePoolRegisterError acquire_error = TryAcquireSlot(idx);
    if (acquire_error != ENodePoolRegisterError::None)
        return FNodePoolRegisterResult{FNodeId{}, acquire_error};

    FSlot& s = m_Slots[idx];
    // gen を進める。0 にラップしたら 1 にスキップ (gen=0 は invalid 表現と衝突)。
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0u) s.gen = 1u;
    s.ptr    = node;
    s.active = true;
    ++m_ActiveCount;

    const FNodeId new_id{idx, s.gen};
    node->SetId_Internal(new_id);
    return FNodePoolRegisterResult{new_id, ENodePoolRegisterError::None};
}

/** ANode を登録し、重複時は既存 Id、失敗時は invalid を返す。 */
FNodeId CNodePool::RegisterExistingNode(ANode* node) noexcept {
    const FNodePoolRegisterResult result = TryRegisterExistingNode(node);
    return result.Succeeded() || result.Error == ENodePoolRegisterError::AlreadyRegistered
         ? result.Id : FNodeId{};
}

/** slot を free 化し、対応 ANode の Id を invalid に戻す (stale / 二重解放は安全)。 */
void CNodePool::Unregister(FNodeId id) noexcept {
    if (!id.IsValid()) return;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Slots.Num()) return;   // 0 / 範囲外は無視

    FSlot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Generation()) return;   // stale / 既に free

    // ANode 側の Id を invalid にリセット (ぶら下がった stale handle を node 経由でも検出可能に)。
    if (s.ptr != nullptr) {
        s.ptr->SetId_Internal(FNodeId{});
    }

    s.active = false;
    s.ptr    = nullptr;
    // gen は TryAcquireSlot 後の登録側で +1 されるため、ここでは進めない。
    // 結果として「次に同 slot を再利用した時 gen が必ず変わる」性質は維持される。

    if (m_ActiveCount > 0u) --m_ActiveCount;
    (void)m_FreeIndices.TryAdd(idx);
}

/** Destroy 済みのノードを、ツリーから解放される前に一括 Unregister する。 */
u32 CNodePool::PurgePendingDestroy() noexcept {
    u32 purged = 0u;
    for (u32 i = 1; i < m_Slots.Num(); ++i) {
        FSlot& s = m_Slots[i];
        if (!s.active || s.ptr == nullptr) continue;

        bool pending_subtree = false;
        const ANode* ancestor = s.ptr;
        u32 depth = 0u;
        while (ancestor != nullptr && depth <= kNodeMaxTreeDepth) {
            if (ancestor->IsPendingDestroy()) {
                pending_subtree = true;
                break;
            }
            ancestor = ancestor->Parent();
            ++depth;
        }
        // 公開構造APIの不変条件を外れた祖先chainも、reap前の安全側としてpurgeする。
        if (ancestor != nullptr && depth > kNodeMaxTreeDepth) pending_subtree = true;

        if (pending_subtree) {
            s.ptr->SetId_Internal(FNodeId{});
            s.active = false;
            s.ptr    = nullptr;
            if (m_ActiveCount > 0u) --m_ActiveCount;
            (void)m_FreeIndices.TryAdd(i);
            ++purged;
        }
    }
    return purged;
}

/** slot が active かつ generation 一致なら true を返す。 */
bool CNodePool::IsValid(FNodeId id) const noexcept {
    if (!id.IsValid()) return false;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Slots.Num()) return false;
    const FSlot& s = m_Slots[idx];
    return s.active && s.ptr != nullptr && s.gen == id.Generation();
}

/** id が有効なら対応する ANode* を、stale / invalid なら nullptr を返す。 */
ANode* CNodePool::Get(FNodeId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Slots.Num()) return nullptr;
    const FSlot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Generation()) return nullptr;
    return s.ptr;
}

/** node ポインタを線形探索して FNodeId を逆引きする (無ければ invalid)。 */
FNodeId CNodePool::IdOf(ANode* node) const noexcept {
    if (node == nullptr) return FNodeId{};
    // index 0 は dummy なので 1 から走査。
    for (u32 i = 1; i < m_Slots.Num(); ++i) {
        const FSlot& s = m_Slots[i];
        if (s.active && s.ptr == node) {
            return FNodeId{i, s.gen};
        }
    }
    return FNodeId{};
}

/** 全 active slot を free 化し、各 ANode の Id を invalid に戻す (gen は維持)。 */
void CNodePool::ClearAll() noexcept {
    const usize reusable_count = m_Slots.Num() > 0u ? m_Slots.Num() - 1u : 0u;
    const bool can_rebuild_free_list = m_FreeIndices.TryReserve(reusable_count);
    // 全 active slot を free 化、対応 node の Id を invalid に。
    // gen はあえてリセットせず維持する (= 同じ slot を ClearAll 後に再利用した時、
    // 旧 handle が gen 不一致で確実に stale 検出される)。
    for (u32 i = 1; i < m_Slots.Num(); ++i) {
        FSlot& s = m_Slots[i];
        if (s.active) {
            if (s.ptr != nullptr) {
                s.ptr->SetId_Internal(FNodeId{});
            }
            s.active = false;
            s.ptr    = nullptr;
        }
    }
    if (can_rebuild_free_list) {
        // 全物理 slot をちょうど1回ずつ free list へ戻す。
        m_FreeIndices.Reset();
        for (u32 i = 1u; i < m_Slots.Num(); ++i) {
            (void)m_FreeIndices.TryAdd(i); // 事前 Reserve 済みなので失敗しない
        }
    }
    m_ActiveCount = 0u;
    // m_Slots 自体は保持 (容量再利用、index 0 の dummy も残す)。
}

} // namespace acs::game
