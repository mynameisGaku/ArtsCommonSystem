#include "ECSWorld.h"

EntityID ECSWorld::CreateEntity()
{
    return m_EntityManager.CreateEntity();
}

void ECSWorld::DestroyEntity(EntityID id)
{
    m_EntityManager.DestroyEntity(id);
    m_ArchetypeManager.RemoveEntity(id);
}

void ECSWorld::RegisterSystem(std::function<void()> func)
{
    m_SystemManager.RegisterSystem(func);
}

void ECSWorld::RegisterSystem(std::shared_ptr<ISystem> system)
{
    m_SystemManager.RegisterSystem(system);
}

void ECSWorld::Update()
{
    m_SystemManager.UpdateAll();
}

std::vector<uint32_t> ECSWorld::GetComponentTypes(EntityID id) const
{
    return m_ArchetypeManager.GetComponentTypes(id);
}

std::vector<EntityID> ECSWorld::QueryEntities(const std::vector<uint32_t>& typeIDs) const
{
    return m_ArchetypeManager.QueryEntities(typeIDs);
}

int ECSWorld::GetEntityCount() const
{
    return static_cast<int>(m_EntityManager.GetAliveCount());
}