#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <Windows.h>

#include <Core/UseModules.h>

#ifdef ACSM_ECS
#include <modules/ECS/ACSM_ECS_ArchetypeManager.h>
#include <modules/ECS/ACSM_ECS_ComponentManager.h>
#include <modules/ECS/ACSM_ECS_ComponentTypeID.h>
#include <modules/ECS/ACSM_ECS_ECSWorld.h>
#include <modules/ECS/ACSM_ECS_EntityID.h>
#include <modules/ECS/ACSM_ECS_EntityManager.h>
#include <modules/ECS/ACSM_ECS_SystemManager.h>
#include <modules/ECS/ACSM_ECS_IComponentPool.h>
#include <modules/ECS/IACSM_ECS_System.h>
#endif