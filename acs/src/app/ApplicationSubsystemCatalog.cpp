// SPDX-License-Identifier: Apache-2.0
#include "app/ApplicationSubsystemCatalog.h"

#include "app/AssetSubsystem.h"
#include "app/TimerSubsystem.h"
#include "foundation/Limits.h"
#include "memory/UniquePtr.h"
#include "subsystem/SubsystemRegistry.h"

namespace acs {
namespace {

/** App の既存タイマー管理器へ接続するアダプターを生成する。 */
TUniquePtr<FSubsystem> CreateApplicationTimerSubsystem() noexcept
{
    return MakeUnique<FTimerSubsystem>();
}

/** App の既存アセット登録簿へ接続するアダプターを生成する。 */
TUniquePtr<FSubsystem> CreateApplicationAssetSubsystem() noexcept
{
    return MakeUnique<FAssetSubsystem>();
}

} // namespace

/** App 組み込みサブシステムを冪等登録し、全 factory が利用可能なら true を返す。 */
bool AcsRegisterApplicationSubsystems() noexcept
{
    // process 内で共有するサブシステム登録簿。
    FSubsystemRegistry& registry = FSubsystemRegistry::Get();
    // 既存タイマー管理器を最初の PreUpdate で進める factory。
    const FSubsystemFactory TimerFactory{SubsystemKindOf<FTimerSubsystem>(), ESubsystemScope::Engine, "FTimerSubsystem", &CreateApplicationTimerSubsystem, ESubsystemTickPhase::PreUpdate, TNumLimits<i32>::Min()};
    const bool TimerRegistered = registry.TryRegisterActive(TimerFactory);
    // 更新 callback を持たず、既存アセット登録簿だけを公開する factory。
    const FSubsystemFactory AssetFactory{SubsystemKindOf<FAssetSubsystem>(), ESubsystemScope::Engine, "FAssetSubsystem", &CreateApplicationAssetSubsystem, ESubsystemTickPhase::None, 0};
    const bool AssetRegistered = registry.TryRegisterActive(AssetFactory);
    return TimerRegistered && AssetRegistered;
}

} // namespace acs
