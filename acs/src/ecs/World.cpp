// SPDX-License-Identifier: Apache-2.0
// ECS World 実装
#include "ecs/World.h"
#include "memory/Memory.h"   // MemCopy (CopyFrom の POD 配列コピー)

namespace acs {

namespace {

/**
 * POD 要素の TArray を丸ごとコピーする (CopyFrom の実装用)。
 *
 * @tparam T トリビアルコピー可能な要素型。
 * @param dst コピー先 (サイズは src に合わせて変更される)。
 * @param src コピー元。
 * @return 確保に成功したら true。
 */
template<typename T>
bool CopyPodArray(TArray<T>& dst, const TArray<T>& src) noexcept
{
    static_assert(IsTriviallyCopyableV<T>, "CopyPodArray: T must be trivially copyable");
    if (!dst.TryResize(src.Size())) return false;
    if (src.Size() > 0) {
        MemCopy(dst.Data(), src.Data(), src.Size() * sizeof(T));
    }
    return true;
}

} // namespace

/** 空の World を構築する。 */
World::World() noexcept = default;

/** World を破棄し、所有する全 SparseSet を解放する。 */
World::~World() noexcept {
    Clear();
}

/** 全エンティティとコンポーネントストレージを解放して初期状態へ戻す。 */
void World::Clear() noexcept
{
    // 全 SparseSet を解放（Delete が仮想デストラクタ経由で型ごとの破棄を実行し、確保元
    // アロケータへ返す）。生 delete を避け、GetOrCreateSet の New と対で MemorySystem 追跡する。
    for (usize i = 0; i < m_Sets.Size(); ++i) {
        if (m_Sets[i]) {
            Delete(*m_Sets.GetAllocator(), m_Sets[i]);
            m_Sets[i] = nullptr;
        }
    }

    // Clear() だけでは容量を保持するため、終了処理ではバッファ自体も返す。
    m_Sets = TArray<SparseSetBase*>{*m_Sets.GetAllocator()};
    m_Slots = TArray<Slot>{*m_Slots.GetAllocator()};
    m_FreeIndices = TArray<u32>{*m_FreeIndices.GetAllocator()};
    m_AliveCount = 0;
    ++m_GenerationSeed;
}

/** src の完全複製をこの World に作る (詳細な契約は World.h 参照)。 */
bool World::CopyFrom(const World& src) noexcept
{
    if (this == &src) return true;
    Clear();

    // エンティティスロット (世代含む)・フリーリスト・カウンタをそのままコピーする。
    // 世代を保つことで snapshot 時の EntityId が復元後も有効になる。
    if (!CopyPodArray(m_Slots, src.m_Slots) || !CopyPodArray(m_FreeIndices, src.m_FreeIndices)) {
        Clear();
        return false;
    }
    m_AliveCount     = src.m_AliveCount;
    m_GenerationSeed = src.m_GenerationSeed;

    // 各 SparseSet を型消去クローンで複製する (非コピー型 or OOM は全体を失敗させる)。
    if (!m_Sets.TryResize(src.m_Sets.Size())) {
        Clear();
        return false;
    }
    for (usize i = 0; i < src.m_Sets.Size(); ++i) {
        m_Sets[i] = nullptr;
        if (src.m_Sets[i] != nullptr) {
            m_Sets[i] = src.m_Sets[i]->CloneErased(*m_Sets.GetAllocator());
            if (m_Sets[i] == nullptr) {
                Clear();   // 部分複製は残さない (複製済みの set もここで解放される)
                return false;
            }
        }
    }
    return true;
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
        m_Slots.PushBack({m_GenerationSeed, true});
        e.index = idx;
        e.generation = m_GenerationSeed;
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
