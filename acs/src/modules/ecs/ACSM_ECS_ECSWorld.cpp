#include "ACSM_ECS_ECSWorld.h"

ACSM_ECS_EntityID ACSM_ECS_ECSWorld::CreateEntity()
{
    return m_EntityManager.CreateEntity();
}

void ACSM_ECS_ECSWorld::DestroyEntity(ACSM_ECS_EntityID id)
{
    m_EntityManager.DestroyEntity(id);
    m_ArchetypeManager.RemoveEntity(id);
}

void ACSM_ECS_ECSWorld::RegisterSystem(std::function<void()> func)
{
    m_SystemManager.RegisterSystem(func);
}

void ACSM_ECS_ECSWorld::RegisterSystem(std::shared_ptr<IACSM_ECS_System> system)
{
    m_SystemManager.RegisterSystem(system);
}

void ACSM_ECS_ECSWorld::Update()
{
    m_SystemManager.UpdateAll();
}

std::vector<uint32_t> ACSM_ECS_ECSWorld::GetComponentTypes(ACSM_ECS_EntityID id) const
{
    return m_ArchetypeManager.GetComponentTypes(id);
}

std::vector<ACSM_ECS_EntityID> ACSM_ECS_ECSWorld::QueryEntities(const std::vector<uint32_t>& typeIDs) const
{
    return m_ArchetypeManager.QueryEntities(typeIDs);
}

int ACSM_ECS_ECSWorld::GetEntityCount() const
{
    return static_cast<int>(m_EntityManager.GetAliveCount());
}