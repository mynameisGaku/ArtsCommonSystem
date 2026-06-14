// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNode3DPool 実装 (FNodePool の 3D 版)
#include "gameframework/Node3DPool.h"
#include "gameframework/Node3D.h"   // _SetId / IsPendingDestroy を呼ぶため full include

namespace acs::game {

/** 初期容量を予約する (index 0 dummy slot を確保し、配列を reserve する)。 */
void FNode3DPool::Init(u32 initial_capacity) noexcept {
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack(Slot{});   // index 0 = dummy (invalid と衝突回避)
    }
    if (initial_capacity > 0) {
        m_Slots.Reserve(static_cast<usize>(initial_capacity) + 1u);
        m_FreeIndices.Reserve(static_cast<usize>(initial_capacity));
    }
}

/** 空き slot を 1 つ取得する (free stack → 末尾追加、16M 超で 0 を返す)。 */
u32 FNode3DPool::AcquireSlot() noexcept {
    if (!m_FreeIndices.IsEmpty()) {
        const u32 idx = m_FreeIndices.Back();
        m_FreeIndices.PopBack();
        return idx;
    }
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack(Slot{});   // index 0 = dummy
    }
    if (m_Slots.Size() > static_cast<usize>(kMaxIndex)) {
        return 0u;   // 24bit index 上限
    }
    m_Slots.PushBack(Slot{});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

/** FNode3D を登録し、世代を進めた FNodeId を発行して node->_SetId する。 */
FNodeId FNode3DPool::RegisterExistingNode(FNode3D* node) noexcept {
    if (node == nullptr) return FNodeId{};

    const u32 idx = AcquireSlot();
    if (idx == 0u) return FNodeId{};   // 16M 越え → 失敗

    Slot& s = m_Slots[idx];
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0u) s.gen = 1u;        // gen=0 は invalid 表現とぶつかるのでスキップ
    s.ptr    = node;
    s.active = true;
    ++m_ActiveCount;

    const FNodeId new_id{idx, s.gen};
    node->_SetId(new_id);
    return new_id;
}

/** slot を free 化し、対応 FNode3D の Id を invalid に戻す。 */
void FNode3DPool::Unregister(FNodeId id) noexcept {
    if (!id.IsValid()) return;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Slots.Size()) return;

    Slot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Generation()) return;   // stale / 既に free

    if (s.ptr != nullptr) {
        s.ptr->_SetId(FNodeId{});
    }
    s.active = false;
    s.ptr    = nullptr;
    if (m_ActiveCount > 0u) --m_ActiveCount;
    m_FreeIndices.PushBack(idx);
}

/** Destroy 済み (IsPendingDestroy) のノードを一括 Unregister する。 */
u32 FNode3DPool::PurgePendingDestroy() noexcept {
    u32 purged = 0;
    // index 0 は dummy。reap 前なので s.ptr はまだ有効に deref できる。
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        Slot& s = m_Slots[i];
        if (s.active && s.ptr != nullptr && s.ptr->IsPendingDestroy()) {
            s.ptr->_SetId(FNodeId{});
            s.active = false;
            s.ptr    = nullptr;
            if (m_ActiveCount > 0u) --m_ActiveCount;
            m_FreeIndices.PushBack(i);
            ++purged;
        }
    }
    return purged;
}

/** slot が active かつ generation 一致なら true を返す。 */
bool FNode3DPool::IsValid(FNodeId id) const noexcept {
    if (!id.IsValid()) return false;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Slots.Size()) return false;
    const Slot& s = m_Slots[idx];
    return s.active && s.gen == id.Generation();
}

/** id が有効なら対応する FNode3D* を、stale / invalid なら nullptr を返す。 */
FNode3D* FNode3DPool::Get(FNodeId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Slots.Size()) return nullptr;
    const Slot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Generation()) return nullptr;
    return s.ptr;
}

/** node ポインタを線形探索して FNodeId を逆引きする (無ければ invalid)。 */
FNodeId FNode3DPool::IdOf(FNode3D* node) const noexcept {
    if (node == nullptr) return FNodeId{};
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const Slot& s = m_Slots[i];
        if (s.active && s.ptr == node) {
            return FNodeId{i, s.gen};
        }
    }
    return FNodeId{};
}

/** 全 active slot を free 化し、各 FNode3D の Id を invalid に戻す (gen は維持)。 */
void FNode3DPool::ClearAll() noexcept {
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
}

} // namespace acs::game
