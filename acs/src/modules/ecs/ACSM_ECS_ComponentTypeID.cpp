#include "ACSM_ECS_ComponentTypeID.h"

std::unordered_map<std::type_index, uint32_t> ACSM_ECS_ComponentTypeID::s_TypeToIDMap;
uint32_t ACSM_ECS_ComponentTypeID::s_NextID = 0;
std::mutex ACSM_ECS_ComponentTypeID::s_Mutex;

uint32_t ACSM_ECS_ComponentTypeID::GetCount()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    return static_cast<uint32_t>(s_TypeToIDMap.size());
}