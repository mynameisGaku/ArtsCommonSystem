// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: FEventBus (オブジェクト間 pub/sub) の検証 (GPU 非依存)。
//   ・Subscribe → Publish でハンドラが listener+payload 付きで呼ばれる
//   ・別イベントは呼ばれない / Unsubscribe で止まる / 複数購読
//   ・Id() のハッシュが名前で決定的(衝突せず一致)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/EventBus.h"

using namespace acs;
using namespace acs::game;

namespace {
int s_calls = 0;
int s_sum   = 0;
// captureless ハンドラ: listener を +1、payload(int*)を s_sum へ加算、呼び出し回数を数える。
void TestHandler(void* listener, const void* payload) noexcept {
    ++s_calls;
    if (payload  != nullptr) s_sum += *static_cast<const int*>(payload);
    if (listener != nullptr) *static_cast<int*>(listener) += 1;
}
} // namespace

// 購読 → 発火でハンドラが listener / payload 付きで呼ばれる。別イベント/解除も正しい。
ACS_TEST(EventBus, SubscribePublishUnsubscribe) {
    s_calls = 0; s_sum = 0;
    FEventBus bus;
    int listenerState = 0;
    const auto HIT = FEventBus::Id("Hit");

    int h = bus.Subscribe(HIT, &TestHandler, &listenerState);
    EXPECT_TRUE(h >= 0);

    int payload = 5;
    bus.Publish(HIT, &payload);
    EXPECT_EQ(s_calls, 1);
    EXPECT_EQ(s_sum, 5);
    EXPECT_EQ(listenerState, 1);

    // 別イベントは呼ばれない。
    bus.Publish(FEventBus::Id("Other"), nullptr);
    EXPECT_EQ(s_calls, 1);

    // 解除すると呼ばれない。
    bus.Unsubscribe(h);
    bus.Publish(HIT, &payload);
    EXPECT_EQ(s_calls, 1);
}

// 同一イベントの複数購読は全て呼ばれる(空きスロット再利用も含む)。
ACS_TEST(EventBus, MultipleSubscribers) {
    s_calls = 0; s_sum = 0;
    FEventBus bus;
    const auto EV = FEventBus::Id("Tick");
    int a = bus.Subscribe(EV, &TestHandler, nullptr);
    bus.Subscribe(EV, &TestHandler, nullptr);
    bus.Publish(EV, nullptr);
    EXPECT_EQ(s_calls, 2);                 // 2 ハンドラ

    bus.Unsubscribe(a);                    // 1 つ解除 → 空きスロット
    s_calls = 0;
    int reuse = bus.Subscribe(EV, &TestHandler, nullptr);   // 空きを再利用
    EXPECT_EQ(reuse, a);                   // 解除した枠が再利用される
    bus.Publish(EV, nullptr);
    EXPECT_EQ(s_calls, 2);
}

// Id() は名前で決定的(同名一致・異名不一致)。
ACS_TEST(EventBus, EventIdIsDeterministic) {
    EXPECT_TRUE(FEventBus::Id("PlayerDied") == FEventBus::Id("PlayerDied"));
    EXPECT_TRUE(FEventBus::Id("PlayerDied") != FEventBus::Id("EnemyDied"));
    EXPECT_TRUE(FEventBus::Id("") == FEventBus::Id(""));
}
