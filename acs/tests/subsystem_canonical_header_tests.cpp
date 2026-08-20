// SPDX-License-Identifier: Apache-2.0
#include "app/UiFontDefaults.h"

#include <cstring>
#include <type_traits>

/** UiFontDefaults.h 単独で利用できる公開関数の型。 */
using FUiFontLoadSignature = acs::TResult<void> (*)(acs::FFont&, acs::IRhiDevice&, acs::f32, acs::u32, bool) noexcept;
static_assert(std::is_same_v<decltype(&acs::UiFontDefaults::TryLoad), FUiFontLoadSignature>);

#include "gameframework/Game.h"

/** Game.hだけで正規AScene戻り値をoverrideできることを固定する。 */
class CGameHeaderFirstConsumer : public acs::CGame {
protected:
    acs::TUniquePtr<acs::AScene> InitialScene() noexcept override = 0;
};

/** Game.h 単独で旧ゲーム名と旧シーン名を利用できることを固定する。 */
class CLegacyGameHeaderFirstConsumer : public acs::FGame {
protected:
    acs::TUniquePtr<acs::FScene> InitialScene() noexcept override = 0;
};

#include "gameframework/SubsystemCollection.h"

namespace acs::game {

/** 旧namespace配置でlegacy登録macroを検証する型。 */
class CLegacyNamespaceMacroSubsystem final : public ASubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(CLegacyNamespaceMacroSubsystem)
};

} // namespace acs::game

/** global配置でlegacy登録macroを検証する型。 */
class CGlobalMacroSubsystem final : public acs::ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CGlobalMacroSubsystem)
};

/** サブシステム基底の旧診断名を固定する検証型。 */
class CDefaultSubsystemNameProbe final : public acs::ASubsystem {
public:
    /** 検証型固有のサブシステム種別を返す。 */
    const void* Kind() const noexcept override
    {
        return acs::SubsystemKindOf<CDefaultSubsystemNameProbe>();
    }
};

namespace acs {

/** canonical namespace配置でlegacy登録macroを検証する型。 */
class CCanonicalNamespaceMacroSubsystem final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(CCanonicalNamespaceMacroSubsystem)
};

} // namespace acs

ACS_REGISTER_SUBSYSTEM(CLegacyNamespaceMacroSubsystem, ::acs::ESubsystemScope::Engine)
ACS_REGISTER_SUBSYSTEM(CGlobalMacroSubsystem, ::acs::ESubsystemScope::Engine)
ACS_REGISTER_SUBSYSTEM(CCanonicalNamespaceMacroSubsystem, ::acs::ESubsystemScope::Engine)

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
/** 描画コンテキストの明示的な接続窓口を取得する公開署名。 */
using FRenderContextWiringAccessSignature = acs::FRenderContext::FWiringAdapter (acs::FRenderContext::*)() noexcept;
/** 描画フレームの開始配線を行う公開署名。 */
using FRenderContextBeginFrameSignature = void (acs::FRenderContext::FWiringAdapter::*)(acs::CRenderer&, acs::IRhiCommandList&, acs::u32, acs::u32) noexcept;
/** 描画フレームの終了配線を行う公開署名。 */
using FRenderContextEndFrameSignature = void (acs::FRenderContext::FWiringAdapter::*)() noexcept;
/** 2D 一括描画器を配線する公開署名。 */
using FRenderContextSpriteBatchSignature = void (acs::FRenderContext::FWiringAdapter::*)(acs::CSpriteBatch*) noexcept;
/** UI フォントを配線する公開署名。 */
using FRenderContextFontSignature = void (acs::FRenderContext::FWiringAdapter::*)(acs::FFont*) noexcept;
/** 描画用テクスチャを配線する共通公開署名。 */
using FRenderContextTextureSignature = void (acs::FRenderContext::FWiringAdapter::*)(acs::IRhiTexture*) noexcept;
/** 描画パスの状態を配線する共通公開署名。 */
using FRenderContextPassFlagSignature = void (acs::FRenderContext::FWiringAdapter::*)(bool) noexcept;
/** 2D 表示変換を配線する公開署名。 */
using FRenderContextView2DSignature = void (acs::FRenderContext::FWiringAdapter::*)(acs::FVec2, acs::f32) noexcept;
static_assert(std::is_same_v<decltype(&acs::FRenderContext::WiringAccess),
                             FRenderContextWiringAccessSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::BeginFrame),
                             FRenderContextBeginFrameSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::EndFrame),
                             FRenderContextEndFrameSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::SetSpriteBatch),
                             FRenderContextSpriteBatchSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::SetFont),
                             FRenderContextFontSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::SetReflection),
                             FRenderContextTextureSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::SetSceneColor),
                             FRenderContextTextureSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::SetSceneDepth),
                             FRenderContextTextureSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::SetSceneColorCapturePass),
                             FRenderContextPassFlagSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::SetWaterDepthCapturePass),
                             FRenderContextPassFlagSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::SetView2D),
                             FRenderContextView2DSignature>);
static_assert(std::is_same_v<decltype(&acs::FRenderContext::FWiringAdapter::SetStencilMaskActive),
                             FRenderContextPassFlagSignature>);
static_assert(std::is_same_v<acs::CSceneServices, acs::game::CSceneServices>);
static_assert(std::is_same_v<acs::FSceneServices, acs::CSceneServices>);
static_assert(std::is_same_v<acs::ESvc, acs::game::ESvc>);
static_assert(std::is_same_v<acs::ANode, acs::game::ANode>);
static_assert(std::is_same_v<acs::AComponent, acs::game::AComponent>);
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
static_assert(std::is_same_v<decltype(&acs::ASubsystem::ManagementAccess),
                              acs::ASubsystem::FManagementAdapter (acs::ASubsystem::*)() noexcept>);
static_assert(std::is_same_v<decltype(&acs::ASubsystem::FManagementAdapter::SetOwner),
                              void (acs::ASubsystem::FManagementAdapter::*)(void*) noexcept>);
static_assert(std::is_same_v<decltype(&acs::ASubsystem::FManagementAdapter::SetOwnerDescriptor),
                              void (acs::ASubsystem::FManagementAdapter::*)(acs::FSubsystemOwner) noexcept>);
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
    CDefaultSubsystemNameProbe Probe;
    EXPECT_TRUE(std::strcmp(Probe.Name(), "FSubsystem") == 0);
}
