// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Event — FTimerManager / MessageBroker テスト
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

struct ClearFromCallbackContext {
    FTimerManager* manager = nullptr;
    int clear_hits = 0;
    int later_hits = 0;
    FTimerHandle registration_during_clear{};
};

void OnLaterTimer(void* user)
{
    ++static_cast<ClearFromCallbackContext*>(user)->later_hits;
}

void OnClearFromCallback(void* user)
{
    auto& context = *static_cast<ClearFromCallbackContext*>(user);
    ++context.clear_hits;
    context.manager->Clear();
    context.registration_during_clear = context.manager->SetTimeout(0.0f, &OnLaterTimer, &context);
    // Clear 保留中の再入 Tick は何も発火せず、最外周 Tick が容量解放を担当する。
    context.manager->Tick(1.0f);
}
} // namespace

ACS_TEST(Event, TimerSetTimeoutFires) {
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
ACS_TEST(Event, TimerSetIntervalRepeats) {
    FTimerManager t;
    TimeoutCtx ctx;
    t.SetInterval(0.5f, &OnTimeout, &ctx);

    t.Tick(1.6f);            // 1.6 / 0.5 = 3 発火
    EXPECT_TRUE(ctx.hits >= 3);
}

// ---- FTimerManager: Cancel で発火を止められる -------------------------------
ACS_TEST(Event, TimerCancel) {
    FTimerManager t;
    TimeoutCtx ctx;
    auto h = t.SetTimeout(1.0f, &OnTimeout, &ctx);
    EXPECT_TRUE(t.Cancel(h));
    EXPECT_FALSE(t.Cancel(h));   // 二重 Cancel は false

    t.Tick(2.0f);
    EXPECT_EQ(ctx.hits, 0);
}

ACS_TEST(Event, TimerClearRemovesCallbacksAndAllowsReuse)
{
    FTimerManager timers;
    TimeoutCtx context;
    const FTimerHandle old_handle = timers.SetTimeout(0.0f, &OnTimeout, &context);
    EXPECT_TRUE(old_handle.IsValid());

    timers.Clear();
    timers.Clear();
    EXPECT_EQ(timers.ActiveCount(), 0u);
    EXPECT_TRUE(!timers.Cancel(old_handle));
    timers.Tick(1.0f);
    EXPECT_EQ(context.hits, 0);

    const FTimerHandle new_handle = timers.SetTimeout(0.0f, &OnTimeout, &context);
    EXPECT_TRUE(new_handle.IsValid());
    EXPECT_TRUE(!timers.Cancel(old_handle));
    timers.Tick(0.0f);
    EXPECT_EQ(context.hits, 1);
}

ACS_TEST(Event, TimerClearFromTimeoutCallbackDefersStorageRelease)
{
    FTimerManager timers;
    ClearFromCallbackContext context;
    context.manager = &timers;

    const FTimerHandle clearing_handle = timers.SetTimeout(0.0f, &OnClearFromCallback, &context);
    const FTimerHandle later_handle = timers.SetTimeout(0.0f, &OnLaterTimer, &context);
    EXPECT_TRUE(clearing_handle.IsValid());
    EXPECT_TRUE(later_handle.IsValid());

    timers.Tick(0.0f);
    EXPECT_EQ(context.clear_hits, 1);
    EXPECT_EQ(context.later_hits, 0);
    EXPECT_TRUE(!context.registration_during_clear.IsValid());
    EXPECT_EQ(timers.ActiveCount(), 0u);
    EXPECT_TRUE(!timers.Cancel(clearing_handle));
    EXPECT_TRUE(!timers.Cancel(later_handle));

    const FTimerHandle after_clear = timers.SetTimeout(0.0f, &OnLaterTimer, &context);
    EXPECT_TRUE(after_clear.IsValid());
    EXPECT_TRUE(!timers.Cancel(clearing_handle));
    timers.Tick(0.0f);
    EXPECT_EQ(context.later_hits, 1);
}

ACS_TEST(Event, TimerClearFromIntervalCallbackStopsCatchUp)
{
    FTimerManager timers;
    ClearFromCallbackContext context;
    context.manager = &timers;

    const FTimerHandle interval = timers.SetInterval(0.01f, &OnClearFromCallback, &context);
    EXPECT_TRUE(interval.IsValid());
    timers.Tick(1.0f);

    EXPECT_EQ(context.clear_hits, 1);
    EXPECT_EQ(timers.ActiveCount(), 0u);
    EXPECT_TRUE(!timers.Cancel(interval));
}

// ---- MessageBroker: Subscribe + Publish + Unsubscribe ----------------------
namespace {
struct DamageEvent { int amount; };
struct DamageCtx   { int total = 0; };

void OnDamage(const void* payload, void* user) {
    auto* e = static_cast<const DamageEvent*>(payload);
    static_cast<DamageCtx*>(user)->total += e->amount;
}
} // namespace

ACS_TEST(Event, BrokerSubscribePublish) {
    MessageBroker bus;
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

// ---- MessageBroker: 複数購読者 ---------------------------------------------
ACS_TEST(Event, BrokerMultiSubscribers) {
    MessageBroker bus;
    DamageCtx a, b, c;
    bus.Subscribe<DamageEvent>(&OnDamage, &a);
    bus.Subscribe<DamageEvent>(&OnDamage, &b);
    bus.Subscribe<DamageEvent>(&OnDamage, &c);

    bus.Publish<DamageEvent>(DamageEvent{7});
    EXPECT_EQ(a.total, 7);
    EXPECT_EQ(b.total, 7);
    EXPECT_EQ(c.total, 7);
}

ACS_TEST(Event, BrokerClearRemovesSubscribersAndAllowsReuse)
{
    MessageBroker broker;
    DamageCtx context;
    const SubscriptionHandle old_handle = broker.Subscribe<DamageEvent>(&OnDamage, &context);
    EXPECT_TRUE(old_handle.IsValid());

    broker.Clear();
    broker.Clear();
    EXPECT_EQ(broker.SubscriberCount(GetEventTypeId<DamageEvent>()), 0u);
    EXPECT_TRUE(!broker.Unsubscribe(old_handle));
    broker.Publish(DamageEvent{1});
    EXPECT_EQ(context.total, 0);

    const SubscriptionHandle new_handle = broker.Subscribe<DamageEvent>(&OnDamage, &context);
    EXPECT_TRUE(new_handle.IsValid());
    EXPECT_TRUE(!broker.Unsubscribe(old_handle));
    broker.Publish(DamageEvent{2});
    EXPECT_EQ(context.total, 2);
}
