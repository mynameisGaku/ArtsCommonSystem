// ECS の動作確認テスト
#include "test/Test.h"
#include "test/Expect.h"
#include "ecs/World.h"
#include "ecs/Query.h"
#include "ecs/System.h"

using namespace acs;

namespace {
struct Position { f32 x, y, z; };
struct Velocity { f32 dx, dy, dz; };
struct Health   { i32 hp; };
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
