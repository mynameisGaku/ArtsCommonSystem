// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS FEvent — FTimerManager / FMessageBroker テスト
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "event/Timer.h"
#include "event/MessageBroker.h"

using namespace acs;

// ---- FTimerManager: SetTimeout が指定時間で発火 ------------------------------
namespace {
struct TimeoutCtx { int hits = 0; };
void OnTimeout(void* user) {
    static_cast<TimeoutCtx*>(user)->hits++;
}
} // namespace

ACS_TEST(FEvent, TimerSetTimeoutFires) {
    FTimerManager t;
    TimeoutCtx ctx;
    auto h = t.SetTimeout(1.0f, &OnTimeout, &ctx);
    EXPECT_TRUE(h.IsValid());

    // 0.5 秒 → まだ発火せず
    t.Tick(0.5f);
    EXPECT_EQ(ctx.hits, 0);

    // さらに 0.6 秒 → 発火
    t.Tick(0.6f);
    EXPECT_EQ(ctx.hits, 1);

    // 1 回限りなので追加経過しても増えない
    t.Tick(2.0f);
    EXPECT_EQ(ctx.hits, 1);
    EXPECT_EQ(t.ActiveCount(), 0u);
}

// ---- FTimerManager: SetInterval が周期で何回も発火 ---------------------------
ACS_TEST(FEvent, TimerSetIntervalRepeats) {
    FTimerManager t;
    TimeoutCtx ctx;
    t.SetInterval(0.5f, &OnTimeout, &ctx);

    t.Tick(1.6f);            // 1.6 / 0.5 = 3 発火
    EXPECT_TRUE(ctx.hits >= 3);
}

// ---- FTimerManager: Cancel で発火を止められる -------------------------------
ACS_TEST(FEvent, TimerCancel) {
    FTimerManager t;
    TimeoutCtx ctx;
    auto h = t.SetTimeout(1.0f, &OnTimeout, &ctx);
    EXPECT_TRUE(t.Cancel(h));
    EXPECT_FALSE(t.Cancel(h));   // 二重 Cancel は false

    t.Tick(2.0f);
    EXPECT_EQ(ctx.hits, 0);
}

// ---- FMessageBroker: Subscribe + Publish + Unsubscribe ----------------------
namespace {
struct DamageEvent { int amount; };
struct DamageCtx   { int total = 0; };

void OnDamage(const void* payload, void* user) {
    auto* e = static_cast<const DamageEvent*>(payload);
    static_cast<DamageCtx*>(user)->total += e->amount;
}
} // namespace

ACS_TEST(FEvent, BrokerSubscribePublish) {
    FMessageBroker bus;
    DamageCtx ctx;
    auto h = bus.Subscribe<DamageEvent>(&OnDamage, &ctx);
    EXPECT_TRUE(h.IsValid());

    bus.Publish<DamageEvent>(DamageEvent{10});
    bus.Publish<DamageEvent>(DamageEvent{25});
    EXPECT_EQ(ctx.total, 35);

    EXPECT_TRUE(bus.Unsubscribe(h));
    bus.Publish<DamageEvent>(DamageEvent{99});
    EXPECT_EQ(ctx.total, 35);   // Unsubscribe 後は届かない
}

// ---- FMessageBroker: 複数購読者 ---------------------------------------------
ACS_TEST(FEvent, BrokerMultiSubscribers) {
    FMessageBroker bus;
    DamageCtx a, b, c;
    bus.Subscribe<DamageEvent>(&OnDamage, &a);
    bus.Subscribe<DamageEvent>(&OnDamage, &b);
    bus.Subscribe<DamageEvent>(&OnDamage, &c);

    bus.Publish<DamageEvent>(DamageEvent{7});
    EXPECT_EQ(a.total, 7);
    EXPECT_EQ(b.total, 7);
    EXPECT_EQ(c.total, 7);
}
