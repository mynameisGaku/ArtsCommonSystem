#pragma once
#include <type_traits>

template<typename T>
uint32_t ComponentTypeID::Get()
{
    static_assert(!std::is_pointer<T>::value, "T must not be a pointer type.");

    std::lock_guard<std::mutex> lock(s_Mutex);
    std::type_index typeIdx(typeid(T));

    auto it = s_TypeToIDMap.find(typeIdx);
    if (it != s_TypeToIDMap.end())
    {
        return it->second;
    }

    const uint32_t id = s_NextID++;
    s_TypeToIDMap[typeIdx] = id;
    return id;
}
