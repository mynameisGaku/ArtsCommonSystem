#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX


#include <array>
#include <vector>
#include <unordered_map>
#include <limits>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <bitset>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <functional>
#include <typeinfo>
#include <future>
#include <mutex>
#include <queue>
#include <Windows.h>

#include <Core/UseModules.h>

#ifdef ACSM_ECS
#include <modules/ecs/seecs.h>
#endif