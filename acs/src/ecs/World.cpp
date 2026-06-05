// SPDX-License-Identifier: Apache-2.0
// ECS World 実装
#include "ecs/World.h"

namespace acs {

/** 空の World を構築する。 */
World::World() noexcept = default;

/** World を破棄し、所有する全 SparseSet を解放する。 */
World::~World() noexcept {
    // 全 SparseSet を解放（仮想デストラクタ経由で型ごとの破棄を実行）
    for (usize i = 0; i < m_Sets.Size(); ++i) {
        if (m_Sets[i]) {
            ::delete m_Sets[i];
            m_Sets[i] = nullptr;
        }
    }
}

/** エンティティを生成する (フリースロット再利用、無ければ新規確保)。 */
EntityId World::Create() noexcept {
    EntityId e{};
    if (!m_FreeIndices.IsEmpty()) {
        // フリースロットを再利用（生成回数で世代がインクリメントされる）
        const u32 idx = m_FreeIndices.Back();
        m_FreeIndices.PopBack();
        Slot& s = m_Slots[idx];
        s.alive = true;
        e.index = idx;
        e.generation = s.generation;
    } else {
        // 新規スロット作成
        const u32 idx = static_cast<u32>(m_Slots.Size());
        m_Slots.PushBack({0, true});
        e.index = idx;
        e.generation = 0;
    }
    ++m_AliveCount;
    return e;
}

/** エンティティの全コンポーネントを除去し、世代を進めてスロットを再利用待ちへ戻す。 */
void World::Destroy(EntityId e) noexcept {
    if (!IsAlive(e)) return;
    // 全コンポーネントを取り除く（型消去 Remove で各 SparseSet に通知）
    for (usize i = 0; i < m_Sets.Size(); ++i) {
        if (m_Sets[i]) m_Sets[i]->RemoveErased(e.index);
    }
    Slot& s = m_Slots[e.index];
    s.alive = false;
    ++s.generation;  // 世代を進めて古い EntityId を無効化
    m_FreeIndices.PushBack(e.index);
    --m_AliveCount;
}

/** エンティティが生存中かつ世代一致かを返す。 */
bool World::IsAlive(EntityId e) const noexcept {
    if (!e.IsValid()) return false;
    if (e.index >= m_Slots.Size()) return false;
    const Slot& s = m_Slots[e.index];
    return s.alive && s.generation == e.generation;
}

} // namespace acs
