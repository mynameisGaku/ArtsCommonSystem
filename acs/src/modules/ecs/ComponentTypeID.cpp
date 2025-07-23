#include "ComponentTypeID.h"

std::unordered_map<std::type_index, uint32_t> ComponentTypeID::s_TypeToIDMap;
uint32_t ComponentTypeID::s_NextID = 0;
std::mutex ComponentTypeID::s_Mutex;

uint32_t ComponentTypeID::GetCount()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    return static_cast<uint32_t>(s_TypeToIDMap.size());
}
