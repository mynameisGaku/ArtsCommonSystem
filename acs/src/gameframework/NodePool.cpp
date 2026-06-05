// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNodePool 実装
//
// ヘッダの設計コメント (FNodePool.h) と FCollisionWorld2D.cpp の slot+gen pool
// パターンに完全準拠する。所有権を持たないこと、index 0 予約、generation 0
// スキップ、24bit index 上限の 4 点が正しさの肝。
#include "gameframework/NodePool.h"
#include "gameframework/Node2D.h"   // Node2D::_SetId を呼ぶため full include

namespace acs::game {

/** 初期容量を予約する (index 0 dummy slot を確保し、配列を reserve する)。 */
void FNodePool::Init(u32 initial_capacity) noexcept {
    // index 0 は予約 (FNodeId{0,0} = invalid と衝突しない dummy slot)。
    // 既に Init 済 (= m_Slots.Size() > 0) の場合でも追加 reserve だけ行う。
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack(Slot{});   // index 0 = dummy
    }
    if (initial_capacity > 0) {
        // +1 して index 0 分も含めて予約 (再 alloc 回避)。
        m_Slots.Reserve(static_cast<usize>(initial_capacity) + 1u);
        m_FreeIndices.Reserve(static_cast<usize>(initial_capacity));
    }
}

/** 空き slot を 1 つ取得する (free stack → 末尾追加、16M 超で 0 を返す)。 */
u32 FNodePool::AcquireSlot() noexcept {
    // free stack から再利用 slot を取得
    if (!m_FreeIndices.IsEmpty()) {
        const u32 idx = m_FreeIndices.Back();
        m_FreeIndices.PopBack();
        return idx;
    }
    // 末尾追加。index 0 (dummy) が無ければ先に確保。
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack(Slot{});   // index 0 = dummy
    }
    // 24bit index 上限チェック。次に追加されると Size() == m_Slots.Size()。
    // 既に kMaxIndex まで埋まっていたら拒否 (= 0 = invalid 表現)。
    if (m_Slots.Size() > static_cast<usize>(kMaxIndex)) {
        return 0u;
    }
    m_Slots.PushBack(Slot{});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

/** FNode2D を登録し、世代を進めた FNodeId を発行して node->_SetId する。 */
FNodeId FNodePool::RegisterExistingNode(FNode2D* node) noexcept {
    if (node == nullptr) return FNodeId{};

    const u32 idx = AcquireSlot();
    if (idx == 0u) return FNodeId{};   // 16M 越え → 失敗 (node の Id は触らない)

    Slot& s = m_Slots[idx];
    // gen を進める。0 にラップしたら 1 にスキップ (gen=0 は invalid 表現と衝突)。
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0u) s.gen = 1u;
    s.ptr    = node;
    s.active = true;
    ++m_ActiveCount;

    const FNodeId new_id{idx, s.gen};
    node->_SetId(new_id);
    return new_id;
}

/** slot を free 化し、対応 FNode2D の Id を invalid に戻す (stale / 二重解放は安全)。 */
void FNodePool::Unregister(FNodeId id) noexcept {
    if (!id.IsValid()) return;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Slots.Size()) return;   // 0 / 範囲外は無視

    Slot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Generation()) return;   // stale / 既に free

    // FNode2D 側の Id を invalid にリセット (ぶら下がった stale handle を node 経由でも検出可能に)。
    if (s.ptr != nullptr) {
        s.ptr->_SetId(FNodeId{});
    }

    s.active = false;
    s.ptr    = nullptr;
    // gen は AcquireSlot 側で +1 されるため、ここでは進めない (= 二重インクリメント回避)。
    // 結果として「次に同 slot を再利用した時 gen が必ず変わる」性質は維持される。

    if (m_ActiveCount > 0u) --m_ActiveCount;
    m_FreeIndices.PushBack(idx);
}

/** slot が active かつ generation 一致なら true を返す。 */
bool FNodePool::IsValid(FNodeId id) const noexcept {
    if (!id.IsValid()) return false;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Slots.Size()) return false;
    const Slot& s = m_Slots[idx];
    return s.active && s.gen == id.Generation();
}

/** id が有効なら対応する FNode2D* を、stale / invalid なら nullptr を返す。 */
FNode2D* FNodePool::Get(FNodeId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Slots.Size()) return nullptr;
    const Slot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Generation()) return nullptr;
    return s.ptr;
}

/** node ポインタを線形探索して FNodeId を逆引きする (無ければ invalid)。 */
FNodeId FNodePool::IdOf(FNode2D* node) const noexcept {
    if (node == nullptr) return FNodeId{};
    // index 0 は dummy なので 1 から走査。
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const Slot& s = m_Slots[i];
        if (s.active && s.ptr == node) {
            return FNodeId{i, s.gen};
        }
    }
    return FNodeId{};
}

/** 全 active slot を free 化し、各 FNode2D の Id を invalid に戻す (gen は維持)。 */
void FNodePool::ClearAll() noexcept {
    // 全 active slot を free 化、対応 node の Id を invalid に。
    // gen はあえてリセットせず維持する (= 同じ slot を ClearAll 後に再利用した時、
    // 旧 handle が gen 不一致で確実に stale 検出される)。
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        Slot& s = m_Slots[i];
        if (s.active) {
            if (s.ptr != nullptr) {
                s.ptr->_SetId(FNodeId{});
            }
            s.active = false;
            s.ptr    = nullptr;
        }
    }
    m_FreeIndices.Clear();
    m_ActiveCount = 0u;
    // m_Slots 自体は保持 (容量再利用、index 0 の dummy も残す)。
}

} // namespace acs::game
