// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Game.h"

/** Game.hだけで正規FScene戻り値をoverrideできることを固定する。 */
class FGameHeaderFirstConsumer : public acs::FGame {
protected:
    acs::TUniquePtr<acs::FScene> InitialScene() noexcept override = 0;
};

#include "gameframework/SubsystemCollection.h"

namespace acs::game {

/** 旧namespace配置でlegacy登録macroを検証する型。 */
class FLegacyNamespaceMacroSubsystem final : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FLegacyNamespaceMacroSubsystem)
};

} // namespace acs::game

/** global配置でlegacy登録macroを検証する型。 */
class FGlobalMacroSubsystem final : public acs::FSubsystem {
public:
    ACS_SUBSYSTEM_KIND(FGlobalMacroSubsystem)
};

namespace acs {

/** canonical namespace配置でlegacy登録macroを検証する型。 */
class FCanonicalNamespaceMacroSubsystem final : public FSubsystem {
public:
    ACS_SUBSYSTEM_KIND(FCanonicalNamespaceMacroSubsystem)
};

} // namespace acs

ACS_REGISTER_SUBSYSTEM(FLegacyNamespaceMacroSubsystem, ::acs::ESubsystemScope::Engine)
ACS_REGISTER_SUBSYSTEM(FGlobalMacroSubsystem, ::acs::ESubsystemScope::Engine)
ACS_REGISTER_SUBSYSTEM(FCanonicalNamespaceMacroSubsystem, ::acs::ESubsystemScope::Engine)

#include "subsystem/Subsystem.h"
#include "subsystem/SubsystemCollection.h"
#include "subsystem/SubsystemFactory.h"
#include "subsystem/SubsystemFrameContext.h"
#include "subsystem/SubsystemRegistry.h"
#include "subsystem/SubsystemScope.h"
#include "subsystem/SubsystemTickPhase.h"

#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Subsystem.h"
#include "gameframework/SubsystemCollection.h"
#include "gameframework/SubsystemRegistry.h"
#include "gameframework/Scene.h"
#include "gameframework/SceneManager.h"
#include "gameframework/RenderContext.h"
#include "gameframework/SceneServices.h"
#include "gameframework/ANode.h"
#include "gameframework/AComponent.h"
#include "gameframework/Spawn2DSubsystem.h"

#include <cstddef>
#include <type_traits>

static_assert(std::is_same_v<acs::game::FSubsystem, acs::FSubsystem>);
static_assert(std::is_same_v<acs::game::FSubsystemCollection, acs::FSubsystemCollection>);
static_assert(std::is_same_v<acs::game::FSubsystemRegistry, acs::FSubsystemRegistry>);
static_assert(std::is_same_v<acs::game::ESubsystemScope, acs::ESubsystemScope>);
static_assert(std::is_same_v<acs::FGame, acs::game::FGame>);
static_assert(std::is_same_v<acs::FScene, acs::game::FScene>);
static_assert(std::is_same_v<acs::FSceneManager, acs::game::FSceneManager>);
static_assert(std::is_same_v<acs::FRenderContext, acs::game::FRenderContext>);
static_assert(std::is_same_v<acs::FSceneServices, acs::game::FSceneServices>);
static_assert(std::is_same_v<acs::ESvc, acs::game::ESvc>);
static_assert(std::is_same_v<acs::ANode, acs::game::ANode>);
static_assert(std::is_same_v<acs::AComponent, acs::game::AComponent>);
static_assert(std::is_same_v<acs::FSpawn2DSubsystem, acs::game::FSpawn2DSubsystem>);
static_assert(sizeof(acs::FSubsystemOwner) == 16u);
static_assert(alignof(acs::FSubsystemOwner) == 8u);
static_assert(offsetof(acs::FSubsystemOwner, pointer) == 0u);
static_assert(offsetof(acs::FSubsystemOwner, kind) == 8u);
static_assert(sizeof(acs::FSubsystemFrameContext) == 24u);
static_assert(alignof(acs::FSubsystemFrameContext) == 8u);
static_assert(offsetof(acs::FSubsystemFrameContext, scaled_delta_seconds) == 0u);
static_assert(offsetof(acs::FSubsystemFrameContext, unscaled_delta_seconds) == 4u);
static_assert(offsetof(acs::FSubsystemFrameContext, frame_number) == 8u);
static_assert(offsetof(acs::FSubsystemFrameContext, phase) == 16u);
static_assert(sizeof(acs::FSubsystemFactory) == 40u);
static_assert(alignof(acs::FSubsystemFactory) == 8u);
static_assert(offsetof(acs::FSubsystemFactory, kind) == 0u);
static_assert(offsetof(acs::FSubsystemFactory, scope) == 8u);
static_assert(offsetof(acs::FSubsystemFactory, name) == 16u);
static_assert(offsetof(acs::FSubsystemFactory, create) == 24u);
static_assert(offsetof(acs::FSubsystemFactory, phase) == 32u);
static_assert(offsetof(acs::FSubsystemFactory, order) == 36u);
static_assert(sizeof(acs::FSubsystem) == 24u);
static_assert(alignof(acs::FSubsystem) == 8u);
static_assert(std::is_polymorphic_v<acs::FSubsystem>);
static_assert(std::has_virtual_destructor_v<acs::FSubsystem>);
static_assert(sizeof(acs::FSubsystemCollection) == 80u);
static_assert(alignof(acs::FSubsystemCollection) == 8u);
static_assert(sizeof(acs::FSpawn2DSubsystem) == 32u);
static_assert(alignof(acs::FSpawn2DSubsystem) == 8u);
static_assert(std::is_same_v<decltype(&acs::FSubsystem::OnTick),
                             void (acs::FSubsystem::*)(acs::f32) noexcept>);
static_assert(std::is_same_v<decltype(&acs::FSubsystem::OnTickFrame),
                             void (acs::FSubsystem::*)(
                                 const acs::FSubsystemFrameContext&) noexcept>);
static_assert(std::is_same_v<decltype(&acs::FSubsystem::_SetOwner),
                             void (acs::FSubsystem::*)(void*) noexcept>);
static_assert(std::is_same_v<decltype(&acs::FSubsystem::_SetOwnerDescriptor),
                             void (acs::FSubsystem::*)(acs::FSubsystemOwner) noexcept>);
static_assert(std::is_same_v<decltype(&acs::FSubsystemCollection::Initialize),
                             void (acs::FSubsystemCollection::*)(
                                 acs::ESubsystemScope, acs::FSubsystemCollection*, void*) noexcept>);
static_assert(std::is_same_v<decltype(&acs::FSubsystemCollection::Tick),
                             void (acs::FSubsystemCollection::*)(acs::f32) noexcept>);
static_assert(std::is_same_v<decltype(&acs::FSubsystemCollection::TickFrame),
                             void (acs::FSubsystemCollection::*)(
                                 const acs::FSubsystemFrameContext&) noexcept>);

ACS_TEST(SubsystemCanonicalHeaders, TopLevelAndCompatibilityNamesMatch)
{
    EXPECT_TRUE((std::is_same_v<acs::game::FSubsystem, acs::FSubsystem>));
}
