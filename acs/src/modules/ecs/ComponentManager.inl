#pragma once
#include <unordered_map>
#include <vector>

///<summary>
/// コンポーネントを格納するSoA型テンプレートストレージ
///</summary>
template<typename T>
class ComponentPool : public IComponentPool
{
public:
    void Add(EntityID id, const T& component)
    {
        if (m_EntityToIndex.count(id.index) == 0)
        {
            size_t newIndex = m_Components.size();
            m_Components.push_back(component);
            m_EntityToIndex[id.index] = newIndex;
            m_IndexToEntity[newIndex] = id.index;
        }
    }

    T* Get(EntityID id)
    {
        auto it = m_EntityToIndex.find(id.index);
        if (it != m_EntityToIndex.end())
        {
            return &m_Components[it->second];
        }
        return nullptr;
    }

    void Remove(EntityID id) override
    {
        auto it = m_EntityToIndex.find(id.index);
        if (it != m_EntityToIndex.end())
        {
            size_t index = it->second;
            size_t last = m_Components.size() - 1;

            if (index != last)
            {
                m_Components[index] = std::move(m_Components[last]);
                uint32_t movedEntity = m_IndexToEntity[last];
                m_EntityToIndex[movedEntity] = index;
                m_IndexToEntity[index] = movedEntity;
            }

            m_Components.pop_back();
            m_EntityToIndex.erase(id.index);
            m_IndexToEntity.erase(last);
        }
    }

    size_t GetCount() const override
    {
        return m_Components.size();
    }

private:
    std::vector<T> m_Components;                       // コンポーネントの実体
    std::unordered_map<uint32_t, size_t> m_EntityToIndex;  // EntityID.index -> 配列Index
    std::unordered_map<size_t, uint32_t> m_IndexToEntity;  // 配列Index -> EntityID.index
};

template<typename T>
void ComponentManager::AddComponent(EntityID entity, const T& component)
{
    uint32_t typeID = ComponentTypeID::Get<T>();
    if (m_Pools.count(typeID) == 0)
    {
        m_Pools[typeID] = std::make_unique<ComponentPool<T>>();
    }

    auto* pool = static_cast<ComponentPool<T>*>(m_Pools[typeID].get());
    pool->Add(entity, component);
}

template<typename T>
T* ComponentManager::GetComponent(EntityID entity)
{
    uint32_t typeID = ComponentTypeID::Get<T>();
    auto it = m_Pools.find(typeID);
    if (it == m_Pools.end()) return nullptr;

    auto* pool = static_cast<ComponentPool<T>*>(it->second.get());
    return pool->Get(entity);
}

template<typename T>
void ComponentManager::RemoveComponent(EntityID entity)
{
    uint32_t typeID = ComponentTypeID::Get<T>();
    auto it = m_Pools.find(typeID);
    if (it != m_Pools.end())
    {
        auto* pool = static_cast<ComponentPool<T>*>(it->second.get());
        pool->Remove(entity);
    }
}

template<typename T>
size_t ComponentManager::GetComponentCount() const
{
    uint32_t typeID = ComponentTypeID::Get<T>();
    auto it = m_Pools.find(typeID);
    if (it != m_Pools.end())
    {
        auto* pool = static_cast<ComponentPool<T>*>(it->second.get());
        return pool->GetCount();
    }
    return 0;
}
