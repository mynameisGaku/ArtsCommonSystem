// SPDX-License-Identifier: Apache-2.0
// ECS World 実装
#include "ecs/World.h"

namespace acs {

World::World() noexcept = default;

World::~World() noexcept {
    // 全 SparseSet を解放（仮想デストラクタ経由で型ごとの破棄を実行）
    for (usize i = 0; i < _sets.Size(); ++i) {
        if (_sets[i]) {
            ::delete _sets[i];
            _sets[i] = nullptr;
        }
    }
}

EntityId World::Create() noexcept {
    EntityId e{};
    if (!_free_indices.IsEmpty()) {
        // フリースロットを再利用（生成回数で世代がインクリメントされる）
        u32 idx = _free_indices.Back();
        _free_indices.PopBack();
        Slot& s = _slots[idx];
        s.alive = true;
        e.index = idx;
        e.generation = s.generation;
    } else {
        // 新規スロット作成
        u32 idx = static_cast<u32>(_slots.Size());
        _slots.PushBack({0, true});
        e.index = idx;
        e.generation = 0;
    }
    ++_alive_count;
    return e;
}

void World::Destroy(EntityId e) noexcept {
    if (!IsAlive(e)) return;
    // 全コンポーネントを取り除く（型消去 Remove で各 SparseSet に通知）
    for (usize i = 0; i < _sets.Size(); ++i) {
        if (_sets[i]) _sets[i]->RemoveErased(e.index);
    }
    Slot& s = _slots[e.index];
    s.alive = false;
    ++s.generation;  // 世代を進めて古い EntityId を無効化
    _free_indices.PushBack(e.index);
    --_alive_count;
}

bool World::IsAlive(EntityId e) const noexcept {
    if (!e.IsValid()) return false;
    if (e.index >= _slots.Size()) return false;
    const Slot& s = _slots[e.index];
    return s.alive && s.generation == e.generation;
}

} // namespace acs
