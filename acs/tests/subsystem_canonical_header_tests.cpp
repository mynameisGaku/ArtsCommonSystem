// SPDX-License-Identifier: Apache-2.0
#include "app/Sample.h"

#include <cstring>
#include <type_traits>

static_assert(std::is_same_v<acs::FApplication, acs::CApplication>);

/** Sample.h 単独で旧アプリケーション前方宣言を利用できることを固定する。 */
void AcceptLegacySampleApplication(acs::FApplication*) noexcept;

#include "gameframework/Game.h"

/** Game.hだけで正規AScene戻り値をoverrideできることを固定する。 */
class FGameHeaderFirstConsumer : public acs::CGame {
protected:
    acs::TUniquePtr<acs::AScene> InitialScene() noexcept override = 0;
};

/** Game.h 単独で旧ゲーム名と旧シーン名を利用できることを固定する。 */
class FLegacyGameHeaderFirstConsumer : public acs::FGame {
protected:
    acs::TUniquePtr<acs::FScene> InitialScene() noexcept override = 0;
};

#include "gameframework/SubsystemCollection.h"

namespace acs::game {

/** 旧namespace配置でlegacy登録macroを検証する型。 */
class FLegacyNamespaceMacroSubsystem final : public ASubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FLegacyNamespaceMacroSubsystem)
};

} // namespace acs::game

/** global配置でlegacy登録macroを検証する型。 */
class FGlobalMacroSubsystem final : public acs::ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(FGlobalMacroSubsystem)
};

/** サブシステム基底の旧診断名を固定する検証型。 */
class FDefaultSubsystemNameProbe final : public acs::ASubsystem {
public:
    /** 検証型固有のサブシステム種別を返す。 */
    const void* Kind() const noexcept override
    {
        return acs::SubsystemKindOf<FDefaultSubsystemNameProbe>();
    }
};

namespace acs {

/** canonical namespace配置でlegacy登録macroを検証する型。 */
class FCanonicalNamespaceMacroSubsystem final : public ASubsystem {
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
#include "gameframework/Scene2D.h"
#include "gameframework/EventBus.h"
#include "gameframework/WorldClockSubsystem.h"
#include "app/AssetSubsystem.h"
#include "app/TimerSubsystem.h"

#include <cstddef>

static_assert(std::is_same_v<acs::game::ASubsystem, acs::ASubsystem>);
static_assert(std::is_same_v<acs::game::FSubsystem, acs::ASubsystem>);
static_assert(std::is_same_v<acs::FSubsystem, acs::ASubsystem>);
static_assert(std::is_same_v<acs::game::CSubsystemCollection, acs::CSubsystemCollection>);
static_assert(std::is_same_v<acs::game::FSubsystemCollection, acs::CSubsystemCollection>);
static_assert(std::is_same_v<acs::FSubsystemCollection, acs::CSubsystemCollection>);
static_assert(std::is_same_v<acs::game::CSubsystemRegistry, acs::CSubsystemRegistry>);
static_assert(std::is_same_v<acs::game::FSubsystemRegistry, acs::CSubsystemRegistry>);
static_assert(std::is_same_v<acs::game::FSubsystemAutoRegister, acs::CSubsystemAutoRegister>);
static_assert(std::is_same_v<acs::FSubsystemRegistry, acs::CSubsystemRegistry>);
static_assert(std::is_same_v<acs::FSubsystemAutoRegister, acs::CSubsystemAutoRegister>);
static_assert(std::is_same_v<acs::game::ESubsystemScope, acs::ESubsystemScope>);
static_assert(std::is_same_v<acs::CGame, acs::game::CGame>);
static_assert(std::is_same_v<acs::FGame, acs::CGame>);
static_assert(std::is_same_v<acs::AScene, acs::game::AScene>);
static_assert(std::is_same_v<acs::FScene, acs::AScene>);
static_assert(std::is_same_v<acs::CSceneManager, acs::game::CSceneManager>);
static_assert(std::is_same_v<acs::FSceneManager, acs::CSceneManager>);
static_assert(std::is_same_v<acs::FRenderContext, acs::game::FRenderContext>);
static_assert(std::is_same_v<acs::CSceneServices, acs::game::CSceneServices>);
static_assert(std::is_same_v<acs::FSceneServices, acs::CSceneServices>);
static_assert(std::is_same_v<acs::ESvc, acs::game::ESvc>);
static_assert(std::is_same_v<acs::ANode, acs::game::ANode>);
static_assert(std::is_same_v<acs::AComponent, acs::game::AComponent>);
static_assert(std::is_same_v<acs::FScene2D, acs::AScene2D>);
static_assert(std::is_same_v<acs::ASpawn2DSubsystem, acs::game::ASpawn2DSubsystem>);
static_assert(std::is_same_v<acs::FSpawn2DSubsystem, acs::ASpawn2DSubsystem>);
static_assert(std::is_same_v<acs::FEventBus, acs::AEventBus>);
static_assert(std::is_same_v<acs::FWorldClockSubsystem, acs::AWorldClockSubsystem>);
static_assert(std::is_same_v<acs::FTimerSubsystem, acs::ATimerSubsystem>);
static_assert(std::is_same_v<acs::FAssetSubsystem, acs::AAssetSubsystem>);
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
static_assert(sizeof(acs::ASubsystem) == 24u);
static_assert(alignof(acs::ASubsystem) == 8u);
static_assert(std::is_polymorphic_v<acs::ASubsystem>);
static_assert(std::has_virtual_destructor_v<acs::ASubsystem>);
static_assert(sizeof(acs::CSubsystemCollection) == 80u);
static_assert(alignof(acs::CSubsystemCollection) == 8u);
static_assert(sizeof(acs::ASpawn2DSubsystem) == 32u);
static_assert(alignof(acs::ASpawn2DSubsystem) == 8u);
static_assert(std::is_same_v<decltype(&acs::ASubsystem::OnTick),
                             void (acs::ASubsystem::*)(acs::f32) noexcept>);
static_assert(std::is_same_v<decltype(&acs::ASubsystem::OnTickFrame),
                             void (acs::ASubsystem::*)(
                                 const acs::FSubsystemFrameContext&) noexcept>);
static_assert(std::is_same_v<decltype(&acs::ASubsystem::_SetOwner),
                             void (acs::ASubsystem::*)(void*) noexcept>);
static_assert(std::is_same_v<decltype(&acs::ASubsystem::_SetOwnerDescriptor),
                             void (acs::ASubsystem::*)(acs::FSubsystemOwner) noexcept>);
static_assert(std::is_same_v<decltype(&acs::CSubsystemCollection::Initialize),
                             void (acs::CSubsystemCollection::*)(
                                 acs::ESubsystemScope, acs::CSubsystemCollection*, void*) noexcept>);
static_assert(std::is_same_v<decltype(&acs::CSubsystemCollection::Tick),
                             void (acs::CSubsystemCollection::*)(acs::f32) noexcept>);
static_assert(std::is_same_v<decltype(&acs::CSubsystemCollection::TickFrame),
                             void (acs::CSubsystemCollection::*)(
                                 const acs::FSubsystemFrameContext&) noexcept>);

ACS_TEST(SubsystemCanonicalHeaders, TopLevelAndCompatibilityNamesMatch)
{
    EXPECT_TRUE((std::is_same_v<acs::game::ASubsystem, acs::ASubsystem>));
    /** 既定診断名を確認する基底実装の検証値。 */
    FDefaultSubsystemNameProbe Probe;
    EXPECT_TRUE(std::strcmp(Probe.Name(), "FSubsystem") == 0);
}
