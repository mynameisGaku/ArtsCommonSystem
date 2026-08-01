// SPDX-License-Identifier: Apache-2.0
#include "gameframework/SubsystemCatalog.h"

#include "gameframework/EventBus.h"
#include "gameframework/Spawn2DSubsystem.h"
#include "gameframework/WorldClockSubsystem.h"
#include "subsystem/SubsystemFactory.h"
#include "subsystem/SubsystemRegistry.h"
#include "memory/UniquePtr.h"

namespace acs {
namespace {

/** ワールド用イベントバスを生成する。 */
TUniquePtr<FSubsystem> CreateEventBusSubsystem() noexcept
{
    return MakeUnique<game::FEventBus>();
}

/** 2D プレハブ生成サブシステムを生成する。 */
TUniquePtr<FSubsystem> CreateSpawn2DSubsystem() noexcept
{
    return MakeUnique<game::FSpawn2DSubsystem>();
}

/** ワールド時計サブシステムを生成する。 */
TUniquePtr<FSubsystem> CreateWorldClockSubsystem() noexcept
{
    return MakeUnique<game::FWorldClockSubsystem>();
}

} // namespace

bool AcsRegisterGameFrameworkSubsystems() noexcept
{
    /** GameFramework 同梱型を保持する正規登録簿。 */
    FSubsystemRegistry& Registry = FSubsystemRegistry::Get();
    const bool EventRegistered = Registry.TryRegisterActive(FSubsystemFactory{
        SubsystemKindOf<game::FEventBus>(), ESubsystemScope::World, "FEventBus",
        &CreateEventBusSubsystem, ESubsystemTickPhase::PreUpdate, 0});
    const bool SpawnRegistered = Registry.TryRegisterActive(FSubsystemFactory{
        SubsystemKindOf<game::FSpawn2DSubsystem>(), ESubsystemScope::World, "FSpawn2DSubsystem",
        &CreateSpawn2DSubsystem, ESubsystemTickPhase::None, 10});
    const bool ClockRegistered = Registry.TryRegisterActive(FSubsystemFactory{
        SubsystemKindOf<game::FWorldClockSubsystem>(), ESubsystemScope::World, "FWorldClockSubsystem",
        &CreateWorldClockSubsystem, ESubsystemTickPhase::PreUpdate, 20});
    return EventRegistered && SpawnRegistered && ClockRegistered;
}

} // namespace acs
