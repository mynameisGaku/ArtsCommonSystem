// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B Phase 4 — NodePool 実装
//
// ヘッダの設計コメント (NodePool.h) と CollisionWorld2D.cpp の slot+gen pool
// パターンに完全準拠する。所有権を持たないこと、index 0 予約、generation 0
// スキップ、24bit index 上限の 4 点が正しさの肝。
#include "gameframework/NodePool.h"
#include "gameframework/Node2D.h"   // Node2D::_SetId を呼ぶため full include

namespace acs::game {

void NodePool::Init(u32 initial_capacity) noexcept {
    // index 0 は予約 (NodeId{0,0} = invalid と衝突しない dummy slot)。
    // 既に Init 済 (= _slots.Size() > 0) の場合でも追加 reserve だけ行う。
    if (_slots.IsEmpty()) {
        _slots.PushBack(Slot{});   // index 0 = dummy
    }
    if (initial_capacity > 0) {
        // +1 して index 0 分も含めて予約 (再 alloc 回避)。
        _slots.Reserve(static_cast<usize>(initial_capacity) + 1u);
        _free_indices.Reserve(static_cast<usize>(initial_capacity));
    }
}

u32 NodePool::AcquireSlot() noexcept {
    // free stack から再利用 slot を取得
    if (!_free_indices.IsEmpty()) {
        const u32 idx = _free_indices.Back();
        _free_indices.PopBack();
        return idx;
    }
    // 末尾追加。index 0 (dummy) が無ければ先に確保。
    if (_slots.IsEmpty()) {
        _slots.PushBack(Slot{});   // index 0 = dummy
    }
    // 24bit index 上限チェック。次に追加されると Size() == _slots.Size()。
    // 既に kMaxIndex まで埋まっていたら拒否 (= 0 = invalid 表現)。
    if (_slots.Size() > static_cast<usize>(kMaxIndex)) {
        return 0u;
    }
    _slots.PushBack(Slot{});
    return static_cast<u32>(_slots.Size()) - 1u;
}

NodeId NodePool::RegisterExistingNode(Node2D* node) noexcept {
    if (node == nullptr) return NodeId{};

    const u32 idx = AcquireSlot();
    if (idx == 0u) return NodeId{};   // 16M 越え → 失敗 (node の Id は触らない)

    Slot& s = _slots[idx];
    // gen を進める。0 にラップしたら 1 にスキップ (gen=0 は invalid 表現と衝突)。
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0u) s.gen = 1u;
    s.ptr    = node;
    s.active = true;
    ++_active_count;

    const NodeId new_id{idx, s.gen};
    node->_SetId(new_id);
    return new_id;
}

void NodePool::Unregister(NodeId id) noexcept {
    if (!id.IsValid()) return;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= _slots.Size()) return;   // 0 / 範囲外は無視

    Slot& s = _slots[idx];
    if (!s.active || s.gen != id.Generation()) return;   // stale / 既に free

    // Node2D 側の Id を invalid にリセット (ぶら下がった stale handle を node 経由でも検出可能に)。
    if (s.ptr != nullptr) {
        s.ptr->_SetId(NodeId{});
    }

    s.active = false;
    s.ptr    = nullptr;
    // gen は AcquireSlot 側で +1 されるため、ここでは進めない (= 二重インクリメント回避)。
    // 結果として「次に同 slot を再利用した時 gen が必ず変わる」性質は維持される。

    if (_active_count > 0u) --_active_count;
    _free_indices.PushBack(idx);
}

bool NodePool::IsValid(NodeId id) const noexcept {
    if (!id.IsValid()) return false;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= _slots.Size()) return false;
    const Slot& s = _slots[idx];
    return s.active && s.gen == id.Generation();
}

Node2D* NodePool::Get(NodeId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= _slots.Size()) return nullptr;
    const Slot& s = _slots[idx];
    if (!s.active || s.gen != id.Generation()) return nullptr;
    return s.ptr;
}

NodeId NodePool::IdOf(Node2D* node) const noexcept {
    if (node == nullptr) return NodeId{};
    // index 0 は dummy なので 1 から走査。
    for (u32 i = 1; i < _slots.Size(); ++i) {
        const Slot& s = _slots[i];
        if (s.active && s.ptr == node) {
            return NodeId{i, s.gen};
        }
    }
    return NodeId{};
}

void NodePool::ClearAll() noexcept {
    // 全 active slot を free 化、対応 node の Id を invalid に。
    // gen はあえてリセットせず維持する (= 同じ slot を ClearAll 後に再利用した時、
    // 旧 handle が gen 不一致で確実に stale 検出される)。
    for (u32 i = 1; i < _slots.Size(); ++i) {
        Slot& s = _slots[i];
        if (s.active) {
            if (s.ptr != nullptr) {
                s.ptr->_SetId(NodeId{});
            }
            s.active = false;
            s.ptr    = nullptr;
        }
    }
    _free_indices.Clear();
    _active_count = 0u;
    // _slots 自体は保持 (容量再利用、index 0 の dummy も残す)。
}

} // namespace acs::game
