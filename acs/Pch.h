#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#pragma comment(lib, "xinput.lib")

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
#include <Xinput.h>
#include <set>
#include <hidsdi.h>
#include <hidusage.h>
#include <tchar.h>
#include <fstream>
#include <unordered_set>

#include <extension/ImGui/imgui.h>
#include <extension/magic_enum/magic_enum.hpp>
#include <extension/nlohmann/json.hpp>
#include <extension/nlohmann/json_fwd.hpp>
#include <extension/uuid4/uuid4.h>

#include <Core/UseModules.h>

#ifdef ACSM_ECS
#include <modules/ecs/seecs.h>
#endif

#ifdef ACSM_INPUT
#include <modules/input/JoyshockWrapper.h>
#include <modules/input/KeyWrapper.h>
#endif