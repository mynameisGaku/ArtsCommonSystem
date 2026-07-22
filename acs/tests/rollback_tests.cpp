// SPDX-License-Identifier: Apache-2.0
// FRollbackBuffer (ECS World スナップショット履歴) の動作確認テスト
//
// リング上書き / 復元契約 / 非コピー型の拒否に加えて、FLockstep (入力履歴) と
// 組み合わせた GGPO 風の「予測ミス → 巻き戻し → 権威入力で再シミュレーション」
// ループが権威フルシミュレーションと bit 一致することを検証する。
#include "test/Test.h"
#include "test/Expect.h"
#include "ecs/World.h"
#include "ecs/Query.h"
#include "ecs/RollbackBuffer.h"
#include "gameframework/Lockstep.h"

using namespace acs;

namespace {

struct FGridPos { i32 x = 0; i32 y = 0; };
struct FScore   { u32 v = 0; };

/** 非コピー・可ムーブなコンポーネント (SaveFrame の拒否契約検証用)。 */
struct FRbMoveOnlyComp {
    u32 v = 0;
    FRbMoveOnlyComp() = default;
    explicit FRbMoveOnlyComp(u32 x) noexcept : v(x) {}
    FRbMoveOnlyComp(const FRbMoveOnlyComp&) = delete;
    FRbMoveOnlyComp& operator=(const FRbMoveOnlyComp&) = delete;
    FRbMoveOnlyComp(FRbMoveOnlyComp&&) noexcept = default;
    FRbMoveOnlyComp& operator=(FRbMoveOnlyComp&&) noexcept = default;
};

/**
 * 決定論的な 1 tick 分のシミュレーション (整数のみ、入力依存)。
 *
 * @param w 進める World。
 * @param in この tick の入力。
 */
void SimStep(FWorld& w, const game::FInputFrame& in) noexcept
{
    w.Query<FGridPos>().Each([&](FEntityId, FGridPos& p) {
        p.x += (in.buttons & 0x1) ? 2 : 1;
        p.y += (in.buttons & 0x2) ? -1 : 1;
    });
    w.Query<FScore>().Each([&](FEntityId, FScore& s) {
        s.v = s.v * 31u + in.buttons;
    });
}

} // namespace

ACS_TEST(RollbackBuffer, InitShutdownContract) {
    FRollbackBuffer rb;
    EXPECT_FALSE(rb.Init(0));            // 容量 0 は拒否
    EXPECT_EQ(rb.Capacity(), 0u);

    FWorld w;
    EXPECT_FALSE(rb.SaveFrame(0, w));    // 未初期化では保存できない
    EXPECT_FALSE(rb.RestoreFrame(0, w));

    EXPECT_TRUE(rb.Init(4));
    EXPECT_EQ(rb.Capacity(), 4u);
    EXPECT_EQ(rb.SavedCount(), 0u);

    EXPECT_TRUE(rb.SaveFrame(0, w));
    EXPECT_TRUE(rb.Init(2));             // 再 Init は履歴を破棄して確保し直す
    EXPECT_EQ(rb.Capacity(), 2u);
    EXPECT_EQ(rb.SavedCount(), 0u);

    rb.Shutdown();
    EXPECT_EQ(rb.Capacity(), 0u);
    rb.Shutdown();                       // 二重 Shutdown も安全
}

ACS_TEST(RollbackBuffer, SaveRestoreRoundtrip) {
    FWorld w;
    FEntityId a = w.Create();
    FEntityId b = w.Create();
    w.Add<FGridPos>(a, {10, 20});
    w.Add<FGridPos>(b, {-5, 7});

    FRollbackBuffer rb;
    EXPECT_TRUE(rb.Init(4));
    EXPECT_TRUE(rb.SaveFrame(5, w));
    EXPECT_TRUE(rb.HasFrame(5));
    EXPECT_EQ(rb.SavedCount(), 1u);

    // snapshot 後に破壊的変更: b を破棄し、a を書き換え、新規 c を作る。
    w.Destroy(b);
    if (FGridPos* p = w.Get<FGridPos>(a)) { p->x = 999; }
    FEntityId c = w.Create();
    w.Add<FGridPos>(c, {1, 1});

    EXPECT_TRUE(rb.RestoreFrame(5, w));

    // snapshot 時の EntityId はそのまま有効で、値も snapshot 時点に戻る。
    EXPECT_TRUE(w.IsAlive(a));
    EXPECT_TRUE(w.IsAlive(b));
    EXPECT_FALSE(w.IsAlive(c));          // snapshot 後に作った ID は無効化される
    const FGridPos* pa = w.Get<FGridPos>(a);
    const FGridPos* pb = w.Get<FGridPos>(b);
    EXPECT_TRUE(pa != nullptr && pb != nullptr);
    if (pa) { EXPECT_EQ(pa->x, 10); EXPECT_EQ(pa->y, 20); }
    if (pb) { EXPECT_EQ(pb->x, -5); EXPECT_EQ(pb->y, 7); }

    // 履歴は復元後も残り、同じ tick へ複数回巻き戻せる。
    EXPECT_TRUE(rb.HasFrame(5));
    EXPECT_TRUE(rb.RestoreFrame(5, w));
}

ACS_TEST(RollbackBuffer, RingEvictsOldTicks) {
    FWorld w;
    FEntityId e = w.Create();
    w.Add<FGridPos>(e, {0, 0});

    FRollbackBuffer rb;
    EXPECT_TRUE(rb.Init(4));
    for (u32 t = 0; t <= 5; ++t) {
        if (FGridPos* p = w.Get<FGridPos>(e)) { p->x = static_cast<i32>(t); }
        EXPECT_TRUE(rb.SaveFrame(t, w));
    }

    // 容量 4 なので tick 0,1 は 4,5 に上書き済み。
    EXPECT_FALSE(rb.HasFrame(0));
    EXPECT_FALSE(rb.HasFrame(1));
    EXPECT_TRUE(rb.HasFrame(2));
    EXPECT_TRUE(rb.HasFrame(5));
    EXPECT_EQ(rb.SavedCount(), 4u);
    EXPECT_FALSE(rb.RestoreFrame(0, w));

    EXPECT_TRUE(rb.RestoreFrame(3, w));
    const FGridPos* p = w.Get<FGridPos>(e);
    EXPECT_TRUE(p != nullptr);
    if (p) EXPECT_EQ(p->x, 3);

    rb.InvalidateAll();
    EXPECT_EQ(rb.SavedCount(), 0u);
    EXPECT_FALSE(rb.RestoreFrame(3, w));
    EXPECT_EQ(rb.Capacity(), 4u);        // 容量は保持
}

ACS_TEST(RollbackBuffer, NonCopyableComponentRejected) {
    FWorld good;
    FEntityId g = good.Create();
    good.Add<FGridPos>(g, {1, 2});

    FWorld bad;
    FEntityId e = bad.Create();
    bad.Add<FRbMoveOnlyComp>(e, FRbMoveOnlyComp{7});

    FRollbackBuffer rb;
    EXPECT_TRUE(rb.Init(2));
    EXPECT_TRUE(rb.SaveFrame(3, good));
    EXPECT_TRUE(rb.HasFrame(3));

    // 同じ slot への失敗保存は、古い有効履歴も無効化する (壊れた復元をさせない)。
    EXPECT_FALSE(rb.SaveFrame(3, bad));
    EXPECT_FALSE(rb.HasFrame(3));
    FWorld sink;
    EXPECT_FALSE(rb.RestoreFrame(3, sink));
}

ACS_TEST(RollbackBuffer, LockstepResimulationConverges) {
    // 権威入力列: tick ごとにボタンパターンが変わる。
    constexpr u32 kTicks    = 20;
    constexpr u32 kRollback = 10;   // ここから先の予測が外れていたことにする
    const auto AuthButtons = [](u32 t) noexcept -> u8 {
        return static_cast<u8>((t * 7u + 3u) & 0x3u);
    };

    // --- 権威フルシミュレーション (正解) -----------------------------------
    FWorld auth;
    FEntityId a1 = auth.Create();
    FEntityId a2 = auth.Create();
    auth.Add<FGridPos>(a1, {0, 0});
    auth.Add<FGridPos>(a2, {100, -100});
    auth.Add<FScore>(a2, {1});
    for (u32 t = 0; t < kTicks; ++t) {
        game::FInputFrame f{t, 0, AuthButtons(t), {}};
        SimStep(auth, f);
    }

    // --- クライアント側: 権威入力を FLockstep に記録しつつ予測実行 ---------
    game::FLockstep ls;
    ls.Init(game::ENetMode::Local, 60);

    FWorld sim;
    FEntityId s1 = sim.Create();
    FEntityId s2 = sim.Create();
    sim.Add<FGridPos>(s1, {0, 0});
    sim.Add<FGridPos>(s2, {100, -100});
    sim.Add<FScore>(s2, {1});

    FRollbackBuffer history;
    EXPECT_TRUE(history.Init(16));

    for (u32 t = 0; t < kTicks; ++t) {
        ls.RecordInput(game::FInputFrame{t, 0, AuthButtons(t), {}});  // 権威入力 (後で届く体)
        EXPECT_TRUE(history.SaveFrame(t, sim));
        // kRollback 以降は予測が外れている: ボタン無しで進めてしまう。
        const u8 predicted = (t < kRollback) ? AuthButtons(t) : u8{0};
        game::FInputFrame f{t, 0, predicted, {}};
        SimStep(sim, f);
    }

    // 予測ミスにより権威状態とはズレている。
    {
        const FGridPos* pa = auth.Get<FGridPos>(a1);
        const FGridPos* ps = sim.Get<FGridPos>(s1);
        EXPECT_TRUE(pa != nullptr && ps != nullptr);
        if (pa && ps) EXPECT_TRUE(pa->x != ps->x || pa->y != ps->y);
    }

    // --- 権威入力が届いた: kRollback へ巻き戻して再シミュレーション --------
    ls.StartReplay();
    EXPECT_TRUE(history.RestoreFrame(kRollback, sim));
    for (u32 t = kRollback; t < kTicks; ++t) {
        EXPECT_TRUE(history.SaveFrame(t, sim));   // 履歴も正しい系列で上書き
        game::FInputFrame f;
        // Replay cursor は tick 昇順消費を仮定するため、先頭から該当 tick まで進める。
        const bool found = ls.ConsumeInput(t, 0, f);
        EXPECT_TRUE(found);
        if (found) SimStep(sim, f);
    }

    // 再シミュレーション後は権威フルシミュレーションと完全一致する。
    const FGridPos* pa1 = auth.Get<FGridPos>(a1);
    const FGridPos* ps1 = sim.Get<FGridPos>(s1);
    const FGridPos* pa2 = auth.Get<FGridPos>(a2);
    const FGridPos* ps2 = sim.Get<FGridPos>(s2);
    const FScore*   ka2 = auth.Get<FScore>(a2);
    const FScore*   ks2 = sim.Get<FScore>(s2);
    EXPECT_TRUE(pa1 && ps1 && pa2 && ps2 && ka2 && ks2);
    if (pa1 && ps1) { EXPECT_EQ(ps1->x, pa1->x); EXPECT_EQ(ps1->y, pa1->y); }
    if (pa2 && ps2) { EXPECT_EQ(ps2->x, pa2->x); EXPECT_EQ(ps2->y, pa2->y); }
    if (ka2 && ks2) { EXPECT_EQ(ks2->v, ka2->v); }
}
