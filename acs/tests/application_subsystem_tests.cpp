// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "app/Application.h"
#include "app/ApplicationSubsystemCatalog.h"
#include "app/AssetSubsystem.h"
#include "app/TimerSubsystem.h"
#include "subsystem/SubsystemRegistry.h"

using namespace acs;

namespace {

/** タイマー発火回数を一度進める。 */
void CountTimerFire(void* User) noexcept
{
    ++(*static_cast<u32*>(User));
}

/** 現在有効なfactoryをkindで検索する。 */
const FSubsystemFactory* FindActiveFactory(const void* Kind) noexcept
{
    FSubsystemRegistry& Registry = FSubsystemRegistry::Get();
    for (u32 Index = 0u; Index < Registry.Count(); ++Index) {
        if (Registry.At(Index).kind == Kind) return &Registry.At(Index);
    }
    return nullptr;
}

} // namespace

ACS_TEST(ApplicationSubsystem, AdaptersKeepLegacyIdentityAndPhase)
{
    AcsRegisterApplicationSubsystems();
    FApplication Application;
    FTimerManager* const LegacyTimers = &Application.GetTimers();
    FAssetRegistry* const LegacyAssets = &Application.GetAssets();
    EXPECT_TRUE(Application.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Application, ESubsystemOwnerKind::Application}));
    EXPECT_TRUE(Application.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Application, ESubsystemOwnerKind::Application}));

    FTimerSubsystem* const Timer = Application.GetSubsystem<FTimerSubsystem>();
    FAssetSubsystem* const Asset = Application.GetSubsystem<FAssetSubsystem>();
    EXPECT_TRUE(Timer != nullptr);
    EXPECT_TRUE(Asset != nullptr);
    if (Timer == nullptr || Asset == nullptr) return;
    EXPECT_TRUE(Timer->GetTimers() == LegacyTimers);
    EXPECT_TRUE(Asset->GetAssets() == LegacyAssets);

    u32 FireCount = 0u;
    EXPECT_TRUE(Application.GetTimers().SetTimeout(0.5f, &CountTimerFire, &FireCount).IsValid());
    Application.EngineSubsystems().TickFrame(
        FSubsystemFrameContext{0.1f, 0.5f, 1u, ESubsystemTickPhase::PostUpdate});
    EXPECT_EQ(FireCount, 0u);
    Application.EngineSubsystems().TickFrame(
        FSubsystemFrameContext{0.1f, 0.5f, 1u, ESubsystemTickPhase::PreUpdate});
    EXPECT_EQ(FireCount, 1u);

    Application.EngineSubsystems().Deinitialize();
    EXPECT_TRUE(Application.GetSubsystem<FTimerSubsystem>() == nullptr);
    EXPECT_TRUE(&Application.GetTimers() == LegacyTimers);
    EXPECT_TRUE(&Application.GetAssets() == LegacyAssets);
}

ACS_TEST(ApplicationSubsystem, CatalogRejectsShadowMetadataAndRecovers)
{
    EXPECT_TRUE(AcsRegisterApplicationSubsystems());
    FSubsystemRegistry& Registry = FSubsystemRegistry::Get();
    const FSubsystemFactory* const Active = FindActiveFactory(SubsystemKindOf<FTimerSubsystem>());
    EXPECT_TRUE(Active != nullptr);
    if (Active == nullptr) return;

    const FSubsystemFactory Expected = *Active;
    EXPECT_TRUE(Registry.Unregister(Expected));
    FSubsystemFactory Shadow = Expected;
    Shadow.phase = ESubsystemTickPhase::None;
    EXPECT_TRUE(Registry.TryRegister(Shadow));
    EXPECT_TRUE(!AcsRegisterApplicationSubsystems());
    const FSubsystemFactory* const ShadowActive =
        FindActiveFactory(SubsystemKindOf<FTimerSubsystem>());
    EXPECT_TRUE(ShadowActive != nullptr);
    if (ShadowActive != nullptr) EXPECT_EQ(ShadowActive->phase, ESubsystemTickPhase::None);

    EXPECT_TRUE(Registry.Unregister(Shadow));
    EXPECT_TRUE(AcsRegisterApplicationSubsystems());
    const FSubsystemFactory* const Restored =
        FindActiveFactory(SubsystemKindOf<FTimerSubsystem>());
    EXPECT_TRUE(Restored != nullptr);
    if (Restored != nullptr) EXPECT_EQ(Restored->phase, ESubsystemTickPhase::PreUpdate);
}

ACS_TEST(ApplicationSubsystem, LegacyEngineOwnersKeepAdaptersInert)
{
    EXPECT_TRUE(AcsRegisterApplicationSubsystems());

    FSubsystemCollection Inert;
    Inert.Initialize(ESubsystemScope::Engine);
    EXPECT_TRUE(Inert.IsInitialized());
    FTimerSubsystem* const InertTimer = Inert.Get<FTimerSubsystem>();
    FAssetSubsystem* const InertAsset = Inert.Get<FAssetSubsystem>();
    EXPECT_TRUE(InertTimer != nullptr);
    EXPECT_TRUE(InertAsset != nullptr);
    if (InertTimer != nullptr) EXPECT_TRUE(InertTimer->GetTimers() == nullptr);
    if (InertAsset != nullptr) EXPECT_TRUE(InertAsset->GetAssets() == nullptr);
    FApplication TypedApplication;
    EXPECT_TRUE(!Inert.TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&TypedApplication, ESubsystemOwnerKind::Application}));

    FApplication Application;
    FSubsystemCollection LegacyOwned;
    LegacyOwned.Initialize(ESubsystemScope::Engine, nullptr, &Application);
    EXPECT_TRUE(LegacyOwned.IsInitialized());
    FTimerSubsystem* const Timer = LegacyOwned.Get<FTimerSubsystem>();
    FAssetSubsystem* const Asset = LegacyOwned.Get<FAssetSubsystem>();
    EXPECT_TRUE(Timer != nullptr);
    EXPECT_TRUE(Asset != nullptr);
    if (Timer != nullptr) EXPECT_TRUE(Timer->GetTimers() == nullptr);
    if (Asset != nullptr) EXPECT_TRUE(Asset->GetAssets() == nullptr);

    FSubsystemCollection WrongTypedOwner;
    EXPECT_TRUE(!WrongTypedOwner.TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Application, ESubsystemOwnerKind::Game}));
    EXPECT_EQ(WrongTypedOwner.Count(), 0u);
}

ACS_TEST(ApplicationSubsystem, InitializedCollectionReinitializeRequiresExactOwner)
{
    int FirstOwner = 0;
    int SecondOwner = 0;
    FSubsystemCollection Collection;
    const FSubsystemOwner Owner{&FirstOwner, ESubsystemOwnerKind::Game};
    EXPECT_TRUE(Collection.TryInitialize(ESubsystemScope::GameInstance, nullptr, Owner));
    const usize InitialCount = Collection.Count();
    EXPECT_TRUE(Collection.TryInitialize(ESubsystemScope::GameInstance, nullptr, Owner));
    EXPECT_EQ(Collection.Count(), InitialCount);
    EXPECT_TRUE(!Collection.TryInitialize(
        ESubsystemScope::GameInstance, nullptr,
        FSubsystemOwner{&SecondOwner, ESubsystemOwnerKind::Game}));
    EXPECT_EQ(Collection.Count(), InitialCount);
    EXPECT_TRUE(!Collection.TryInitialize(
        ESubsystemScope::GameInstance, nullptr,
        FSubsystemOwner{&FirstOwner, ESubsystemOwnerKind::Scene}));
    EXPECT_EQ(Collection.Count(), InitialCount);
}
