// SPDX-License-Identifier: Apache-2.0
// CBuffSystem / CCinematicsDirector の «コールバック再入» 安全性テスト
//
// どちらも Tick/FireUpTo で配列を走査しながらユーザーコールバックを発火する。
// コールバックが同オブジェクトの ApplyBuff / Clear 等を呼んで内部配列を再確保・
// 空化しても、走査中の要素参照が dangling せず範囲外アクセスも起きないことを検証する
// (走査を終えてからまとめて発火する「配列操作を済ませてから発火」規約の回帰テスト)。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/BuffSystem.h"
#include "gameframework/CinematicsDirector.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

// ---- CBuffSystem: tick callback が同 owner へ ApplyBuff して配列を再確保させる ----

struct FBuffReentryCtx {
    CBuffSystem* sys      = nullptr;
    FBuffOwnerId owner{};
    u32          tick_fires = 0;
    u32          applied    = 0;
};

void OnBuffTickReapply(void* user, FBuffOwnerId owner, const char* /*buff_id*/,
                       u32 /*stack*/, f32 /*magnitude*/) noexcept
{
    auto* ctx = static_cast<FBuffReentryCtx*>(user);
    ++ctx->tick_fires;
    // tick 中に «別の» buff を大量適用して s.buffs を確実に再確保させる。
    // 旧実装ではこの再確保で Tick 側が保持する FBuffInstance& が dangling した。
    for (int i = 0; i < 8; ++i) {
        char idbuf[8] = { 'f','i','l','l','0','0',0,0 };
        idbuf[5] = static_cast<char>('0' + i);
        if (ctx->sys->ApplyBuff(owner, idbuf)) ++ctx->applied;
    }
}

} // namespace

ACS_TEST(DirectorReentrancy, BuffTickCallbackMayApplyBuffsSafely)
{
    CBuffSystem sys;
    // tick する毒 buff。
    FBuffDef poison{};
    poison.id                = "poison";
    poison.duration_sec      = 10.0f;
    poison.tick_interval_sec = 0.5f;
    poison.magnitude         = 3.0f;
    poison.stack_policy      = EBuffStackPolicy::Refresh;
    poison.max_stack         = 1;
    sys.RegisterBuff(poison);
    // callback が適用する fill buff 群 (tick しない常駐型)。
    for (int i = 0; i < 8; ++i) {
        char idbuf[8] = { 'f','i','l','l','0','0',0,0 };
        idbuf[5] = static_cast<char>('0' + i);
        FBuffDef fill{};
        fill.id           = nullptr;   // 後で id を差す (下)
        // 文字列リテラルが必要なので static テーブルから渡す。
        (void)fill;
    }
    // fill buff は id リテラルが要るので個別登録する。
    static const char* kFillIds[8] = {
        "fill00","fill01","fill02","fill03","fill04","fill05","fill06","fill07"
    };
    for (int i = 0; i < 8; ++i) {
        FBuffDef fill{};
        fill.id           = kFillIds[i];
        fill.duration_sec = 5.0f;
        fill.stack_policy = EBuffStackPolicy::Ignore;
        fill.max_stack    = 1;
        sys.RegisterBuff(fill);
    }

    const FBuffOwnerId owner = sys.CreateOwner();
    EXPECT_TRUE(owner.IsValid());

    FBuffReentryCtx ctx{};
    ctx.sys   = &sys;
    ctx.owner = owner;
    sys.SetOnTickCallback(&OnBuffTickReapply, &ctx);

    EXPECT_TRUE(sys.ApplyBuff(owner, "poison"));

    // dt=0.6 で poison が 1 回 tick → callback が 8 個 fill を適用 (再確保が起きる)。
    // 旧実装なら dangling 参照へのアクセスで ASan/クラッシュ。新実装は安全。
    sys.Tick(0.6f);
    EXPECT_EQ(ctx.tick_fires, 1u);
    EXPECT_EQ(ctx.applied, 8u);

    // poison + fill 8 = 9 個掛かっている (poison はまだ期限内)。
    EXPECT_EQ(sys.BuffCountOnOwner(owner), 9u);
    EXPECT_TRUE(sys.HasBuff(owner, "poison"));
    EXPECT_TRUE(sys.HasBuff(owner, "fill07"));

    // 複数フレーム進めても破綻しない。
    for (int f = 0; f < 20; ++f) sys.Tick(0.6f);
    // poison は 10 秒で expire 済み。
    EXPECT_FALSE(sys.HasBuff(owner, "poison"));
}

namespace {

// ---- CCinematicsDirector: event callback が同 director を Clear する ----

struct FCineReentryCtx {
    CCinematicsDirector* dir  = nullptr;
    u32                  fires = 0;
    bool                 cleared_on_first = true;
};

void OnCineEventClear(void* user, u32 /*event_id*/) noexcept
{
    auto* ctx = static_cast<FCineReentryCtx*>(user);
    ++ctx->fires;
    // 最初のイベント発火中に Clear() して m_Keyframes を空にする。
    // 旧実装ではキャッシュ済みの n を使うループが空配列を範囲外アクセスした。
    if (ctx->fires == 1u && ctx->cleared_on_first) {
        ctx->dir->Clear();
    }
}

FTimelineKeyframe MakeEventKf(f32 t, u32 id) noexcept
{
    FTimelineKeyframe kf{};
    kf.time_sec         = t;
    kf.kind             = ETimelineTrackKind::FireEvent;
    kf.payload.event.event_id = id;
    return kf;
}

} // namespace

ACS_TEST(DirectorReentrancy, CinematicsEventCallbackMayClearSafely)
{
    CCinematicsDirector dir;
    // 同一フレームで一括発火される 3 つのイベント keyframe。
    dir.AddKeyframe(MakeEventKf(0.1f, 100u));
    dir.AddKeyframe(MakeEventKf(0.1f, 200u));
    dir.AddKeyframe(MakeEventKf(0.1f, 300u));

    FCineReentryCtx ctx{};
    ctx.dir = &dir;
    dir.SetEventCallback(&OnCineEventClear, &ctx);

    dir.Play();
    // dt=1.0 で 3 つとも発火予定。1 つ目の callback が Clear() を呼ぶ。
    // 旧実装は空配列への範囲外アクセス (Release で UB) を起こした。
    dir.Tick(1.0f);

    // 走査完了後にまとめて発火するので、退避済みの 3 つは全て発火する
    // (Clear は «次回以降» の走査に効く。今フレームの退避コピーは安全に消化)。
    EXPECT_EQ(ctx.fires, 3u);
    // Clear されたので keyframe は空、終了状態。
    EXPECT_TRUE(dir.IsFinished());
    EXPECT_EQ(dir.TotalDuration(), 0.0f);

    // Clear 後も通常運用できる。
    ctx.cleared_on_first = false;
    ctx.fires = 0;
    dir.AddKeyframe(MakeEventKf(0.2f, 400u));
    dir.Stop();
    dir.Play();
    dir.Tick(1.0f);
    EXPECT_EQ(ctx.fires, 1u);
    EXPECT_TRUE(dir.IsFinished());
}

ACS_TEST(DirectorReentrancy, CinematicsFiresInTimeOrderAndSkipCompletes)
{
    CCinematicsDirector dir;
    dir.AddKeyframe(MakeEventKf(3.0f, 30u));
    dir.AddKeyframe(MakeEventKf(1.0f, 10u));   // 逆順で追加 → 内部で昇順ソート
    dir.AddKeyframe(MakeEventKf(2.0f, 20u));

    static u32 order[8];
    static u32 order_n;
    order_n = 0;
    struct FL { static void Fn(void* /*u*/, u32 id) noexcept { order[order_n++] = id; } };
    dir.SetEventCallback(&FL::Fn, nullptr);

    EXPECT_NEAR(dir.TotalDuration(), 3.0f, 1e-5f);

    dir.Play();
    dir.Tick(1.5f);                            // t=1.5 → 10 のみ
    EXPECT_EQ(order_n, 1u);
    EXPECT_EQ(order[0], 10u);

    dir.Skip();                                // 残り (20,30) を昇順で発火
    EXPECT_EQ(order_n, 3u);
    EXPECT_EQ(order[1], 20u);
    EXPECT_EQ(order[2], 30u);
    EXPECT_TRUE(dir.IsFinished());
}

ACS_TEST(DirectorReentrancy, CinematicsRejectsInvalidKeyframesAndProgressMutation)
{
    CCinematicsDirector dir;
    // 初期 keyframe の登録結果と負値の 0 丸めを確認する。
    EXPECT_TRUE(dir.TryAddKeyframe(MakeEventKf(-1.0f, 1u)));
    EXPECT_EQ(dir.KeyframeCount(), 1u);

    // 非有限時刻を拒否した後の keyframe 数を保持する。
    const f32 nan_time = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(dir.TryAddKeyframe(MakeEventKf(nan_time, 2u)));
    dir.AddKeyframe(MakeEventKf(std::numeric_limits<f32>::infinity(), 3u));
    EXPECT_FALSE(dir.TryAddKeyframe(MakeEventKf(-std::numeric_limits<f32>::infinity(), 3u)));
    EXPECT_EQ(dir.KeyframeCount(), 1u);

    // 再生中の追加拒否と件数不変を確認する。
    dir.Play();
    // 再生中の拒否判定前に保持するkeyframe件数。
    const u32 while_playing_count = dir.KeyframeCount();
    EXPECT_FALSE(dir.TryAddKeyframe(MakeEventKf(2.0f, 4u)));
    EXPECT_EQ(dir.KeyframeCount(), while_playing_count);

    // 進行後に停止していても追加を拒否し、Stopで初期状態へ戻す。
    dir.Tick(1.0f);
    dir.Pause();
    // 進行後の拒否判定前に保持するkeyframe件数。
    const u32 after_progress_count = dir.KeyframeCount();
    EXPECT_FALSE(dir.TryAddKeyframe(MakeEventKf(2.0f, 5u)));
    EXPECT_EQ(dir.KeyframeCount(), after_progress_count);
    dir.Stop();
    EXPECT_TRUE(dir.TryAddKeyframe(MakeEventKf(2.0f, 6u)));
}

ACS_TEST(DirectorReentrancy, CinematicsRejectsNonFiniteAndOverflowTickAtomically)
{
    CCinematicsDirector dir;
    // Tick の通常発火回数を記録する callback 状態。
    u32 fires = 0u;
    struct FFireCounter {
        static void Fn(void* user, u32) noexcept { ++*static_cast<u32*>(user); }
    };
    EXPECT_TRUE(dir.TryAddKeyframe(MakeEventKf(0.5f, 10u)));
    dir.SetEventCallback(&FFireCounter::Fn, &fires);
    dir.Play();

    // 非有限 dt は時刻を変更しない。
    dir.Tick(std::numeric_limits<f32>::quiet_NaN());
    dir.Tick(std::numeric_limits<f32>::infinity());
    dir.Tick(-std::numeric_limits<f32>::infinity());
    EXPECT_EQ(dir.CurrentTime(), 0.0f);
    EXPECT_EQ(fires, 0u);
    EXPECT_TRUE(dir.IsPlaying());

    // 有限最大値を受け入れ、次の加算 overflow は状態を変更しない。
    const f32 max_time = std::numeric_limits<f32>::max();
    dir.Tick(max_time);
    EXPECT_EQ(dir.CurrentTime(), max_time);
    const f32 before_overflow = dir.CurrentTime();
    dir.Tick(max_time);
    EXPECT_EQ(dir.CurrentTime(), before_overflow);
    EXPECT_EQ(fires, 1u);

    // 通常 dt は同じ keyframe を一度だけ発火する。
    dir.Stop();
    dir.Clear();
    EXPECT_TRUE(dir.TryAddKeyframe(MakeEventKf(0.5f, 20u)));
    dir.SetEventCallback(&FFireCounter::Fn, &fires);
    fires = 0u;
    dir.Play();
    dir.Tick(1.0f);
    dir.Tick(1.0f);
    EXPECT_EQ(fires, 1u);
}

namespace {

// 同時刻 callback の登録順を保存する状態。
struct FCineOrderCtx {
    u32 ids[3] = {};
    u32 count = 0u;
};

// 同時刻 callback の ID を発火順に記録する。
void OnCineOrder(void* user, u32 event_id) noexcept
{
    auto* ctx = static_cast<FCineOrderCtx*>(user);
    if (ctx->count < 3u) ctx->ids[ctx->count] = event_id;
    ++ctx->count;
}

} // namespace

ACS_TEST(DirectorReentrancy, CinematicsSameTimeRegistrationOrderIsStable)
{
    CCinematicsDirector dir;
    // 同じ発火時刻へ登録する三つの event。
    dir.AddKeyframe(MakeEventKf(1.0f, 100u));
    dir.AddKeyframe(MakeEventKf(1.0f, 200u));
    dir.AddKeyframe(MakeEventKf(1.0f, 300u));

    // callback の発火順を検証する状態。
    FCineOrderCtx ctx{};
    dir.SetEventCallback(&OnCineOrder, &ctx);
    dir.Play();
    dir.Tick(1.0f);

    EXPECT_EQ(ctx.count, 3u);
    EXPECT_EQ(ctx.ids[0], 100u);
    EXPECT_EQ(ctx.ids[1], 200u);
    EXPECT_EQ(ctx.ids[2], 300u);
}
