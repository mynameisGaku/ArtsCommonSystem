// SPDX-License-Identifier: Apache-2.0
#include "gameframework/SubsystemCatalog.h"

#include "gameframework/EventBus.h"
#include "gameframework/RenderFrameSubmissionSubsystem.h"
#include "gameframework/Spawn2DSubsystem.h"
#include "gameframework/WorldClockSubsystem.h"
#include "subsystem/SubsystemFactory.h"
#include "subsystem/SubsystemRegistry.h"
#include "memory/UniquePtr.h"

namespace acs {
namespace {

/** ワールド用イベントバスを生成する。 */
TUniquePtr<ASubsystem> CreateEventBusSubsystem() noexcept
{
    return MakeUnique<game::AEventBus>();
}

/** 2D プレハブ生成サブシステムを生成する。 */
TUniquePtr<ASubsystem> CreateSpawn2DSubsystem() noexcept
{
    return MakeUnique<game::ASpawn2DSubsystem>();
}

/** ワールド時計サブシステムを生成する。 */
TUniquePtr<ASubsystem> CreateWorldClockSubsystem() noexcept
{
    return MakeUnique<game::AWorldClockSubsystem>();
}

/** Worldで記録した命令へGPU提出結果を返す通知境界を生成する。 */
TUniquePtr<ASubsystem> CreateRenderFrameSubmissionSubsystem() noexcept
{
    return MakeUnique<game::CRenderFrameSubmissionSubsystem>();
}

} // namespace

bool AcsRegisterGameFrameworkSubsystems() noexcept
{
    /** GameFramework 同梱型を保持する正規登録簿。 */
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    const bool EventRegistered = Registry.TryRegisterActive(FSubsystemFactory{
        SubsystemKindOf<game::AEventBus>(), ESubsystemScope::World, "FEventBus",
        &CreateEventBusSubsystem, ESubsystemTickPhase::PreUpdate, 0});
    const bool SpawnRegistered = Registry.TryRegisterActive(FSubsystemFactory{
        SubsystemKindOf<game::ASpawn2DSubsystem>(), ESubsystemScope::World, "FSpawn2DSubsystem",
        &CreateSpawn2DSubsystem, ESubsystemTickPhase::None, 10});
    const bool ClockRegistered = Registry.TryRegisterActive(FSubsystemFactory{
        SubsystemKindOf<game::AWorldClockSubsystem>(), ESubsystemScope::World, "FWorldClockSubsystem",
        &CreateWorldClockSubsystem, ESubsystemTickPhase::PreUpdate, 20});
    const bool FrameSubmissionRegistered = Registry.TryRegisterActive(FSubsystemFactory{
        SubsystemKindOf<game::CRenderFrameSubmissionSubsystem>(), ESubsystemScope::World,
        "CRenderFrameSubmissionSubsystem", &CreateRenderFrameSubmissionSubsystem,
        ESubsystemTickPhase::None, 30});
    return EventRegistered && SpawnRegistered && ClockRegistered &&
        FrameSubmissionRegistered;
}

} // namespace acs
