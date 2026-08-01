// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "app/Application.h"
#include "app/ApplicationSubsystemCatalog.h"
#include "app/AssetSubsystem.h"
#include "app/TimerSubsystem.h"
#include "subsystem/SubsystemRegistry.h"

#include <cstring>
#include <type_traits>

using namespace acs;

static_assert(std::is_same_v<FTimerSubsystem, ATimerSubsystem>);
static_assert(std::is_same_v<FAssetSubsystem, AAssetSubsystem>);

namespace {

/** タイマー発火回数を一度進める。 */
void CountTimerFire(void* User) noexcept
{
    ++(*static_cast<u32*>(User));
}

/** 現在有効なfactoryをkindで検索する。 */
const FSubsystemFactory* FindActiveFactory(const void* Kind) noexcept
{
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    for (u32 Index = 0u; Index < Registry.Count(); ++Index) {
        if (Registry.At(Index).kind == Kind) return &Registry.At(Index);
    }
    return nullptr;
}

} // namespace

ACS_TEST(ApplicationSubsystem, AdaptersKeepLegacyIdentityAndPhase)
{
    AcsRegisterApplicationSubsystems();
    CApplication Application;
    FTimerManager* const LegacyTimers = &Application.GetTimers();
    FAssetRegistry* const LegacyAssets = &Application.GetAssets();
    EXPECT_TRUE(Application.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Application, ESubsystemOwnerKind::Application}));
    EXPECT_TRUE(Application.EngineSubsystems().TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Application, ESubsystemOwnerKind::Application}));

    ATimerSubsystem* const Timer = Application.GetSubsystem<ATimerSubsystem>();
    AAssetSubsystem* const Asset = Application.GetSubsystem<AAssetSubsystem>();
    FTimerSubsystem* const LegacyTimerAlias = Timer;
    FAssetSubsystem* const LegacyAssetAlias = Asset;
    EXPECT_TRUE(Timer != nullptr);
    EXPECT_TRUE(Asset != nullptr);
    EXPECT_TRUE(LegacyTimerAlias == Timer);
    EXPECT_TRUE(LegacyAssetAlias == Asset);
    if (Timer == nullptr || Asset == nullptr) return;
    EXPECT_TRUE(std::strcmp(Timer->Name(), "FTimerSubsystem") == 0);
    EXPECT_TRUE(std::strcmp(Asset->Name(), "FAssetSubsystem") == 0);
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
    EXPECT_TRUE(Application.GetSubsystem<ATimerSubsystem>() == nullptr);
    EXPECT_TRUE(&Application.GetTimers() == LegacyTimers);
    EXPECT_TRUE(&Application.GetAssets() == LegacyAssets);
}

ACS_TEST(ApplicationSubsystem, CatalogRejectsShadowMetadataAndRecovers)
{
    EXPECT_TRUE(AcsRegisterApplicationSubsystems());
    CSubsystemRegistry& Registry = CSubsystemRegistry::Get();
    const FSubsystemFactory* const Active = FindActiveFactory(SubsystemKindOf<ATimerSubsystem>());
    EXPECT_TRUE(Active != nullptr);
    if (Active == nullptr) return;
    EXPECT_TRUE(std::strcmp(Active->name, "FTimerSubsystem") == 0);

    const FSubsystemFactory Expected = *Active;
    EXPECT_TRUE(Registry.Unregister(Expected));
    FSubsystemFactory Shadow = Expected;
    Shadow.phase = ESubsystemTickPhase::None;
    EXPECT_TRUE(Registry.TryRegister(Shadow));
    EXPECT_TRUE(!AcsRegisterApplicationSubsystems());
    const FSubsystemFactory* const ShadowActive =
        FindActiveFactory(SubsystemKindOf<ATimerSubsystem>());
    EXPECT_TRUE(ShadowActive != nullptr);
    if (ShadowActive != nullptr) EXPECT_EQ(ShadowActive->phase, ESubsystemTickPhase::None);

    EXPECT_TRUE(Registry.Unregister(Shadow));
    EXPECT_TRUE(AcsRegisterApplicationSubsystems());
    const FSubsystemFactory* const Restored =
        FindActiveFactory(SubsystemKindOf<ATimerSubsystem>());
    EXPECT_TRUE(Restored != nullptr);
    if (Restored != nullptr) EXPECT_EQ(Restored->phase, ESubsystemTickPhase::PreUpdate);
}

ACS_TEST(ApplicationSubsystem, LegacyEngineOwnersKeepAdaptersInert)
{
    EXPECT_TRUE(AcsRegisterApplicationSubsystems());

    CSubsystemCollection Inert;
    Inert.Initialize(ESubsystemScope::Engine);
    EXPECT_TRUE(Inert.IsInitialized());
    ATimerSubsystem* const InertTimer = Inert.Get<ATimerSubsystem>();
    AAssetSubsystem* const InertAsset = Inert.Get<AAssetSubsystem>();
    EXPECT_TRUE(InertTimer != nullptr);
    EXPECT_TRUE(InertAsset != nullptr);
    if (InertTimer != nullptr) EXPECT_TRUE(InertTimer->GetTimers() == nullptr);
    if (InertAsset != nullptr) EXPECT_TRUE(InertAsset->GetAssets() == nullptr);
    CApplication TypedApplication;
    EXPECT_TRUE(!Inert.TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&TypedApplication, ESubsystemOwnerKind::Application}));

    CApplication Application;
    CSubsystemCollection LegacyOwned;
    LegacyOwned.Initialize(ESubsystemScope::Engine, nullptr, &Application);
    EXPECT_TRUE(LegacyOwned.IsInitialized());
    ATimerSubsystem* const Timer = LegacyOwned.Get<ATimerSubsystem>();
    AAssetSubsystem* const Asset = LegacyOwned.Get<AAssetSubsystem>();
    EXPECT_TRUE(Timer != nullptr);
    EXPECT_TRUE(Asset != nullptr);
    if (Timer != nullptr) EXPECT_TRUE(Timer->GetTimers() == nullptr);
    if (Asset != nullptr) EXPECT_TRUE(Asset->GetAssets() == nullptr);

    CSubsystemCollection WrongTypedOwner;
    EXPECT_TRUE(!WrongTypedOwner.TryInitialize(
        ESubsystemScope::Engine, nullptr,
        FSubsystemOwner{&Application, ESubsystemOwnerKind::Game}));
    EXPECT_EQ(WrongTypedOwner.Count(), 0u);
}

ACS_TEST(ApplicationSubsystem, InitializedCollectionReinitializeRequiresExactOwner)
{
    int FirstOwner = 0;
    int SecondOwner = 0;
    CSubsystemCollection Collection;
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
