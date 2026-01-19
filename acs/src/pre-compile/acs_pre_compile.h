#pragma once

#include <extension/ImGui/imgui.h>
#include <extension/ImGui/imgui_impl_dxlib.hpp>
#include <extension/magic_enum/magic_enum.hpp>
#include <extension/nlohmann/json.hpp>
#include <extension/nlohmann/json_fwd.hpp>
#include <extension/uuid4/uuid4.h>
#include <extension/entt/entt.hpp>

#include <utl/math/ACSU_Math.h>
#include <utl/enum/ACSU_Enum.h>
#include <utl/time/ACSU_Time.h>
#include <utl/message/MessageBroker.h>

#include <core/ResourceCaches.h>
#include <core/ResourceManager.h>
#include <core/Blackboard.h>
#include <core/BlackboardKeys.h>
#include <core/TransformTRS.h>
#include <core/IScene.h>
#include <core/IObject.h>
#include <core/GO_Component.h>
#include <core/TransformMath.h>
#include <core/GameObject.h>
#include <core/Transform.h>
#include <core/LayerManager.h>
#include <core/SceneBase.h>
#include <core/SceneManager.h>
#include <core/IApplication.h>
#include <core/ApplicationManager.h>
#include <core/CoreMacros.h>
#include <core/UseModules.h>

#ifdef ACSM_INPUT
#include <modules/input/JoyshockWrapper.h>
#include <modules/input/KeyWrapper.h>
#endif

#ifdef ACSM_DXLIB
#include <modules/dxlib/ACSM_Dxlib.h>
#endif