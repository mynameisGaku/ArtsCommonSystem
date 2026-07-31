// SPDX-License-Identifier: Apache-2.0
// ACS TEvent — 再入・世代・所有購読契約テスト
#include "test/Test.h"
#include "test/Expect.h"
#include "event/TypedEvent.h"

using namespace acs;

namespace {

/** 複数のコールバックで共有する入力と観測結果をまとめる。 */
struct FEventContext {
    /** `TEvent<int>` の公開操作、所有権、復元値を検査する参照先。 */
    TEvent<int>* event = nullptr;
    /** `FTypedEventHandle` の公開操作、所有権、復元値を検査するインスタンス。 */
    FTypedEventHandle later{};
    /** `FTypedEventHandle` の公開操作、所有権、復元値を検査するインスタンス。 */
    FTypedEventHandle added{};
    /** `int` の公開操作、所有権、復元値を検査するインスタンス。 */
    int first_hits = 0;
    /** `int` の公開操作、所有権、復元値を検査するインスタンス。 */
    int later_hits = 0;
    /** `int` の公開操作、所有権、復元値を検査するインスタンス。 */
    int added_hits = 0;
};

/** 通知を受け取り、引数と呼び出し回数を観測状態へ記録する。
 * @param User コールバック間で観測状態を受け渡す不透明ポインター。
 */
void OnAdded(void* User, int) noexcept {
    ++static_cast<FEventContext*>(User)->added_hits;
}

/** 通知を受け取り、引数と呼び出し回数を観測状態へ記録する。
 * @param User コールバック間で観測状態を受け渡す不透明ポインター。
 */
void OnLater(void* User, int) noexcept {
    ++static_cast<FEventContext*>(User)->later_hits;
}

/** 通知を受け取り、引数と呼び出し回数を観測状態へ記録する。
 * @param User コールバック間で観測状態を受け渡す不透明ポインター。
 */
void OnMutating(void* User, int) noexcept {
    /** 不透明なコールバック引数から復元した共有検証状態。 */
    auto& Context = *static_cast<FEventContext*>(User);
    ++Context.first_hits;
    Context.event->Unsubscribe(Context.later);
    Context.added = Context.event->Subscribe(&OnAdded, &Context);
}

/** 件数の成功経路と拒否経路を実行し、戻り値または通知回数を検査する。
 * @param User 境界条件を再現する入力値。
 * @param Value 定数ノードへ保存する成分値。
 */
void OnCount(void* User, int Value) noexcept {
    *static_cast<int*>(User) += Value;
}

} // namespace

ACS_TEST(TypedEvent, CallbackMutationUsesPublishSnapshot) {
    /** `T` の公開操作、所有権、復元値を検査するインスタンス。 */
    TEvent<int> Event;
    /** 複数のコールバックで呼び出し回数と寿命を共有する状態。 */
    FEventContext Context;
    Context.event = &Event;
    /** `FTypedEventHandle` の登録順、重複拒否、世代差を比較する値。 */
    const FTypedEventHandle First =
        Event.SubscribeWithPriority(&OnMutating, 10, &Context);
    Context.later = Event.Subscribe(&OnLater, &Context);

    Event.Publish(1);
    EXPECT_TRUE(First.IsValid());
    EXPECT_EQ(Context.first_hits, 1);
    EXPECT_EQ(Context.later_hits, 0);
    EXPECT_EQ(Context.added_hits, 0);

    Event.Unsubscribe(First);
    Event.Publish(1);
    EXPECT_EQ(Context.added_hits, 1);
}

ACS_TEST(TypedEvent, ReusedSlotRejectsStaleGeneration) {
    /** `T` の公開操作、所有権、復元値を検査するインスタンス。 */
    TEvent<int> Event;
    /** `int` の公開操作、所有権、復元値を検査するインスタンス。 */
    int Total = 0;
    /** 型付きイベントの操作による差分を測るため退避した変更前の値。 */
    const FTypedEventHandle Old = Event.Subscribe(&OnCount, &Total);
    EXPECT_TRUE(Event.Unsubscribe(Old));
    /** 状態遷移の前後差を検査する現在。 */
    const FTypedEventHandle Current = Event.Subscribe(&OnCount, &Total);

    EXPECT_TRUE(Current.IsValid());
    EXPECT_TRUE(Current.slot_index == Old.slot_index);
    EXPECT_TRUE(Current.generation != Old.generation);
    EXPECT_FALSE(Event.Unsubscribe(Old));
    Event.Publish(3);
    EXPECT_EQ(Total, 3);
}

ACS_TEST(TypedEvent, OwnedSubscriptionUnsubscribesOnScopeExit) {
    /** `T` の公開操作、所有権、復元値を検査するインスタンス。 */
    TEvent<int> Event;
    /** `int` の公開操作、所有権、復元値を検査するインスタンス。 */
    int Total = 0;
    {
        /** 型付きイベントAPIの呼び出しから得た検証用オブジェクト。 */
        auto Subscription = Event.SubscribeOwned(&OnCount, &Total);
        EXPECT_TRUE(Subscription.IsValid());
        Event.Publish(2);
        EXPECT_EQ(Total, 2);
    }
    EXPECT_EQ(Event.SubscriptionCount(), 0u);
    Event.Publish(4);
    EXPECT_EQ(Total, 2);
}

ACS_TEST(TypedEvent, OwnedSubscriptionMayOutliveEvent) {
    /** `int` の公開操作、所有権、復元値を検査するインスタンス。 */
    int Total = 0;
    /** `TEvent` の公開操作、所有権、復元値を検査するインスタンス。 */
    TEventSubscription<int> Subscription;
    {
        /** `T` の公開操作、所有権、復元値を検査するインスタンス。 */
        TEvent<int> Event;
        Subscription = Event.SubscribeOwned(&OnCount, &Total);
        EXPECT_TRUE(Subscription.IsValid());
    }
    EXPECT_FALSE(Subscription.IsValid());
    EXPECT_FALSE(Subscription.Reset());
}
