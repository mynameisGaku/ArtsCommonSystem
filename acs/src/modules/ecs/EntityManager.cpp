#include "EntityManager.h"

EntityManager::EntityManager()
{
}

EntityID EntityManager::CreateEntity()
{
    uint32_t index;

    // 空きインデックスがあれば再利用、なければ新規発行
    if (!m_FreeIndices.empty())
    {
        index = m_FreeIndices.front();
        m_FreeIndices.pop();
    }
    else
    {
        index = static_cast<uint32_t>(m_Generations.size());
        m_Generations.push_back(0);  // 初期世代として0を追加
    }

    // 世代を進める（0 → 1 など）
    ++m_Generations[index];
    return EntityID{ index, m_Generations[index] };
}

void EntityManager::DestroyEntity(EntityID id)
{
    // 範囲外 or 世代不一致のEntityは無視
    if (id.index >= m_Generations.size()) return;
    if (m_Generations[id.index] != id.generation) return;

    // 世代を進めることでIsAlive()が無効と判定するようになる
    ++m_Generations[id.index];
    m_FreeIndices.push(id.index);  // 再利用可能に登録
}

bool EntityManager::IsAlive(EntityID id) const
{
    // 有効範囲内かつ、世代が一致していれば生きているとみなす
    return id.index < m_Generations.size()
        && m_Generations[id.index] == id.generation;
}

void EntityManager::Clear()
{
    // すべてのEntityを初期化してクリア
    m_Generations.clear();
    m_FreeIndices = std::queue<uint32_t>{};
}

size_t EntityManager::GetAliveCount() const
{
    // 生きているEntity数 = 総数 - 再利用待ち
    return m_Generations.size() - m_FreeIndices.size();
}
