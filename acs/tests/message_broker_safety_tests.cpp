// SPDX-License-Identifier: Apache-2.0
#include "event/MessageBroker.h"
#include "test/Expect.h"
#include "test/Test.h"

using namespace acs;

/** 既存の64-bit Windows利用側と共有する仲介器の物理サイズを固定する。 */
static_assert(sizeof(void*) != 8 || sizeof(CMessageBroker) == 40);

namespace {

/** 配信順序と配信中の購読変更を確認するメッセージ。 */
struct FBrokerSafetyMessage {
    /** 各受信先へ渡す確認値。 */
    int value = 0;
};

/** 配信中の購読追加を確認する状態。 */
struct FSubscribeDuringPublishContext {
    /** 確認対象のメッセージ仲介器。 */
    CMessageBroker* broker = nullptr;
    /** 配信中に追加した購読のハンドル。 */
    FSubscriptionHandle added{};
    /** 購読を追加する処理の呼出し回数。 */
    int registrar_hits = 0;
    /** 配信中に追加した処理の呼出し回数。 */
    int added_hits = 0;
    /** 既存の後続処理の呼出し回数。 */
    int later_hits = 0;
};

/**
 * 配信中に追加された購読の呼出し回数を記録する。
 * @param payload 配信された確認メッセージ。
 * @param user 確認状態へのポインター。
 */
void OnAddedSubscription(const void* payload, void* user) {
    /** 配信された確認メッセージ。 */
    const auto& message = *static_cast<const FBrokerSafetyMessage*>(payload);
    /** 呼出し回数を記録する確認状態。 */
    auto& context = *static_cast<FSubscribeDuringPublishContext*>(user);
    context.added_hits += message.value;
}

/**
 * 配信開始後に新しい購読を一度だけ追加する。
 * @param payload 配信された確認メッセージ。
 * @param user 確認状態へのポインター。
 */
void RegisterFromCallback(const void* payload, void* user) {
    /** 配信された確認メッセージ。 */
    const auto& message = *static_cast<const FBrokerSafetyMessage*>(payload);
    /** 購読追加を行う確認状態。 */
    auto& context = *static_cast<FSubscribeDuringPublishContext*>(user);
    context.registrar_hits += message.value;
    if (!context.added.IsValid()) context.added = context.broker->Subscribe<FBrokerSafetyMessage>(&OnAddedSubscription, &context);
}

/**
 * 配信開始時から存在する後続購読の呼出し回数を記録する。
 * @param payload 配信された確認メッセージ。
 * @param user 確認状態へのポインター。
 */
void OnLaterSubscription(const void* payload, void* user) {
    /** 配信された確認メッセージ。 */
    const auto& message = *static_cast<const FBrokerSafetyMessage*>(payload);
    /** 呼出し回数を記録する確認状態。 */
    auto& context = *static_cast<FSubscribeDuringPublishContext*>(user);
    context.later_hits += message.value;
}

/**
 * 空き購読枠を作るためだけに登録する処理。
 * @param payload 使用しない配信値。
 * @param user 使用しない任意データ。
 */
void UnusedSubscription(const void* payload, void* user) {
    (void)payload;
    (void)user;
}

/** 配信中の全解除を確認する状態。 */
struct FClearDuringPublishContext {
    /** 確認対象のメッセージ仲介器。 */
    CMessageBroker* broker = nullptr;
    /** 全解除中に試した購読のハンドル。 */
    FSubscriptionHandle attempted_during_clear{};
    /** 全解除を行う処理の呼出し回数。 */
    int clear_hits = 0;
    /** 全解除より後ろにあった処理の呼出し回数。 */
    int later_hits = 0;
    /** 全解除完了後に追加した処理の呼出し回数。 */
    int after_clear_hits = 0;
};

/** 異なる通路へ入れ子配信して全解除する確認用メッセージ。 */
struct FNestedClearMessage {};

/** 異なる通路をまたぐ全解除の観測値。 */
struct FNestedClearContext {
    /** 確認対象のメッセージ仲介器。 */
    CMessageBroker* broker = nullptr;
    /** 入れ子配信を開始した回数。 */
    int outer_hits = 0;
    /** 入れ子配信から全解除した回数。 */
    int nested_clear_hits = 0;
    /** 全解除後に停止される外側処理の回数。 */
    int outer_later_hits = 0;
    /** 全解除中に試した購読のハンドル。 */
    FSubscriptionHandle attempted_during_clear{};
};

/**
 * 全解除より後ろにある購読の呼出し回数を記録する。
 * @param payload 使用しない配信値。
 * @param user 確認状態へのポインター。
 */
void OnLaterAfterClear(const void* payload, void* user) {
    (void)payload;
    ++static_cast<FClearDuringPublishContext*>(user)->later_hits;
}

/**
 * 全解除完了後に追加した購読の呼出し回数を記録する。
 * @param payload 使用しない配信値。
 * @param user 確認状態へのポインター。
 */
void OnAfterClear(const void* payload, void* user) {
    (void)payload;
    ++static_cast<FClearDuringPublishContext*>(user)->after_clear_hits;
}

/**
 * 配信中に全購読を解除し、解除完了前の再登録が拒否されることを記録する。
 * @param payload 使用しない配信値。
 * @param user 確認状態へのポインター。
 */
void ClearFromCallback(const void* payload, void* user) {
    (void)payload;
    /** 全解除を行う確認状態。 */
    auto& context = *static_cast<FClearDuringPublishContext*>(user);
    ++context.clear_hits;
    context.broker->Clear();
    context.attempted_during_clear = context.broker->Subscribe<FBrokerSafetyMessage>(&OnAfterClear, &context);
}

/**
 * 入れ子配信から全通路を解除する。
 * @param payload 使用しない配信値。
 * @param user 確認状態へのポインター。
 */
void ClearFromNestedChannel(const void* payload, void* user) {
    (void)payload;
    /** 全解除結果を記録する確認状態。 */
    auto& context = *static_cast<FNestedClearContext*>(user);
    ++context.nested_clear_hits;
    context.broker->Clear();
    context.attempted_during_clear = context.broker->Subscribe<FNestedClearMessage>(&ClearFromNestedChannel, &context);
}

/**
 * 異なる通路への入れ子配信を開始する。
 * @param payload 使用しない配信値。
 * @param user 確認状態へのポインター。
 */
void PublishNestedChannel(const void* payload, void* user) {
    (void)payload;
    /** 入れ子配信を行う確認状態。 */
    auto& context = *static_cast<FNestedClearContext*>(user);
    ++context.outer_hits;
    context.broker->Publish(FNestedClearMessage{});
}

/**
 * 全解除より後ろにある外側通路の処理回数を記録する。
 * @param payload 使用しない配信値。
 * @param user 確認状態へのポインター。
 */
void OnOuterLater(const void* payload, void* user) {
    (void)payload;
    ++static_cast<FNestedClearContext*>(user)->outer_later_hits;
}

} // namespace

/** 配信前から空いていた枠があっても、配信中の追加を次回まで呼ばないことを確認する。 */
ACS_TEST(MessageBrokerSafety, SubscribeDuringPublishUsesNextSnapshot) {
    /** 購読と配信を行う確認対象。 */
    CMessageBroker broker;
    /** 購読変更と呼出し回数をまとめる確認状態。 */
    FSubscribeDuringPublishContext context;
    context.broker = &broker;

    /** 配信中に購読を追加する先頭処理。 */
    const FSubscriptionHandle registrar = broker.Subscribe<FBrokerSafetyMessage>(&RegisterFromCallback, &context);
    /** 先頭処理より後ろに空き枠を作る購読。 */
    const FSubscriptionHandle reusable = broker.Subscribe<FBrokerSafetyMessage>(&UnusedSubscription, nullptr);
    /** 最初の配信でも呼ばれる既存の後続購読。 */
    const FSubscriptionHandle later = broker.Subscribe<FBrokerSafetyMessage>(&OnLaterSubscription, &context);
    EXPECT_TRUE(registrar.IsValid());
    EXPECT_TRUE(reusable.IsValid());
    EXPECT_TRUE(later.IsValid());
    EXPECT_TRUE(broker.Unsubscribe(reusable));

    broker.Publish(FBrokerSafetyMessage{1});
    EXPECT_TRUE(context.added.IsValid());
    EXPECT_EQ(context.registrar_hits, 1);
    EXPECT_EQ(context.later_hits, 1);
    EXPECT_EQ(context.added_hits, 0);

    broker.Publish(FBrokerSafetyMessage{1});
    EXPECT_EQ(context.registrar_hits, 2);
    EXPECT_EQ(context.later_hits, 2);
    EXPECT_EQ(context.added_hits, 1);
}

/** 配信中の全解除が残りの処理を止め、配信終了後に再利用できることを確認する。 */
ACS_TEST(MessageBrokerSafety, ClearDuringPublishDefersStorageRelease) {
    /** 全解除と再利用を行う確認対象。 */
    CMessageBroker broker;
    /** 全解除前後の呼出し回数をまとめる確認状態。 */
    FClearDuringPublishContext context;
    context.broker = &broker;

    /** 配信中に全解除を行う先頭購読。 */
    const FSubscriptionHandle clearing = broker.Subscribe<FBrokerSafetyMessage>(&ClearFromCallback, &context);
    /** 全解除によって呼出しを止める後続購読。 */
    const FSubscriptionHandle later = broker.Subscribe<FBrokerSafetyMessage>(&OnLaterAfterClear, &context);
    EXPECT_TRUE(clearing.IsValid());
    EXPECT_TRUE(later.IsValid());

    broker.Publish(FBrokerSafetyMessage{1});
    EXPECT_EQ(context.clear_hits, 1);
    EXPECT_EQ(context.later_hits, 0);
    EXPECT_FALSE(context.attempted_during_clear.IsValid());
    EXPECT_EQ(broker.SubscriberCount(GetEventTypeId<FBrokerSafetyMessage>()), 0u);
    EXPECT_FALSE(broker.Unsubscribe(clearing));
    EXPECT_FALSE(broker.Unsubscribe(later));

    /** 全解除の完了後に追加した購読。 */
    const FSubscriptionHandle after_clear = broker.Subscribe<FBrokerSafetyMessage>(&OnAfterClear, &context);
    EXPECT_TRUE(after_clear.IsValid());
    broker.Publish(FBrokerSafetyMessage{1});
    EXPECT_EQ(context.after_clear_hits, 1);
}

/** 異なる通路の入れ子配信中も、最外側へ戻るまで全通路の保持領域を解放しないことを確認する。 */
ACS_TEST(MessageBrokerSafety, NestedChannelClearWaitsForOutermostPublish) {
    /** 入れ子配信と全解除を行う確認対象。 */
    CMessageBroker broker;
    /** 通路をまたぐ呼出し結果をまとめる確認状態。 */
    FNestedClearContext context;
    context.broker = &broker;

    /** 異なる通路へ入れ子配信する外側購読。 */
    const FSubscriptionHandle outer = broker.Subscribe<FBrokerSafetyMessage>(&PublishNestedChannel, &context);
    /** 全解除後には呼ばれない外側の後続購読。 */
    const FSubscriptionHandle outer_later = broker.Subscribe<FBrokerSafetyMessage>(&OnOuterLater, &context);
    /** 入れ子配信から全解除する内側購読。 */
    const FSubscriptionHandle nested = broker.Subscribe<FNestedClearMessage>(&ClearFromNestedChannel, &context);
    EXPECT_TRUE(outer.IsValid());
    EXPECT_TRUE(outer_later.IsValid());
    EXPECT_TRUE(nested.IsValid());

    broker.Publish(FBrokerSafetyMessage{1});
    EXPECT_EQ(context.outer_hits, 1);
    EXPECT_EQ(context.nested_clear_hits, 1);
    EXPECT_EQ(context.outer_later_hits, 0);
    EXPECT_FALSE(context.attempted_during_clear.IsValid());
    EXPECT_EQ(broker.SubscriberCount(GetEventTypeId<FBrokerSafetyMessage>()), 0u);
    EXPECT_EQ(broker.SubscriberCount(GetEventTypeId<FNestedClearMessage>()), 0u);

    /** 最外側の配信終了後に再利用できることを確認する購読。 */
    const FSubscriptionHandle reused = broker.Subscribe<FBrokerSafetyMessage>(&OnOuterLater, &context);
    EXPECT_TRUE(reused.IsValid());
    broker.Publish(FBrokerSafetyMessage{1});
    EXPECT_EQ(context.outer_later_hits, 1);
}

/** 全解除前の再利用世代が、全解除後の新しい購読と一致しないことを確認する。 */
ACS_TEST(MessageBrokerSafety, ClearDoesNotRecycleStaleGeneration) {
    /** 世代更新と全解除を行う確認対象。 */
    CMessageBroker broker;

    /** 最初の購読枠を作るハンドル。 */
    const FSubscriptionHandle first = broker.Subscribe<FBrokerSafetyMessage>(&UnusedSubscription, nullptr);
    EXPECT_TRUE(first.IsValid());
    EXPECT_TRUE(broker.Unsubscribe(first));

    /** 同じ購読枠を一度再利用して世代を進めた古いハンドル。 */
    const FSubscriptionHandle stale = broker.Subscribe<FBrokerSafetyMessage>(&UnusedSubscription, nullptr);
    EXPECT_TRUE(stale.IsValid());
    broker.Clear();

    /** 全解除後に作り直した通路の新しい購読ハンドル。 */
    const FSubscriptionHandle current = broker.Subscribe<FBrokerSafetyMessage>(&UnusedSubscription, nullptr);
    EXPECT_TRUE(current.IsValid());
    EXPECT_FALSE(stale == current);
    EXPECT_FALSE(broker.Unsubscribe(stale));
    EXPECT_TRUE(broker.Unsubscribe(current));
}
