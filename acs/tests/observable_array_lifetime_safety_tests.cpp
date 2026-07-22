// SPDX-License-Identifier: Apache-2.0
// TObservableArray — 通知中に owner が破棄される場合の寿命安全性
#include "test/Test.h"
#include "test/Expect.h"
#include "mvvm/ObservableArray.h"

#include <new>

using namespace acs;

namespace acs::mvvm_test {

/**
 * 公開 mutation API の通知中変更禁止契約を破らず、private Notify frame chain を
 * 直接検証するための test seam 実装。
 */
struct FObservableArrayLifetimeTestAccess {
    template<typename T>
    static void InvalidateTwoFrames(TObservableArray<T>& array,
                                    bool& outer_alive,
                                    bool& inner_alive) noexcept {
        typename TObservableArray<T>::FNotifyFrame outer(array);
        typename TObservableArray<T>::FNotifyFrame inner(array);

        array.InvalidateNotifyFrames();
        outer_alive = outer.IsOwnerAlive();
        inner_alive = inner.IsOwnerAlive();
    }
};

} // namespace acs::mvvm_test

namespace {

using FIntObservableArray = TObservableArray<i32>;

/** 単一 ObservableArray と破棄回数をまとめる placement-new 用 owner。 */
struct FArrayOwner {
    explicit FArrayOwner(i32* destructor_calls) noexcept
        : DestructorCalls(destructor_calls) {}

    ~FArrayOwner() noexcept {
        if (DestructorCalls) ++(*DestructorCalls);
    }

    FIntObservableArray Values;
    i32* DestructorCalls = nullptr;
};

struct FDestroyOwnerContext {
    FArrayOwner* Owner = nullptr;
    i32 DestroyingListenerCalls = 0;
    i32 LaterListenerCalls = 0;
};

void DestroyArrayOwner(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FDestroyOwnerContext*>(user);
    ++context->DestroyingListenerCalls;

    FArrayOwner* const owner = context->Owner;
    context->Owner = nullptr;
    owner->~FArrayOwner();

    // owner と通知値はここで無効になり得る。破棄後は何も参照せず直ちに戻る。
}

void CountLaterOwnerListener(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FDestroyOwnerContext*>(user);
    ++context->LaterListenerCalls;
}

struct FPlacementReuseContext {
    void* Storage = nullptr;
    FIntObservableArray* Current = nullptr;
    i32 ReplacingListenerCalls = 0;
    i32 OldLaterListenerCalls = 0;
    i32 ReplacementListenerCalls = 0;
};

void CountReplacementListener(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FPlacementReuseContext*>(user);
    ++context->ReplacementListenerCalls;
}

void ReplaceArrayAtSameAddress(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FPlacementReuseContext*>(user);
    ++context->ReplacingListenerCalls;

    FIntObservableArray* const old_array = context->Current;
    old_array->~FIntObservableArray();

    auto* const replacement =
        ::new (context->Storage) FIntObservableArray();
    context->Current = replacement;
    replacement->Subscribe(&CountReplacementListener, context);

    // old_array と通知値はここで無効。旧 Notify へ処理を戻すだけにする。
}

void CountOldLaterListener(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FPlacementReuseContext*>(user);
    ++context->OldLaterListenerCalls;
}

/** 2 配列の入れ子通知中に owner 全体を破棄するための placement-new owner。 */
struct FNestedArrayOwner {
    explicit FNestedArrayOwner(i32* destructor_calls) noexcept
        : DestructorCalls(destructor_calls) {}

    ~FNestedArrayOwner() noexcept {
        if (DestructorCalls) ++(*DestructorCalls);
    }

    FIntObservableArray Outer;
    FIntObservableArray Inner;
    i32* DestructorCalls = nullptr;
};

struct FNestedDestroyContext {
    FNestedArrayOwner* Owner = nullptr;
    i32 OuterListenerCalls = 0;
    i32 InnerDestroyingListenerCalls = 0;
    i32 OuterLaterListenerCalls = 0;
    i32 InnerLaterListenerCalls = 0;
};

void DestroyNestedOwner(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FNestedDestroyContext*>(user);
    ++context->InnerDestroyingListenerCalls;

    FNestedArrayOwner* const owner = context->Owner;
    context->Owner = nullptr;
    owner->~FNestedArrayOwner();

    // 内外両方の Notify frame が dead になったため、owner へ戻らず直ちに返す。
}

void CountInnerLaterListener(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FNestedDestroyContext*>(user);
    ++context->InnerLaterListenerCalls;
}

void NotifyInnerArray(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FNestedDestroyContext*>(user);
    ++context->OuterListenerCalls;

    FNestedArrayOwner* const owner = context->Owner;
    owner->Inner.PushBack(22);

    // Inner の callback が owner を破棄し得るため、以後 owner を参照しない。
}

void CountOuterLaterListener(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FNestedDestroyContext*>(user);
    ++context->OuterLaterListenerCalls;
}

struct FUnsubscribeContext {
    FIntObservableArray* Array = nullptr;
    FArrayObserverHandle Target;
    FArrayObserverHandle Replacement;
    i32 RemovingListenerCalls = 0;
    i32 SuccessfulRemovals = 0;
    i32 RemovedListenerCalls = 0;
    i32 ReplacementListenerCalls = 0;
};

void CountReusedSlotListener(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FUnsubscribeContext*>(user);
    ++context->ReplacementListenerCalls;
}

void RemoveLaterSubscription(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FUnsubscribeContext*>(user);
    ++context->RemovingListenerCalls;
    if (!context->Replacement.IsValid() &&
        context->Array->Unsubscribe(context->Target)) {
        ++context->SuccessfulRemovals;
        context->Replacement =
            context->Array->Subscribe(&CountReusedSlotListener, context);
    }
}

void CountRemovedListener(EArrayChange, usize, const i32*, void* user) noexcept {
    auto* const context = static_cast<FUnsubscribeContext*>(user);
    ++context->RemovedListenerCalls;
}

} // namespace

ACS_TEST(Mvvm, ObservableArrayNotifyStopsWhenOwnerIsDestroyed) {
    alignas(FArrayOwner) u8 storage[sizeof(FArrayOwner)]{};
    i32 destructor_calls = 0;
    auto* const owner =
        ::new (static_cast<void*>(storage)) FArrayOwner(&destructor_calls);

    FDestroyOwnerContext context;
    context.Owner = owner;
    owner->Values.Subscribe(&DestroyArrayOwner, &context);
    owner->Values.Subscribe(&CountLaterOwnerListener, &context);

    owner->Values.PushBack(7);

    EXPECT_TRUE(context.Owner == nullptr);
    EXPECT_EQ(context.DestroyingListenerCalls, 1);
    EXPECT_EQ(context.LaterListenerCalls, 0);
    EXPECT_EQ(destructor_calls, 1);
}

ACS_TEST(Mvvm, ObservableArrayOldNotifyDoesNotEnterReplacementAtSameAddress) {
    alignas(FIntObservableArray) u8 storage[sizeof(FIntObservableArray)]{};
    auto* const first =
        ::new (static_cast<void*>(storage)) FIntObservableArray();

    FPlacementReuseContext context;
    context.Storage = storage;
    context.Current = first;
    first->Subscribe(&ReplaceArrayAtSameAddress, &context);
    first->Subscribe(&CountOldLaterListener, &context);

    first->PushBack(11);

    EXPECT_TRUE(context.Current == reinterpret_cast<FIntObservableArray*>(storage));
    EXPECT_EQ(context.ReplacingListenerCalls, 1);
    EXPECT_EQ(context.OldLaterListenerCalls, 0);
    EXPECT_EQ(context.ReplacementListenerCalls, 0);
    EXPECT_EQ(context.Current->Size(), usize(0));

    context.Current->PushBack(33);
    EXPECT_EQ(context.ReplacementListenerCalls, 1);
    EXPECT_EQ(context.Current->Size(), usize(1));
    context.Current->~FIntObservableArray();
}

ACS_TEST(Mvvm, ObservableArrayNestedNotificationOwnerDestructionStopsBothFrames) {
    alignas(FNestedArrayOwner) u8 storage[sizeof(FNestedArrayOwner)]{};
    i32 destructor_calls = 0;
    auto* const owner =
        ::new (static_cast<void*>(storage)) FNestedArrayOwner(&destructor_calls);

    FNestedDestroyContext context;
    context.Owner = owner;
    owner->Inner.Subscribe(&DestroyNestedOwner, &context);
    owner->Inner.Subscribe(&CountInnerLaterListener, &context);
    owner->Outer.Subscribe(&NotifyInnerArray, &context);
    owner->Outer.Subscribe(&CountOuterLaterListener, &context);

    owner->Outer.PushBack(11);

    EXPECT_TRUE(context.Owner == nullptr);
    EXPECT_EQ(context.OuterListenerCalls, 1);
    EXPECT_EQ(context.InnerDestroyingListenerCalls, 1);
    EXPECT_EQ(context.InnerLaterListenerCalls, 0);
    EXPECT_EQ(context.OuterLaterListenerCalls, 0);
    EXPECT_EQ(destructor_calls, 1);
}

ACS_TEST(Mvvm, ObservableArrayInvalidatesWholeSameArrayNotifyFrameChain) {
    FIntObservableArray array;
    bool outer_alive = true;
    bool inner_alive = true;

    mvvm_test::FObservableArrayLifetimeTestAccess::InvalidateTwoFrames(
        array, outer_alive, inner_alive);

    EXPECT_FALSE(outer_alive);
    EXPECT_FALSE(inner_alive);
    EXPECT_EQ(array.Size(), usize(0));
    EXPECT_EQ(array.SubscriberCount(), u32(0));
}

ACS_TEST(Mvvm, ObservableArrayUnsubscribeDuringNotifyStillDefersSlotReuse) {
    FIntObservableArray array;
    FUnsubscribeContext context;
    context.Array = &array;

    array.Subscribe(&RemoveLaterSubscription, &context);
    context.Target = array.Subscribe(&CountRemovedListener, &context);
    array.PushBack(1);

    EXPECT_EQ(context.RemovingListenerCalls, 1);
    EXPECT_EQ(context.SuccessfulRemovals, 1);
    EXPECT_EQ(context.RemovedListenerCalls, 0);
    EXPECT_TRUE(context.Replacement.IsValid());
    EXPECT_FALSE(context.Replacement == context.Target);
    // 通知中に解除した slot はまだ free ではない。即再購読した callback は今回呼ばれない。
    EXPECT_EQ(context.ReplacementListenerCalls, 0);
    EXPECT_FALSE(array.Unsubscribe(context.Target));

    array.PushBack(2);
    EXPECT_EQ(context.RemovingListenerCalls, 2);
    EXPECT_EQ(context.SuccessfulRemovals, 1);
    EXPECT_EQ(context.RemovedListenerCalls, 0);
    EXPECT_EQ(context.ReplacementListenerCalls, 1);
}
