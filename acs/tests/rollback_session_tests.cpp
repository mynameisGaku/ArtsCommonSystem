// SPDX-License-Identifier: Apache-2.0
// FRollbackSession (rollback netcode 統合層) の動作確認テスト
//
// 予測 (繰り返し) / 遅延確定入力の bit 一致省略 / 誤予測の自動巻き戻し再実行 /
// 予測上限 (lockstep 退化) / リング追い出し後の入力拒否を検証する。
#include "test/Test.h"
#include "test/Expect.h"
#include "ecs/World.h"
#include "ecs/Query.h"
#include "gameframework/RollbackSession.h"

using namespace acs;
using namespace acs::game;

namespace {

struct FPosC { i32 x = 0; i32 y = 0; };
struct FSeqC { u32 h = 0; };

/** 1 World 分のテストフィクスチャ (sim コールバックの user データ)。 */
struct FRbFixture {
    FWorld*   world = nullptr;
    FEntityId players[2] = {};
    FEntityId seq = {};
    u32      player_count = 2;

    void Setup(FWorld& w, u32 count) noexcept
    {
        world        = &w;
        player_count = count;
        for (u32 p = 0; p < count; ++p) {
            players[p] = w.Create();
            w.Add<FPosC>(players[p], {static_cast<i32>(p) * 100, 0});
        }
        seq = w.Create();
        w.Add<FSeqC>(seq, {1});
    }
};

/** 決定論 sim: 各プレイヤーの buttons を座標へ積み、順序依存ハッシュも更新する。 */
void SessionSim(FWorld& w, u32 tick, const FInputFrame* inputs, u32 input_count, void* user) noexcept
{
    FRbFixture* fx = static_cast<FRbFixture*>(user);
    for (u32 p = 0; p < input_count; ++p) {
        if (FPosC* c = w.Get<FPosC>(fx->players[p])) {
            c->x += static_cast<i32>(inputs[p].buttons);
            c->y += 1;
        }
    }
    if (FSeqC* s = w.Get<FSeqC>(fx->seq)) {
        for (u32 p = 0; p < input_count; ++p) s->h = s->h * 31u + inputs[p].buttons;
        s->h += tick;
    }
}

/** InputFrame を組む補助。 */
FInputFrame MakeInput(u32 tick, u32 player, u8 buttons) noexcept
{
    FInputFrame f{};
    f.tick      = tick;
    f.player_id = player;
    f.buttons   = buttons;
    return f;
}

/** 2 World の対応コンポーネントが一致するかを検証する。 */
void ExpectWorldsEqual(const FRbFixture& a, const FRbFixture& b) noexcept
{
    for (u32 p = 0; p < a.player_count; ++p) {
        const FPosC* pa = a.world->Get<FPosC>(a.players[p]);
        const FPosC* pb = b.world->Get<FPosC>(b.players[p]);
        EXPECT_TRUE(pa != nullptr && pb != nullptr);
        if (pa && pb) { EXPECT_EQ(pa->x, pb->x); EXPECT_EQ(pa->y, pb->y); }
    }
    const FSeqC* sa = a.world->Get<FSeqC>(a.seq);
    const FSeqC* sb = b.world->Get<FSeqC>(b.seq);
    EXPECT_TRUE(sa != nullptr && sb != nullptr);
    if (sa && sb) EXPECT_EQ(sa->h, sb->h);
}

} // namespace

ACS_TEST(RollbackSession, InitContract) {
    FWorld w;
    FRollbackSession s;
    FRollbackSessionConfig cfg;

    EXPECT_FALSE(s.Init(nullptr, cfg));

    cfg.player_count = 0;
    EXPECT_FALSE(s.Init(&w, cfg));
    cfg.player_count = kMaxRollbackPlayers + 1;
    EXPECT_FALSE(s.Init(&w, cfg));

    cfg.player_count   = 2;
    cfg.history_length = 0;
    EXPECT_FALSE(s.Init(&w, cfg));

    cfg.history_length = 4;
    cfg.max_prediction = 4;          // >= history は拒否
    EXPECT_FALSE(s.Init(&w, cfg));

    cfg.max_prediction = 0;
    EXPECT_TRUE(s.Init(&w, cfg));
    EXPECT_TRUE(s.IsInitialized());
    EXPECT_FALSE(s.AdvanceTick());   // コールバック未設定では進めない
    EXPECT_TRUE(s.Init(&w, cfg));    // 再 Init も可
}

ACS_TEST(RollbackSession, PredictionRepeatsLastConfirmedInput) {
    FWorld w;
    FRbFixture fx;
    fx.Setup(w, 1);

    FRollbackSession s;
    FRollbackSessionConfig cfg;
    cfg.player_count   = 1;
    cfg.history_length = 8;
    EXPECT_TRUE(s.Init(&w, cfg));
    s.SetSimCallback(&SessionSim, &fx);

    EXPECT_TRUE(s.SubmitInput(MakeInput(0, 0, 3)));
    EXPECT_TRUE(s.AdvanceTick());    // t0: 確定 3
    EXPECT_TRUE(s.AdvanceTick());    // t1: 予測 = 直前の 3
    EXPECT_TRUE(s.AdvanceTick());    // t2: 予測 = 3
    EXPECT_EQ(s.CurrentTick(), 3u);

    const FPosC* p = w.Get<FPosC>(fx.players[0]);
    EXPECT_TRUE(p != nullptr);
    if (p) { EXPECT_EQ(p->x, 9); EXPECT_EQ(p->y, 3); }

    EXPECT_EQ(s.ConfirmedFloor(), 1u);
    EXPECT_EQ(s.PredictionDepth(), 2u);

    // 予測どおりの遅延確定入力は再シミュレーションを起こさず、床だけ進む。
    EXPECT_TRUE(s.SubmitInput(MakeInput(1, 0, 3)));
    EXPECT_FALSE(s.NeedsResimulation());
    EXPECT_TRUE(s.SubmitInput(MakeInput(2, 0, 3)));
    EXPECT_FALSE(s.NeedsResimulation());
    EXPECT_EQ(s.ConfirmedFloor(), 3u);
    EXPECT_EQ(s.PredictionDepth(), 0u);
}

ACS_TEST(RollbackSession, MispredictionRollsBackAndConverges) {
    // 権威 (正解): 両プレイヤーの真の入力で 7 tick フルシミュレーション。
    const auto A = [](u32 t) noexcept -> u8 { return static_cast<u8>((t % 3u) + 1u); };
    const auto B = [](u32 t) noexcept -> u8 { return static_cast<u8>(((t * 2u) % 4u) + 1u); };
    constexpr u32 kTicks = 7;

    FWorld auth_world;
    FRbFixture auth;
    auth.Setup(auth_world, 2);
    for (u32 t = 0; t < kTicks; ++t) {
        FInputFrame in[2] = {MakeInput(t, 0, A(t)), MakeInput(t, 1, B(t))};
        SessionSim(auth_world, t, in, 2, &auth);
    }

    // セッション側: p0 は毎 tick 確定、p1 は 6 tick 分まとめて遅延到着。
    FWorld sim_world;
    FRbFixture fx;
    fx.Setup(sim_world, 2);

    FRollbackSession s;
    FRollbackSessionConfig cfg;
    cfg.player_count   = 2;
    cfg.history_length = 8;
    EXPECT_TRUE(s.Init(&sim_world, cfg));
    s.SetSimCallback(&SessionSim, &fx);

    for (u32 t = 0; t < 6; ++t) {
        EXPECT_TRUE(s.SubmitInput(MakeInput(t, 0, A(t))));
        EXPECT_TRUE(s.AdvanceTick());    // p1 は予測 (ニュートラル繰り返し) で誤予測
    }
    EXPECT_EQ(s.CurrentTick(), 6u);

    // p1 の真の入力が 0..6 まで一気に届く (0..5 は過去 = 巻き戻し要求)。
    for (u32 t = 0; t <= 6; ++t) {
        EXPECT_TRUE(s.SubmitInput(MakeInput(t, 1, B(t))));
    }
    EXPECT_TRUE(s.NeedsResimulation());
    EXPECT_TRUE(s.SubmitInput(MakeInput(6, 0, A(6))));

    // AdvanceTick が冒頭で 0..5 を自動再実行してから t6 を実行する。
    EXPECT_TRUE(s.AdvanceTick());
    EXPECT_FALSE(s.NeedsResimulation());
    EXPECT_EQ(s.CurrentTick(), 7u);
    EXPECT_EQ(s.ConfirmedFloor(), 7u);

    ExpectWorldsEqual(fx, auth);
}

ACS_TEST(RollbackSession, EvictedTickInputIsRejected) {
    FWorld w;
    FRbFixture fx;
    fx.Setup(w, 1);

    FRollbackSession s;
    FRollbackSessionConfig cfg;
    cfg.player_count   = 1;
    cfg.history_length = 4;
    EXPECT_TRUE(s.Init(&w, cfg));
    s.SetSimCallback(&SessionSim, &fx);

    for (u32 t = 0; t < 6; ++t) EXPECT_TRUE(s.AdvanceTick());   // 全 tick 予測 (ニュートラル)
    EXPECT_EQ(s.CurrentTick(), 6u);

    // 実効巻き戻し窓は history-1 = 3 tick (tick 3..5)。窓外の tick 1/2 は拒否。
    EXPECT_FALSE(s.SubmitInput(MakeInput(1, 0, 7)));
    EXPECT_FALSE(s.SubmitInput(MakeInput(2, 0, 7)));
    EXPECT_FALSE(s.NeedsResimulation());
    // まだ窓内の tick 3 は受理され、誤予測なら巻き戻しが立つ。
    EXPECT_TRUE(s.SubmitInput(MakeInput(3, 0, 7)));
    EXPECT_TRUE(s.NeedsResimulation());
    EXPECT_TRUE(s.AdvanceTick());
    EXPECT_FALSE(s.NeedsResimulation());
    EXPECT_EQ(s.CurrentTick(), 7u);

    // 現在 tick は受理、未来 tick は拒否 (トランスポート側でバッファする契約)。
    EXPECT_TRUE(s.SubmitInput(MakeInput(7, 0, 1)));
    EXPECT_FALSE(s.SubmitInput(MakeInput(8, 0, 1)));
}

ACS_TEST(RollbackSession, MaxPredictionDegradesToLockstep) {
    FWorld w;
    FRbFixture fx;
    fx.Setup(w, 2);

    FRollbackSession s;
    FRollbackSessionConfig cfg;
    cfg.player_count   = 2;
    cfg.history_length = 8;
    cfg.max_prediction = 2;
    EXPECT_TRUE(s.Init(&w, cfg));
    s.SetSimCallback(&SessionSim, &fx);

    // p0 は毎 tick 届くが p1 が沈黙 → 2 tick 先行した時点で停止する。
    EXPECT_TRUE(s.SubmitInput(MakeInput(0, 0, 1)));
    EXPECT_TRUE(s.AdvanceTick());
    EXPECT_TRUE(s.SubmitInput(MakeInput(1, 0, 1)));
    EXPECT_TRUE(s.AdvanceTick());
    EXPECT_FALSE(s.AdvanceTick());   // depth 2 >= max_prediction
    EXPECT_EQ(s.CurrentTick(), 2u);

    // p1 の入力が届けば床が進み、再び進めるようになる
    // (buttons=0 は予測と一致するので巻き戻しは起きない)。
    EXPECT_TRUE(s.SubmitInput(MakeInput(0, 1, 0)));
    EXPECT_TRUE(s.SubmitInput(MakeInput(1, 1, 0)));
    EXPECT_FALSE(s.NeedsResimulation());
    EXPECT_EQ(s.ConfirmedFloor(), 2u);
    EXPECT_TRUE(s.AdvanceTick());
    EXPECT_EQ(s.CurrentTick(), 3u);
}

ACS_TEST(RollbackSession, ResetRestartsSession) {
    FWorld w;
    FRbFixture fx;
    fx.Setup(w, 1);

    FRollbackSession s;
    FRollbackSessionConfig cfg;
    cfg.player_count   = 1;
    cfg.history_length = 4;
    EXPECT_TRUE(s.Init(&w, cfg));
    s.SetSimCallback(&SessionSim, &fx);

    EXPECT_TRUE(s.SubmitInput(MakeInput(0, 0, 2)));
    EXPECT_TRUE(s.AdvanceTick());
    EXPECT_TRUE(s.AdvanceTick());

    s.Reset(100);
    EXPECT_EQ(s.CurrentTick(), 100u);
    EXPECT_EQ(s.ConfirmedFloor(), 100u);
    EXPECT_FALSE(s.NeedsResimulation());

    // 旧 tick への入力は履歴が無いので拒否され、新 tick から通常運転できる。
    EXPECT_FALSE(s.SubmitInput(MakeInput(0, 0, 2)));
    EXPECT_TRUE(s.SubmitInput(MakeInput(100, 0, 5)));
    EXPECT_TRUE(s.AdvanceTick());
    EXPECT_EQ(s.CurrentTick(), 101u);
}
