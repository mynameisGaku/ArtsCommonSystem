// SPDX-License-Identifier: Apache-2.0
// CProjectileSystem のコールバック再入 (callback 内 Despawn) の回帰テスト
//
// OnHit / OnExpire コールバック内で同じ弾を Despawn すると、修正前は Tick 側の
// 内部 despawn と二重実行になり m_AliveCount が二重デクリメントされていた
// (AllAlive が生存弾を取りこぼす)。BuffSystem/CinematicsDirector と同族の
// 再入パターン。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ProjectileSystem.h"

using namespace acs;
using namespace acs::game;

namespace {

/** コールバックから System 本体へ再入するためのフィクスチャ。 */
struct FProjFixture {
    CProjectileSystem* sys = nullptr;
    u32 hit_owner  = 0;   // この owner_id の弾だけ命中させる
    u32 hit_fired  = 0;
    u32 expire_fired = 0;
};

/** owner_id == hit_owner の弾だけ命中させる判定関数。 */
bool HitOnlyOwner(void* user, const FProjectileInstance& proj,
                  u32& out_target, f32& out_damage) noexcept
{
    FProjFixture* fx = static_cast<FProjFixture*>(user);
    out_target = 42u;
    out_damage = proj.damage;
    return proj.owner_id == fx->hit_owner;
}

/** 命中した弾を callback 内で Despawn する (再入)。 */
void DespawnOnHit(void* user, FProjectileId id, const char* /*def*/,
                  u32 /*target*/, f32 /*damage*/) noexcept
{
    FProjFixture* fx = static_cast<FProjFixture*>(user);
    ++fx->hit_fired;
    fx->sys->Despawn(id);
}

/** 寿命切れの弾を callback 内で Despawn する (再入)。 */
void DespawnOnExpire(void* user, FProjectileId id, const char* /*def*/) noexcept
{
    FProjFixture* fx = static_cast<FProjFixture*>(user);
    ++fx->expire_fired;
    fx->sys->Despawn(id);
}

} // namespace

ACS_TEST(ProjectileReentrancy, HitCallbackDespawnKeepsCountConsistent) {
    CProjectileSystem sys;
    sys.Init(8);

    FProjectileDef def{};
    def.id           = "bullet";
    def.lifetime_sec = 10.0f;
    sys.RegisterDef(def);

    FProjFixture fx;
    fx.sys       = &sys;
    fx.hit_owner = 1;
    sys.SetHitTestFn(&HitOnlyOwner, &fx);
    sys.SetOnHitCallback(&DespawnOnHit, &fx);

    const FProjectileId a = sys.Spawn("bullet", {0, 0}, {1, 0}, /*owner=*/1, 5.0f);
    const FProjectileId b = sys.Spawn("bullet", {0, 0}, {1, 0}, /*owner=*/2, 5.0f);
    EXPECT_TRUE(a.IsValid() && b.IsValid());
    EXPECT_EQ(sys.AliveCount(), 2u);

    sys.Tick(0.016f);   // a のみ命中 → callback が Despawn(a)

    EXPECT_EQ(fx.hit_fired, 1u);
    // 修正前: 内部 despawn と二重実行で AliveCount が 0 になっていた。
    EXPECT_EQ(sys.AliveCount(), 1u);
    u32 count = 0;
    const FProjectileInstance* alive = sys.AllAlive(count);
    EXPECT_EQ(count, 1u);
    EXPECT_TRUE(alive != nullptr);
    if (alive) EXPECT_EQ(alive[0].owner_id, 2u);
    EXPECT_TRUE(sys.GetInstance(a) == nullptr);
    EXPECT_TRUE(sys.GetInstance(b) != nullptr);
}

ACS_TEST(ProjectileReentrancy, ExpireCallbackDespawnKeepsCountConsistent) {
    CProjectileSystem sys;
    sys.Init(4);

    FProjectileDef def{};
    def.id           = "spark";
    def.lifetime_sec = 0.5f;
    sys.RegisterDef(def);

    FProjFixture fx;
    fx.sys = &sys;
    sys.SetOnExpireCallback(&DespawnOnExpire, &fx);

    const FProjectileId a = sys.Spawn("spark", {0, 0}, {0, 0}, 1, 0.0f);
    const FProjectileId b = sys.Spawn("spark", {0, 0}, {0, 0}, 2, 0.0f);
    EXPECT_TRUE(a.IsValid() && b.IsValid());

    sys.Tick(1.0f);     // 両方寿命切れ → 各 callback が Despawn (再入)

    EXPECT_EQ(fx.expire_fired, 2u);
    EXPECT_EQ(sys.AliveCount(), 0u);
    u32 count = 0;
    (void)sys.AllAlive(count);
    EXPECT_EQ(count, 0u);

    // pool が壊れていなければ同数を再発射できる。
    EXPECT_TRUE(sys.Spawn("spark", {0, 0}, {0, 0}, 3, 0.0f).IsValid());
    EXPECT_TRUE(sys.Spawn("spark", {0, 0}, {0, 0}, 4, 0.0f).IsValid());
    EXPECT_EQ(sys.AliveCount(), 2u);
}
