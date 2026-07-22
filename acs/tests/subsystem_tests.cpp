// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: サブシステム基盤の検証 (GPU 非依存)。
//   ・FSubsystemCollection が登録簿から «該当スコープ» のサブシステムを全生成し
//     OnInitialize する
//   ・Get<T>() が自スコープ → 上位スコープ (World→GameInstance→Engine) へフォール
//     バック検索する。上位から下位は «見えない»
//   ・OnTick がフレーム毎に呼ばれ、OnDeinitialize が解体時に呼ばれる
//   ・ANode / AComponent の GetSubsystem<T>() が root 配線を walk-to-root で
//     解決する (= ワールドのオブジェクト同士がサブシステム経由でやり取りできる)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Subsystem.h"
#include "gameframework/SubsystemRegistry.h"
#include "gameframework/SubsystemCollection.h"
#include "gameframework/ANode.h"
#include "gameframework/AComponent.h"
#include "gameframework/WorldClockSubsystem.h"
#include "memory/UniquePtr.h"

using namespace acs;
using namespace acs::game;

namespace {

// World スコープ: スコア管理 (オブジェクト間のやり取りのハブを想定)。
class FScoreSub : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FScoreSub)
    int score  = -999;
    int ticks  = 0;
    int deinit = 0;
    void OnInitialize()   noexcept override { score = 0; }
    void OnDeinitialize() noexcept override { ++deinit; }
    void OnTick(f32)      noexcept override { ++ticks; }
    void Add(int n)       noexcept { score += n; }
};

// GameInstance スコープ: シーン跨ぎの設定 (シングルトン)。
class FConfigSub : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FConfigSub)
    int volume = 0;
    void OnInitialize() noexcept override { volume = 50; }
};

// Engine スコープ: アプリ全体寿命。
class FEngineSub : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FEngineSub)
    bool inited = false;
    void OnInitialize() noexcept override { inited = true; }
};

// コンポーネントからサブシステムを引いて使うテストコンポーネント。
struct AProbeComponent : public AComponent {
    ACS_GAME_COMPONENT_KIND(AProbeComponent)
};

// OnInitialize 時点で Owner() を読み取って記録するサブシステム (World)。
class FOwnerSub : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FOwnerSub)
    void* seenOwner = reinterpret_cast<void*>(0x1);   // 初期値は «未設定» と区別できる値
    void OnInitialize() noexcept override { seenOwner = Owner(); }
};

class FDynamicSubsystemOne : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FDynamicSubsystemOne)
};

class FDynamicSubsystemTwo : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FDynamicSubsystemTwo)
};

TUniquePtr<FSubsystem> CreateDynamicSubsystemOne() noexcept
{
    return MakeUnique<FDynamicSubsystemOne>();
}

TUniquePtr<FSubsystem> CreateDynamicSubsystemTwo() noexcept
{
    return MakeUnique<FDynamicSubsystemTwo>();
}

const FSubsystemFactory* FindFactoryByKind(const FSubsystemRegistry& registry, const void* kind) noexcept
{
    for (u32 index = 0; index < registry.Count(); ++index) {
        if (registry.At(index).kind == kind) return &registry.At(index);
    }
    return nullptr;
}

} // namespace

ACS_REGISTER_SUBSYSTEM(FScoreSub,  ESubsystemScope::World)
ACS_REGISTER_SUBSYSTEM(FConfigSub, ESubsystemScope::GameInstance)
ACS_REGISTER_SUBSYSTEM(FEngineSub, ESubsystemScope::Engine)
ACS_REGISTER_SUBSYSTEM(FOwnerSub,  ESubsystemScope::World)

namespace {

// Engine→GameInstance→World の親チェーンで 3 コレクションを初期化するヘルパ。
struct FStack {
    FSubsystemCollection engine, gameInst, world;
    FStack() noexcept {
        engine.Initialize(ESubsystemScope::Engine);
        gameInst.Initialize(ESubsystemScope::GameInstance, &engine);
        world.Initialize(ESubsystemScope::World, &gameInst);
    }
};

} // namespace

// 各スコープのサブシステムが «該当スコープのコレクションにのみ» 生成される。
ACS_TEST(Subsystem, InstantiatedPerScope) {
    FStack s;
    EXPECT_TRUE(s.world.Get<FScoreSub>()     != nullptr);
    EXPECT_TRUE(s.gameInst.Get<FConfigSub>() != nullptr);
    EXPECT_TRUE(s.engine.Get<FEngineSub>()   != nullptr);
    // OnInitialize が走っている。
    EXPECT_EQ(s.world.Get<FScoreSub>()->score, 0);
    EXPECT_EQ(s.gameInst.Get<FConfigSub>()->volume, 50);
    EXPECT_TRUE(s.engine.Get<FEngineSub>()->inited);
}

// 下位スコープから上位スコープを取得できる (フォールバック)。上位→下位は不可。
ACS_TEST(Subsystem, ScopeFallback) {
    FStack s;
    // World から GameInstance / Engine が見える。
    EXPECT_TRUE(s.world.Get<FConfigSub>() != nullptr);
    EXPECT_TRUE(s.world.Get<FEngineSub>() != nullptr);
    // GameInstance から Engine は見えるが World は見えない。
    EXPECT_TRUE(s.gameInst.Get<FEngineSub>() != nullptr);
    EXPECT_TRUE(s.gameInst.Get<FScoreSub>()  == nullptr);
    // Engine から下位は見えない。
    EXPECT_TRUE(s.engine.Get<FConfigSub>() == nullptr);
    EXPECT_TRUE(s.engine.Get<FScoreSub>()  == nullptr);
}

// Tick が毎フレーム呼ばれる。
ACS_TEST(Subsystem, TickForwards) {
    FStack s;
    s.world.Tick(0.016f);
    s.world.Tick(0.016f);
    s.world.Tick(0.016f);
    EXPECT_EQ(s.world.Get<FScoreSub>()->ticks, 3);
}

// Deinitialize で OnDeinitialize が呼ばれ、以後取得不可になる。冪等。
ACS_TEST(Subsystem, DeinitializeTearsDown) {
    FSubsystemCollection world;
    world.Initialize(ESubsystemScope::World);
    EXPECT_TRUE(world.Get<FScoreSub>() != nullptr);
    world.Deinitialize();
    EXPECT_TRUE(world.Get<FScoreSub>() == nullptr);
    world.Deinitialize();   // 冪等 (二重解体しても安全)
}

// ノード/コンポーネントが root 配線のサブシステムを walk-to-root で取得できる
// (= ワールドのオブジェクト同士がサブシステム経由でやり取りできる)。
ACS_TEST(Subsystem, NodeAndComponentAccess) {
    FStack s;
    ANode root;
    root._SetSubsystems(&s.world);                          // root にのみ配線
    ANode& child = root.AddChild(NewObject<ANode>());
    AProbeComponent& comp = child.AddComponent<AProbeComponent>();

    // 子ノードから (walk-to-root)。
    EXPECT_TRUE(child.GetSubsystem<FScoreSub>()  != nullptr);
    EXPECT_TRUE(child.GetSubsystem<FConfigSub>() != nullptr);   // 上位へフォールバック
    // コンポーネントから (owner → root)。
    EXPECT_TRUE(comp.GetSubsystem<FScoreSub>() != nullptr);

    // コンポーネント経由でスコアを加算 → 同じ World サブシステムへ反映される。
    comp.GetSubsystem<FScoreSub>()->Add(7);
    comp.GetSubsystem<FScoreSub>()->Add(3);
    EXPECT_EQ(s.world.Get<FScoreSub>()->score, 10);

    // 未配線ノードは nullptr。
    ANode lonely;
    EXPECT_TRUE(lonely.GetSubsystem<FScoreSub>() == nullptr);
}

// Owner() が Initialize で渡したコンテキストになる(OnInitialize 時点で有効)。
ACS_TEST(Subsystem, OwnerContextIsSet) {
    int dummyOwner = 0;
    void* owner = &dummyOwner;
    FSubsystemCollection world;
    world.Initialize(ESubsystemScope::World, nullptr, owner);
    FOwnerSub* s = world.Get<FOwnerSub>();
    EXPECT_TRUE(s != nullptr);
    EXPECT_TRUE(s->Owner() == owner);            // 取得 API
    EXPECT_TRUE(s->seenOwner == owner);          // OnInitialize 時点で既に配線済み
    EXPECT_TRUE(s->OwnerAs<int>() == &dummyOwner);
    // owner 未指定なら nullptr。
    FSubsystemCollection w2;
    w2.Initialize(ESubsystemScope::World);
    EXPECT_TRUE(w2.Get<FOwnerSub>()->Owner() == nullptr);
}

ACS_TEST(Subsystem, DuplicateFactorySourcesPromoteOnUnregister)
{
    static const int source_kind = 0;
    const FSubsystemFactory first{&source_kind, ESubsystemScope::World, "DynamicSubsystem", &CreateDynamicSubsystemOne};
    const FSubsystemFactory second{&source_kind, ESubsystemScope::World, "DynamicSubsystem",
                                   &CreateDynamicSubsystemTwo};

    FSubsystemRegistry& registry = FSubsystemRegistry::Get();
    const u32 count_before = registry.Count();
    registry.Register(first);
    registry.Register(second);
    EXPECT_EQ(registry.Count(), count_before + 1u);
    EXPECT_TRUE(FindFactoryByKind(registry, &source_kind)->create == &CreateDynamicSubsystemOne);

    // 先に有効だった module/source を外すと、残る factory へ昇格する。
    EXPECT_TRUE(registry.Unregister(first));
    const FSubsystemFactory* promoted = FindFactoryByKind(registry, &source_kind);
    EXPECT_TRUE(promoted != nullptr);
    EXPECT_TRUE(promoted->create == &CreateDynamicSubsystemTwo);

    EXPECT_TRUE(registry.Unregister(second));
    EXPECT_EQ(registry.Count(), count_before);
    EXPECT_TRUE(FindFactoryByKind(registry, &source_kind) == nullptr);
    EXPECT_TRUE(!registry.Unregister(second));
}

// エンジン提供の FWorldClockSubsystem が各 World に自動生成され、OnTick で経過時間と
// フレーム数を積む。Deinitialize→再 Initialize で 0 から始まる。
ACS_TEST(Subsystem, WorldClockAccumulatesElapsedAndFrames)
{
    FSubsystemCollection world;
    world.Initialize(ESubsystemScope::World);
    FWorldClockSubsystem* clock = world.Get<FWorldClockSubsystem>();
    EXPECT_TRUE(clock != nullptr);
    if (clock == nullptr) return;

    // OnInitialize 直後は 0。
    EXPECT_NEAR(clock->ElapsedSeconds(), 0.0, 1e-9);
    EXPECT_EQ(clock->FrameCount(), 0ull);

    world.Tick(0.5f);
    world.Tick(0.25f);
    world.Tick(0.25f);
    EXPECT_EQ(clock->FrameCount(), 3ull);
    EXPECT_NEAR(clock->ElapsedSeconds(), 1.0, 1e-6);        // 0.5 + 0.25 + 0.25
    EXPECT_NEAR(clock->LastDeltaSeconds(), 0.25f, 1e-6f);

    // Deinitialize で消え、再 Initialize で 0 から。
    world.Deinitialize();
    EXPECT_TRUE(world.Get<FWorldClockSubsystem>() == nullptr);
    world.Initialize(ESubsystemScope::World);
    FWorldClockSubsystem* reinit = world.Get<FWorldClockSubsystem>();
    EXPECT_TRUE(reinit != nullptr);
    if (reinit != nullptr) {
        EXPECT_NEAR(reinit->ElapsedSeconds(), 0.0, 1e-9);
        EXPECT_EQ(reinit->FrameCount(), 0ull);
    }
}
