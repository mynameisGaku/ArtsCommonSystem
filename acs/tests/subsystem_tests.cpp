// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: サブシステム基盤の検証 (GPU 非依存)。
//   ・FSubsystemCollection が登録簿から «該当スコープ» のサブシステムを全生成し
//     OnInitialize する
//   ・Get<T>() が自スコープ → 上位スコープ (World→GameInstance→Engine) へフォール
//     バック検索する。上位から下位は «見えない»
//   ・OnTick がフレーム毎に呼ばれ、OnDeinitialize が解体時に呼ばれる
//   ・FNode2D / FComponent2D の GetSubsystem<T>() が root 配線を walk-to-root で
//     解決する (= ワールドのオブジェクト同士がサブシステム経由でやり取りできる)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Subsystem.h"
#include "gameframework/SubsystemRegistry.h"
#include "gameframework/SubsystemCollection.h"
#include "gameframework/Node2D.h"
#include "gameframework/Component2D.h"
#include "memory/UniquePtr.h"

using namespace acs;
using namespace acs::game;

namespace {

// World スコープ: スコア管理 (オブジェクト間のやり取りのハブを想定)。
class TScoreSub : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(TScoreSub)
    int score  = -999;
    int ticks  = 0;
    int deinit = 0;
    void OnInitialize()   noexcept override { score = 0; }
    void OnDeinitialize() noexcept override { ++deinit; }
    void OnTick(f32)      noexcept override { ++ticks; }
    void Add(int n)       noexcept { score += n; }
};

// GameInstance スコープ: シーン跨ぎの設定 (シングルトン)。
class TConfigSub : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(TConfigSub)
    int volume = 0;
    void OnInitialize() noexcept override { volume = 50; }
};

// Engine スコープ: アプリ全体寿命。
class TEngineSub : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(TEngineSub)
    bool inited = false;
    void OnInitialize() noexcept override { inited = true; }
};

// コンポーネントからサブシステムを引いて使うテストコンポーネント。
struct TProbeComp : public FComponent2D {
    ACS_GAME_COMPONENT_KIND(TProbeComp)
};

} // namespace

ACS_REGISTER_SUBSYSTEM(TScoreSub,  ESubsystemScope::World)
ACS_REGISTER_SUBSYSTEM(TConfigSub, ESubsystemScope::GameInstance)
ACS_REGISTER_SUBSYSTEM(TEngineSub, ESubsystemScope::Engine)

namespace {

// Engine→GameInstance→World の親チェーンで 3 コレクションを初期化するヘルパ。
struct Stack {
    FSubsystemCollection engine, gameInst, world;
    Stack() noexcept {
        engine.Initialize(ESubsystemScope::Engine);
        gameInst.Initialize(ESubsystemScope::GameInstance, &engine);
        world.Initialize(ESubsystemScope::World, &gameInst);
    }
};

} // namespace

// 各スコープのサブシステムが «該当スコープのコレクションにのみ» 生成される。
ACS_TEST(Subsystem, InstantiatedPerScope) {
    Stack s;
    EXPECT_TRUE(s.world.Get<TScoreSub>()     != nullptr);
    EXPECT_TRUE(s.gameInst.Get<TConfigSub>() != nullptr);
    EXPECT_TRUE(s.engine.Get<TEngineSub>()   != nullptr);
    // OnInitialize が走っている。
    EXPECT_EQ(s.world.Get<TScoreSub>()->score, 0);
    EXPECT_EQ(s.gameInst.Get<TConfigSub>()->volume, 50);
    EXPECT_TRUE(s.engine.Get<TEngineSub>()->inited);
}

// 下位スコープから上位スコープを取得できる (フォールバック)。上位→下位は不可。
ACS_TEST(Subsystem, ScopeFallback) {
    Stack s;
    // World から GameInstance / Engine が見える。
    EXPECT_TRUE(s.world.Get<TConfigSub>() != nullptr);
    EXPECT_TRUE(s.world.Get<TEngineSub>() != nullptr);
    // GameInstance から Engine は見えるが World は見えない。
    EXPECT_TRUE(s.gameInst.Get<TEngineSub>() != nullptr);
    EXPECT_TRUE(s.gameInst.Get<TScoreSub>()  == nullptr);
    // Engine から下位は見えない。
    EXPECT_TRUE(s.engine.Get<TConfigSub>() == nullptr);
    EXPECT_TRUE(s.engine.Get<TScoreSub>()  == nullptr);
}

// Tick が毎フレーム呼ばれる。
ACS_TEST(Subsystem, TickForwards) {
    Stack s;
    s.world.Tick(0.016f);
    s.world.Tick(0.016f);
    s.world.Tick(0.016f);
    EXPECT_EQ(s.world.Get<TScoreSub>()->ticks, 3);
}

// Deinitialize で OnDeinitialize が呼ばれ、以後取得不可になる。冪等。
ACS_TEST(Subsystem, DeinitializeTearsDown) {
    FSubsystemCollection world;
    world.Initialize(ESubsystemScope::World);
    EXPECT_TRUE(world.Get<TScoreSub>() != nullptr);
    world.Deinitialize();
    EXPECT_TRUE(world.Get<TScoreSub>() == nullptr);
    world.Deinitialize();   // 冪等 (二重解体しても安全)
}

// ノード/コンポーネントが root 配線のサブシステムを walk-to-root で取得できる
// (= ワールドのオブジェクト同士がサブシステム経由でやり取りできる)。
ACS_TEST(Subsystem, NodeAndComponentAccess) {
    Stack s;
    FNode2D root;
    root._SetSubsystems(&s.world);                          // root にのみ配線
    FNode2D& child = root.AddChild(MakeUnique<FNode2D>());
    TProbeComp& comp = child.AddComponent<TProbeComp>();

    // 子ノードから (walk-to-root)。
    EXPECT_TRUE(child.GetSubsystem<TScoreSub>()  != nullptr);
    EXPECT_TRUE(child.GetSubsystem<TConfigSub>() != nullptr);   // 上位へフォールバック
    // コンポーネントから (owner → root)。
    EXPECT_TRUE(comp.GetSubsystem<TScoreSub>() != nullptr);

    // コンポーネント経由でスコアを加算 → 同じ World サブシステムへ反映される。
    comp.GetSubsystem<TScoreSub>()->Add(7);
    comp.GetSubsystem<TScoreSub>()->Add(3);
    EXPECT_EQ(s.world.Get<TScoreSub>()->score, 10);

    // 未配線ノードは nullptr。
    FNode2D lonely;
    EXPECT_TRUE(lonely.GetSubsystem<TScoreSub>() == nullptr);
}
