#pragma once

#include "ACSM_ECS_ECSWorld.h"

template<typename... Components>
void ACSM_ECS_ECSWorld::AddComponents(ACSM_ECS_EntityID id, Components&&... components)
{
    std::vector<uint32_t> typeIDs;

    // “WŠJFAddComponent & typeIDæ“¾
    (void)std::initializer_list<int>{
        (m_ComponentManager.AddComponent(id, std::forward<Components>(components)),
            typeIDs.push_back(ACSM_ECS_ComponentTypeID::Get<Components>()), 0)...
    };

    std::sort(typeIDs.begin(), typeIDs.end()); // ArchetypeKey‚Æ‚µ‚ÄˆÀ’è‰»

    m_ArchetypeManager.AssignArchetype(id, typeIDs);
}

template<typename T>
T* ACSM_ECS_ECSWorld::GetComponent(ACSM_ECS_EntityID id)
{
    return m_ComponentManager.GetComponent<T>(id);
}
