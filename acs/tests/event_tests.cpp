// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Event — FTimerManager / MessageBroker テスト
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/TypeTraits.h"
#include "event/Timer.h"
#include "event/MessageBroker.h"

#include <limits>

using namespace acs;

/** 旧 CTimerManager が正規のタイマー管理器と同じ型を参照することを保証する。 */
static_assert(IsSameV<CTimerManager, FTimerManager>);
/** 旧 CMessageBroker が正規のメッセージ仲介器と同じ型を参照することを保証する。 */
static_assert(IsSameV<CMessageBroker, FMessageBroker>);

// ---- FTimerManager: SetTimeout が指定時間で発火 ------------------------------
namespace {
struct FTimeoutCtx { int hits = 0; };
void OnTimeout(void* user) {
    static_cast<FTimeoutCtx*>(user)->hits++;
}

struct FClearFromCallbackContext {
    FTimerManager* manager = nullptr;
    int clear_hits = 0;
    int later_hits = 0;
    FTimerHandle registration_during_clear{};
};

/** callback 中 cancel の active word 再読込を検査する context。 */
struct FCancelLaterTimerContext {
    /** cancel を実行する manager。 */
    FTimerManager* manager = nullptr;
    /** 同じ word の後方にある timer。 */
    FTimerHandle later{};
    /** cancel callback の発火数。 */
    int cancel_hits = 0;
    /** cancel 対象 callback の発火数。 */
    int later_hits = 0;
};

void OnLaterTimer(void* user)
{
    ++static_cast<FClearFromCallbackContext*>(user)->later_hits;
}

void OnClearFromCallback(void* user)
{
    auto& context = *static_cast<FClearFromCallbackContext*>(user);
    ++context.clear_hits;
    context.manager->Clear();
    context.registration_during_clear = context.manager->SetTimeout(0.0f, &OnLaterTimer, &context);
    // Clear 保留中の再入 Tick は何も発火せず、最外周 Tick が容量解放を担当する。
    context.manager->Tick(1.0f);
}

/** 同じ active word の後方 timer を発火前に cancel する。 */
void OnCancelLaterTimer(void* user)
{
    /** mutation 対象を保持する context。 */
    FCancelLaterTimerContext& context = *static_cast<FCancelLaterTimerContext*>(user);
    ++context.cancel_hits;
    (void)context.manager->Cancel(context.later);
}

/** cancel されなかった場合だけ後方発火を記録する。 */
void OnCancelledLaterTimer(void* user)
{
    /** 発火数を保持する context。 */
    FCancelLaterTimerContext& context = *static_cast<FCancelLaterTimerContext*>(user);
    ++context.later_hits;
}

/** 再入更新の呼出し回数を記録する。 */
struct FReentrantTickContext {
    /** 更新を呼び戻すタイマー管理器。 */
    FTimerManager* manager = nullptr;
    /** 周期処理が呼ばれた回数。 */
    int hits = 0;
    /** 再入更新を一度だけ試したかを示す。 */
    bool nested_tick_requested = false;
};

/**
 * 更新中に再び同じ管理器を更新しようとする。
 * @param user 再入更新の確認状態。
 */
void OnReentrantInterval(void* user)
{
    /** 再入更新の確認状態。 */
    auto& context = *static_cast<FReentrantTickContext*>(user);
    ++context.hits;
    if (context.nested_tick_requested) return;
    context.nested_tick_requested = true;
    context.manager->Tick(1.0f);
}

/** 更新中に登録したタイマーの発火時期を記録する。 */
struct FDeferredRegistrationContext {
    /** 新しいタイマーを登録する管理器。 */
    FTimerManager* manager = nullptr;
    /** 登録処理が呼ばれた回数。 */
    int registration_hits = 0;
    /** 新しいタイマーが呼ばれた回数。 */
    int deferred_hits = 0;
    /** 更新中に登録したタイマーのハンドル。 */
    FTimerHandle deferred_handle{};
};

/**
 * 更新中に登録されたタイマーの呼出し回数を増やす。
 * @param user 更新中登録の確認状態。
 */
void OnDeferredTimer(void* user)
{
    ++static_cast<FDeferredRegistrationContext*>(user)->deferred_hits;
}

/**
 * 周期処理から次回更新用のタイマーを一度だけ登録する。
 * @param user 更新中登録の確認状態。
 */
void OnRegisterDuringTick(void* user)
{
    /** 更新中登録の確認状態。 */
    auto& context = *static_cast<FDeferredRegistrationContext*>(user);
    ++context.registration_hits;
    if (context.deferred_handle.IsValid()) return;
    context.deferred_handle = context.manager->SetTimeout(0.0f, &OnDeferredTimer, &context);
}
} // namespace

ACS_TEST(Event, TimerHandleRequiresIdentifierAndGeneration)
{
    /** 番号と世代が未設定のハンドル。 */
    constexpr FTimerHandle empty{};
    /** 番号だけが設定されたハンドル。 */
    constexpr FTimerHandle identifier_only{1u, 0u};
    /** 番号と世代が設定されたハンドル。 */
    constexpr FTimerHandle complete{1u, 2u};
    /** 異なる世代を持つ比較用ハンドル。 */
    constexpr FTimerHandle other_generation{1u, 3u};

    static_assert(!empty.IsValid());
    static_assert(!identifier_only.IsValid());
    static_assert(complete.IsValid());
    static_assert(complete.Valid());
    static_assert(complete == FTimerHandle{1u, 2u});
    static_assert(complete != other_generation);

    EXPECT_FALSE(empty.IsValid());
    EXPECT_FALSE(identifier_only.IsValid());
    EXPECT_TRUE(complete.IsValid());
    EXPECT_TRUE(complete != other_generation);
}

ACS_TEST(Event, TimerSetTimeoutFires) {
    FTimerManager t;
    FTimeoutCtx ctx;
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
    FTimeoutCtx ctx;
    t.SetInterval(0.5f, &OnTimeout, &ctx);

    t.Tick(1.6f);            // 1.6 / 0.5 = 3 発火
    EXPECT_TRUE(ctx.hits >= 3);
}

// ---- FTimerManager: Cancel で発火を止められる -------------------------------
ACS_TEST(Event, TimerCancel) {
    FTimerManager t;
    FTimeoutCtx ctx;
    auto h = t.SetTimeout(1.0f, &OnTimeout, &ctx);
    EXPECT_TRUE(t.Cancel(h));
    EXPECT_FALSE(t.Cancel(h));   // 二重 Cancel は false

    t.Tick(2.0f);
    EXPECT_EQ(ctx.hits, 0);
}

ACS_TEST(Event, TimerClearRemovesCallbacksAndAllowsReuse)
{
    FTimerManager timers;
    FTimeoutCtx context;
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
    FClearFromCallbackContext context;
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
    FClearFromCallbackContext context;
    context.manager = &timers;

    const FTimerHandle interval = timers.SetInterval(0.01f, &OnClearFromCallback, &context);
    EXPECT_TRUE(interval.IsValid());
    timers.Tick(1.0f);

    EXPECT_EQ(context.clear_hits, 1);
    EXPECT_EQ(timers.ActiveCount(), 0u);
    EXPECT_TRUE(!timers.Cancel(interval));
}

ACS_TEST(Event, TimerReloadsActiveWordAfterCallbackMutation)
{
    /** callback mutation を実行する timer manager。 */
    FTimerManager timers;
    /** 同じ word 内 cancel の結果を保持する context。 */
    FCancelLaterTimerContext context;
    context.manager = &timers;
    /** 先に走査される cancel callback timer。 */
    const FTimerHandle cancelling = timers.SetTimeout(0.0f, &OnCancelLaterTimer, &context);
    context.later = timers.SetTimeout(0.0f, &OnCancelledLaterTimer, &context);
    EXPECT_TRUE(cancelling.IsValid());
    EXPECT_TRUE(context.later.IsValid());

    timers.Tick(0.0f);
    EXPECT_EQ(context.cancel_hits, 1);
    EXPECT_EQ(context.later_hits, 0);
    EXPECT_EQ(timers.ActiveCount(), 0u);
}

ACS_TEST(Event, TimerRejectsNonFiniteDurationsAndDeltas)
{
    /** 異常値を受け取るタイマー管理器。 */
    FTimerManager timers;
    /** 呼出し回数を記録する対象。 */
    FTimeoutCtx context;
    /** 数ではない浮動小数点値。 */
    const f32 not_a_number = std::numeric_limits<f32>::quiet_NaN();
    /** 正の無限大。 */
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();

    EXPECT_FALSE(timers.SetTimeout(not_a_number, &OnTimeout, &context).IsValid());
    EXPECT_FALSE(timers.SetTimeout(positive_infinity, &OnTimeout, &context).IsValid());
    EXPECT_FALSE(timers.SetInterval(not_a_number, &OnTimeout, &context).IsValid());
    EXPECT_FALSE(timers.SetInterval(positive_infinity, &OnTimeout, &context).IsValid());
    EXPECT_FALSE(timers.SetInterval(-1.0f, &OnTimeout, &context).IsValid());

    /** 正常な時間で登録した確認用タイマー。 */
    const FTimerHandle handle = timers.SetTimeout(0.5f, &OnTimeout, &context);
    EXPECT_TRUE(handle.IsValid());
    timers.Tick(not_a_number);
    timers.Tick(positive_infinity);
    timers.Tick(-1.0f);
    EXPECT_EQ(context.hits, 0);
    EXPECT_EQ(timers.ActiveCount(), 1u);
    timers.Tick(0.5f);
    EXPECT_EQ(context.hits, 1);
    EXPECT_EQ(timers.ActiveCount(), 0u);
}

ACS_TEST(Event, TimerIgnoresReentrantTick)
{
    /** 再入更新を試すタイマー管理器。 */
    FTimerManager timers;
    /** 再入更新の確認状態。 */
    FReentrantTickContext context;
    context.manager = &timers;

    /** 再入更新を起こす周期タイマー。 */
    const FTimerHandle interval = timers.SetInterval(1.0f, &OnReentrantInterval, &context);
    EXPECT_TRUE(interval.IsValid());
    timers.Tick(1.0f);
    EXPECT_EQ(context.hits, 1);
    EXPECT_TRUE(timers.IsActive(interval));

    timers.Tick(1.0f);
    EXPECT_EQ(context.hits, 2);
}

ACS_TEST(Event, TimerDefersRegistrationIntoReusableSlot)
{
    /** 更新中登録を確認するタイマー管理器。 */
    FTimerManager timers;
    /** 更新中登録の確認状態。 */
    FDeferredRegistrationContext context;
    context.manager = &timers;

    /** 更新中に新しいタイマーを登録する周期タイマー。 */
    const FTimerHandle interval = timers.SetInterval(1.0f, &OnRegisterDuringTick, &context);
    /** 再利用枠を用意するための予約タイマー。 */
    const FTimerHandle reusable = timers.SetTimeout(10.0f, &OnDeferredTimer, &context);
    EXPECT_TRUE(interval.IsValid());
    EXPECT_TRUE(reusable.IsValid());
    EXPECT_TRUE(timers.Cancel(reusable));

    timers.Tick(1.0f);
    EXPECT_EQ(context.registration_hits, 1);
    EXPECT_EQ(context.deferred_hits, 0);
    EXPECT_TRUE(timers.IsActive(context.deferred_handle));

    timers.Tick(0.0f);
    EXPECT_EQ(context.deferred_hits, 1);
    EXPECT_FALSE(timers.IsActive(context.deferred_handle));
}

ACS_TEST(Event, TimerCancelAllInvalidatesEveryHandle)
{
    /** 一括解除を確認するタイマー管理器。 */
    FTimerManager timers;
    /** 発火回数を記録する対象。 */
    FTimeoutCtx context;
    /** 一度だけ呼ぶタイマー。 */
    const FTimerHandle timeout = timers.SetTimeout(1.0f, &OnTimeout, &context);
    /** 繰り返し呼ぶタイマー。 */
    const FTimerHandle interval = timers.SetInterval(1.0f, &OnTimeout, &context);
    EXPECT_TRUE(timers.IsActive(timeout));
    EXPECT_TRUE(timers.IsActive(interval));

    timers.CancelAll();
    EXPECT_FALSE(timers.IsActive(timeout));
    EXPECT_FALSE(timers.IsActive(interval));
    EXPECT_EQ(timers.ActiveCount(), 0u);
    timers.Tick(2.0f);
    EXPECT_EQ(context.hits, 0);

    /** 一括解除後に再利用できる確認用タイマー。 */
    const FTimerHandle reused = timers.SetTimeout(0.0f, &OnTimeout, &context);
    EXPECT_TRUE(reused.IsValid());
    EXPECT_FALSE(reused == timeout);
    timers.Tick(0.0f);
    EXPECT_EQ(context.hits, 1);
}

// ---- MessageBroker: Subscribe + Publish + Unsubscribe ----------------------
namespace {
struct FDamageEvent { int amount; };
struct FDamageCtx   { int total = 0; };

void OnDamage(const void* payload, void* user) {
    auto* e = static_cast<const FDamageEvent*>(payload);
    static_cast<FDamageCtx*>(user)->total += e->amount;
}
} // namespace

ACS_TEST(Event, SubscriptionHandleRequiresChannelIdentifierAndGeneration)
{
    /** すべての値が未設定の購読ハンドル。 */
    constexpr FSubscriptionHandle empty{};
    /** 通路だけが範囲外の購読ハンドル。 */
    constexpr FSubscriptionHandle invalid_channel{kMaxEventTypes, 1u, 1u};
    /** 購読番号が未設定の購読ハンドル。 */
    constexpr FSubscriptionHandle missing_identifier{0u, 0u, 1u};
    /** 世代番号が未設定の購読ハンドル。 */
    constexpr FSubscriptionHandle missing_generation{0u, 1u, 0u};
    /** すべての値が設定された購読ハンドル。 */
    constexpr FSubscriptionHandle complete{0u, 1u, 2u};
    /** 異なる世代を持つ比較用購読ハンドル。 */
    constexpr FSubscriptionHandle other_generation{0u, 1u, 3u};

    static_assert(!empty.IsValid());
    static_assert(!invalid_channel.IsValid());
    static_assert(!missing_identifier.IsValid());
    static_assert(!missing_generation.IsValid());
    static_assert(complete.IsValid());
    static_assert(complete == FSubscriptionHandle{0u, 1u, 2u});
    static_assert(!(complete == other_generation));
    static_assert(IsValidEventTypeId(0u));
    static_assert(IsValidEventTypeId(kMaxEventTypes - 1u));
    static_assert(!IsValidEventTypeId(kMaxEventTypes));

    EXPECT_FALSE(empty.IsValid());
    EXPECT_FALSE(invalid_channel.IsValid());
    EXPECT_FALSE(missing_identifier.IsValid());
    EXPECT_FALSE(missing_generation.IsValid());
    EXPECT_TRUE(complete.IsValid());
    EXPECT_TRUE(IsValidEventTypeId(kMaxEventTypes - 1u));
    EXPECT_FALSE(IsValidEventTypeId(kMaxEventTypes));
}

ACS_TEST(Event, BrokerSubscribePublish) {
    FMessageBroker bus;
    FDamageCtx ctx;
    auto h = bus.Subscribe<FDamageEvent>(&OnDamage, &ctx);
    EXPECT_TRUE(h.IsValid());

    bus.Publish<FDamageEvent>(FDamageEvent{10});
    bus.Publish<FDamageEvent>(FDamageEvent{25});
    EXPECT_EQ(ctx.total, 35);

    EXPECT_TRUE(bus.Unsubscribe(h));
    bus.Publish<FDamageEvent>(FDamageEvent{99});
    EXPECT_EQ(ctx.total, 35);   // Unsubscribe 後は届かない
}

// ---- MessageBroker: 複数購読者 ---------------------------------------------
ACS_TEST(Event, BrokerMultiSubscribers) {
    FMessageBroker bus;
    FDamageCtx a, b, c;
    bus.Subscribe<FDamageEvent>(&OnDamage, &a);
    bus.Subscribe<FDamageEvent>(&OnDamage, &b);
    bus.Subscribe<FDamageEvent>(&OnDamage, &c);

    bus.Publish<FDamageEvent>(FDamageEvent{7});
    EXPECT_EQ(a.total, 7);
    EXPECT_EQ(b.total, 7);
    EXPECT_EQ(c.total, 7);
}

ACS_TEST(Event, BrokerClearRemovesSubscribersAndAllowsReuse)
{
    FMessageBroker broker;
    FDamageCtx context;
    const FSubscriptionHandle old_handle = broker.Subscribe<FDamageEvent>(&OnDamage, &context);
    EXPECT_TRUE(old_handle.IsValid());

    broker.Clear();
    broker.Clear();
    EXPECT_EQ(broker.SubscriberCount(GetEventTypeId<FDamageEvent>()), 0u);
    EXPECT_TRUE(!broker.Unsubscribe(old_handle));
    broker.Publish(FDamageEvent{1});
    EXPECT_EQ(context.total, 0);

    const FSubscriptionHandle new_handle = broker.Subscribe<FDamageEvent>(&OnDamage, &context);
    EXPECT_TRUE(new_handle.IsValid());
    EXPECT_TRUE(!broker.Unsubscribe(old_handle));
    broker.Publish(FDamageEvent{2});
    EXPECT_EQ(context.total, 2);
}
