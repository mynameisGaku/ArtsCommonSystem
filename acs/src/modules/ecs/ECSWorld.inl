#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>

template<typename... Components>
void ECSWorld::AddComponents(EntityID id, Components&&... components)
{
    std::vector<uint32_t> typeIDs;

    // “WŠJFAddComponent & typeIDæ“¾
    (void)std::initializer_list<int>{
        (m_ComponentManager.AddComponent(id, std::forward<Components>(components)),
            typeIDs.push_back(ComponentTypeID::Get<Components>()), 0)...
    };

    std::sort(typeIDs.begin(), typeIDs.end()); // ArchetypeKey‚Æ‚µ‚ÄˆÀ’è‰»

    m_ArchetypeManager.AssignArchetype(id, typeIDs);
}

template<typename T>
T* ECSWorld::GetComponent(EntityID id)
{
    return m_ComponentManager.GetComponent<T>(id);
}
