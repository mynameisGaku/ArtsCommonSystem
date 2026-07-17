// SPDX-License-Identifier: Apache-2.0
// ECS の動作確認テスト
#include "test/Test.h"
#include "test/Expect.h"
#include "ecs/World.h"
#include "ecs/Query.h"
#include "ecs/System.h"
#include "ecs/EntityCommandBuffer.h"
#include "ecs/ParallelEntityCommandBuffer.h"
#include "threading/ThreadPool.h"

using namespace acs;

namespace {
struct Position { f32 x, y, z; };
struct Velocity { f32 dx, dy, dz; };
struct Health   { i32 hp; };
struct Frozen   { u8 unused; };  // タグ的コンポーネント (除外フィルタ検証用)

/** 常に確保失敗する backing (command buffer の OOM 契約検証用)。 */
class FEcbFailAllocator final : public FAllocator {
public:
    void* Alloc(usize /*Size*/, usize /*Alignment*/, FSourceLoc /*Location*/) noexcept override
    {
        return nullptr;
    }

    void Free(void* /*Pointer*/) noexcept override
    {
    }
};
}

ACS_TEST(Ecs, CreateDestroy) {
    World w;
    EntityId a = w.Create();
    EntityId b = w.Create();
    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));
    EXPECT_EQ(w.EntityCount(), 2u);
    w.Destroy(a);
    EXPECT_FALSE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));
}

ACS_TEST(Ecs, AddGetRemoveComponent) {
    World w;
    EntityId e = w.Create();
    w.Add<Position>(e, {1, 2, 3});
    EXPECT_TRUE(w.Has<Position>(e));
    Position* p = w.Get<Position>(e);
    EXPECT_TRUE(p != nullptr);
    if (p) EXPECT_NEAR(p->x, 1.0f, 1e-5f);
    w.Remove<Position>(e);
    EXPECT_FALSE(w.Has<Position>(e));
}

ACS_TEST(Ecs, GenerationInvalidatesOldId) {
    World w;
    EntityId a = w.Create();
    w.Destroy(a);
    EntityId b = w.Create();  // 同じ index を再利用するが世代が違う
    EXPECT_FALSE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));
}

ACS_TEST(Ecs, ClearReleasesAllStateAndAllowsReuse)
{
    World world;
    const EntityId old_entity = world.Create();
    world.Add<Position>(old_entity, Position{1.0f, 2.0f, 3.0f});
    EXPECT_EQ(world.EntityCount(), 1u);

    world.Clear();
    world.Clear();
    EXPECT_EQ(world.EntityCount(), 0u);
    EXPECT_TRUE(!world.IsAlive(old_entity));

    const EntityId new_entity = world.Create();
    world.Add<Position>(new_entity, Position{4.0f, 5.0f, 6.0f});
    EXPECT_TRUE(world.IsAlive(new_entity));
    EXPECT_TRUE(!world.IsAlive(old_entity));
    EXPECT_TRUE(world.Get<Position>(new_entity) != nullptr);
}

ACS_TEST(Ecs, QueryIteratesMatching) {
    World w;
    for (int i = 0; i < 100; ++i) {
        EntityId e = w.Create();
        w.Add<Position>(e, {(f32)i, 0, 0});
        if (i % 2 == 0) w.Add<Velocity>(e, {1, 0, 0});
    }
    u32 count = 0;
    w.Query<Position, Velocity>().Each([&count](EntityId, Position&, Velocity&){ ++count; });
    EXPECT_EQ(count, 50u);  // 偶数 i のみ Velocity を持つ
}

ACS_TEST(Ecs, SystemSchedulerRuns) {
    World w;
    for (int i = 0; i < 10; ++i) {
        EntityId e = w.Create();
        w.Add<Position>(e, {0, 0, 0});
        w.Add<Velocity>(e, {1, 0, 0});
    }
    SystemScheduler s;
    s.Add([](World& w, f32 dt){
        w.Query<Position, Velocity>().Each([dt](EntityId, Position& p, Velocity& v){
            p.x += v.dx * dt;
        });
    });
    s.Tick(w, 1.0f);
    s.Tick(w, 1.0f);
    bool all_two = true;
    w.Query<Position>().Each([&all_two](EntityId, Position& p){
        if (p.x != 2.0f) all_two = false;
    });
    EXPECT_TRUE(all_two);
}

ACS_TEST(Ecs, EntityCommandBufferDefersAndAppliesStructuralChanges)
{
    World w;
    const EntityId a = w.Create();
    const EntityId b = w.Create();
    const EntityId c = w.Create();
    w.Add<Health>(a, {10});
    w.Add<Health>(b, {0});
    w.Add<Health>(c, {-5});

    FEntityCommandBuffer cmd(w);
    // 反復中は構造変更せず記録だけ。渡された Health& は反復中ずっと有効 (dangling しない)。
    w.Query<Health>().Each([&cmd](EntityId e, Health& h) {
        if (h.hp <= 0) {
            cmd.Destroy(e);                          // hp<=0 のエンティティを破棄予約
        } else {
            cmd.Add<Velocity>(e, {1.0f, 0.0f, 0.0f}); // 生存者へ Velocity 追加予約
        }
    });

    // 反復中は何も適用されていない。
    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));
    EXPECT_TRUE(w.IsAlive(c));
    EXPECT_FALSE(w.Has<Velocity>(a));
    EXPECT_EQ(cmd.Size(), static_cast<usize>(3));
    EXPECT_FALSE(cmd.HasOverflowed());

    cmd.Flush();
    EXPECT_EQ(cmd.Size(), static_cast<usize>(0));

    // hp<=0 の b,c は破棄され、a は生存 + Velocity 付与。
    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_FALSE(w.IsAlive(b));
    EXPECT_FALSE(w.IsAlive(c));
    EXPECT_TRUE(w.Has<Velocity>(a));
    Velocity* const v = w.Get<Velocity>(a);
    EXPECT_TRUE(v != nullptr);
    if (v) EXPECT_NEAR(v->dx, 1.0f, 1e-5f);
}

ACS_TEST(Ecs, EntityCommandBufferRemoveAndClearDiscardsWithoutApplying)
{
    World w;
    const EntityId e = w.Create();
    w.Add<Position>(e, {1, 2, 3});
    w.Add<Velocity>(e, {4, 5, 6});

    {
        FEntityCommandBuffer cmd(w);
        cmd.Remove<Velocity>(e);
        cmd.Add<Health>(e, {99});
        // Clear は適用せず破棄する (退避した Health 値も解放される)。
        cmd.Clear();
        EXPECT_EQ(cmd.Size(), static_cast<usize>(0));
    }
    // Clear したので World は変わらない。
    EXPECT_TRUE(w.Has<Velocity>(e));
    EXPECT_FALSE(w.Has<Health>(e));

    // Flush 経路の Remove も確認する。
    FEntityCommandBuffer cmd2(w);
    cmd2.Remove<Velocity>(e);
    cmd2.Flush();
    EXPECT_FALSE(w.Has<Velocity>(e));
    EXPECT_TRUE(w.Has<Position>(e));
}

ACS_TEST(Ecs, EntityCommandBufferGracefullyHandlesOutOfMemory)
{
    World w;
    const EntityId e = w.Create();

    // command buffer 自身のストレージを常時失敗 backing に載せると、記録は落ちるが
    // クラッシュせず HasOverflowed() で検知でき、Flush も安全 (何も適用しない)。
    FEcbFailAllocator failing;
    FEntityCommandBuffer cmd(w, failing);
    cmd.Destroy(e);              // TryPushBack が失敗
    cmd.Add<Health>(e, {5});     // New が失敗 (値の退避不可)
    cmd.Remove<Health>(e);       // TryPushBack が失敗

    EXPECT_TRUE(cmd.HasOverflowed());
    EXPECT_EQ(cmd.Size(), static_cast<usize>(0));

    cmd.Flush();                 // 記録ゼロなので no-op、クラッシュしない
    EXPECT_TRUE(w.IsAlive(e));    // Destroy は落ちたのでエンティティは生存
}

ACS_TEST(Ecs, ParallelCommandBufferRecordsFromWorkersAndApplies)
{
    // EachParallel の fn 内から FParallelEntityCommandBuffer へロックなしで記録し、
    // 完了後の Flush で一括適用できることを検証する (per-worker スロット分離の実地確認)。
    EXPECT_TRUE(FThreadPool::Init(4).IsOk());
    {
        World w;
        constexpr u32 kCount = 2000u;
        for (u32 i = 0; i < kCount; ++i) {
            const EntityId e = w.Create();
            w.Add<Health>(e, {static_cast<i32>(i % 3u)});   // hp: 0,1,2,0,1,2,...
        }

        FParallelEntityCommandBuffer cmd(w);
        EXPECT_TRUE(cmd.IsValid());

        // grain を小さくして複数ワーカー + 呼び出し元 (Wait 中の steal) に分散させる。
        w.Query<Health>().EachParallel([&cmd](EntityId e, Health& h) {
            if (h.hp <= 0) {
                cmd.Destroy(e);                              // hp==0 (1/3) を破棄予約
            } else {
                cmd.Add<Velocity>(e, {1.0f, 0.0f, 0.0f});    // 残り 2/3 へ Velocity 追加予約
            }
        }, 64u);

        // EachParallel 中は何も適用されない。
        EXPECT_EQ(w.EntityCount(), kCount);
        EXPECT_EQ(cmd.Size(), static_cast<usize>(kCount));
        EXPECT_FALSE(cmd.HasOverflowed());

        cmd.Flush();
        EXPECT_EQ(cmd.Size(), static_cast<usize>(0));

        // hp==0 は 667 体 (i%3==0: 0,3,...,1998)、残り 1333 体に Velocity。
        constexpr u32 kDestroyed = (kCount + 2u) / 3u;
        EXPECT_EQ(w.EntityCount(), kCount - kDestroyed);
        u32 with_velocity = 0;
        w.Query<Velocity>().Each([&with_velocity](EntityId, Velocity&) { ++with_velocity; });
        EXPECT_EQ(with_velocity, kCount - kDestroyed);
    }
    FThreadPool::Shutdown();
}

ACS_TEST(Ecs, ParallelCommandBufferWorksWithoutThreadPool)
{
    // プール未初期化でも構築できる (スロット = 非ワーカー用の 1 本)。逐次 Each からの
    // 記録・Flush が単体の FEntityCommandBuffer と同じに動くことを確認する。
    World w;
    const EntityId a = w.Create();
    const EntityId b = w.Create();
    w.Add<Health>(a, {5});
    w.Add<Health>(b, {0});

    FParallelEntityCommandBuffer cmd(w);
    EXPECT_TRUE(cmd.IsValid());
    w.Query<Health>().Each([&cmd](EntityId e, Health& h) {
        if (h.hp <= 0) cmd.Destroy(e);
        else           cmd.Add<Velocity>(e, {2.0f, 0.0f, 0.0f});
    });
    EXPECT_EQ(cmd.Size(), static_cast<usize>(2));
    cmd.Flush();

    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_FALSE(w.IsAlive(b));
    EXPECT_TRUE(w.Has<Velocity>(a));

    // Clear は適用せず破棄する (Add の退避値もリークしない — ASan/CRT リーク検査で担保)。
    cmd.Add<Health>(a, {99});
    EXPECT_EQ(cmd.Size(), static_cast<usize>(1));
    cmd.Clear();
    EXPECT_EQ(cmd.Size(), static_cast<usize>(0));
    EXPECT_EQ(w.Get<Health>(a)->hp, 5);
}

ACS_TEST(Ecs, QueryEachExcludingSkipsEntitiesWithExcludedComponents)
{
    World w;
    for (int i = 0; i < 20; ++i) {
        const EntityId e = w.Create();
        w.Add<Position>(e, {static_cast<f32>(i), 0, 0});
        if (i % 5 == 0) w.Add<Frozen>(e, {0});  // 0,5,10,15 を Frozen に
    }

    u32 visited = 0;
    u32 frozen_visited = 0;
    w.Query<Position>().EachExcluding<Frozen>([&](EntityId e, Position&) {
        ++visited;
        if (w.Has<Frozen>(e)) ++frozen_visited;  // 除外されるので 0 のはず
    });
    EXPECT_EQ(visited, 16u);        // 20 - 4 (Frozen) = 16
    EXPECT_EQ(frozen_visited, 0u);  // Frozen は 1 つも訪問しない

    // 空の Excludes は Each と同じ (全 Position を訪問)。
    u32 all = 0;
    w.Query<Position>().EachExcluding<>([&](EntityId, Position&) { ++all; });
    EXPECT_EQ(all, 20u);
}
