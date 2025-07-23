#include "ArchetypeManager.h"
#include <algorithm>

void ArchetypeManager::AssignArchetype(EntityID entity, std::vector<uint32_t> typeIDs)
{
    std::sort(typeIDs.begin(), typeIDs.end());  // àÍä—ê´ÇÃÇ†ÇÈÉLÅ[ê∂ê¨

    auto it = m_Archetypes.find(typeIDs);
    if (it == m_Archetypes.end())
    {
        m_Archetypes[typeIDs] = std::make_unique<Archetype>();
    }

    m_Archetypes[typeIDs]->entities.push_back(entity);
    m_EntityToArchetype[entity.index] = typeIDs;
}

std::vector<EntityID> ArchetypeManager::QueryEntities(const std::vector<uint32_t>& typeIDs) const
{
    std::vector<EntityID> result;

    for (const auto& pair : m_Archetypes)
    {
        const auto& key = pair.first;
        const auto& arch = pair.second;

        bool match = std::all_of(typeIDs.begin(), typeIDs.end(), [&](uint32_t id)
            {
                return std::binary_search(key.begin(), key.end(), id);
            });

        if (match)
        {
            result.insert(result.end(), arch->entities.begin(), arch->entities.end());
        }
    }

    return result;
}

std::vector<uint32_t> ArchetypeManager::GetComponentTypes(EntityID entity) const
{
    auto it = m_EntityToArchetype.find(entity.index);
    if (it != m_EntityToArchetype.end())
    {
        return it->second;
    }
    return {};
}

void ArchetypeManager::RemoveEntity(EntityID entity)
{
    auto it = m_EntityToArchetype.find(entity.index);
    if (it == m_EntityToArchetype.end()) return;

    const auto& key = it->second;
    auto archIt = m_Archetypes.find(key);
    if (archIt != m_Archetypes.end())
    {
        auto& vec = archIt->second->entities;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
            [&](const EntityID& e) { return e.index == entity.index; }),
            vec.end());
    }

    m_EntityToArchetype.erase(it);
}
