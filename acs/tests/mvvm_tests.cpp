// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS MVVM — Observable / Derived / Binder / Convert / Command / ObservableArray
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "mvvm/Observable.h"
#include "mvvm/ObservableArray.h"
#include "mvvm/Binder.h"
#include "mvvm/Derived.h"
#include "mvvm/Command.h"
#include "mvvm/Convert.h"
#include "container/String.h"
#include "memory/UniquePtr.h"

#include <new>

using namespace acs;

// ---- Observable: Subscribe / Set / 通知 ------------------------------------
namespace {
struct FCounter { int hits = 0; f32 last = 0; };
void OnFloatChanged(const f32& v, void* user) {
    auto* c = static_cast<FCounter*>(user);
    c->hits++;
    c->last = v;
}
} // namespace

ACS_TEST(Mvvm, ObservableSetTriggersListeners) {
    TObservable<f32> hp{ 100.0f };
    FCounter c;
    auto h = hp.Subscribe(&OnFloatChanged, &c);
    EXPECT_TRUE(h.IsValid());

    hp.Set(80.0f);
    EXPECT_EQ(c.hits, 1);
    EXPECT_EQ(c.last, 80.0f);

    // 同値はスキップ
    hp.Set(80.0f);
    EXPECT_EQ(c.hits, 1);

    hp.Set(75.0f);
    EXPECT_EQ(c.hits, 2);

    // Unsubscribe → 通知無し
    EXPECT_TRUE(hp.Unsubscribe(h));
    hp.Set(50.0f);
    EXPECT_EQ(c.hits, 2);
}

// ---- Observable: 通知中に解除した slot は通知完了まで再利用しない ------------
ACS_TEST(Mvvm, ObservableUnsubscribeDuringNotifyDefersSlotReuse) {
    TObservable<i32> observable{ 0 };
    struct FObservableUnsubscribeContext {
        TObservable<i32>* Observable = nullptr;
        FObservableHandle Target;
        FObservableHandle Replacement;
        TObservable<i32>::Listener ReplacementListener = nullptr;
        i32 RemovingListenerCalls = 0;
        i32 SuccessfulRemovals = 0;
        i32 RemovedListenerCalls = 0;
        i32 ReplacementListenerCalls = 0;
    } context;

    context.Observable = &observable;
    context.ReplacementListener =
        [](const i32&, void* user) {
            auto* const context =
                static_cast<FObservableUnsubscribeContext*>(user);
            ++context->ReplacementListenerCalls;
        };

    observable.Subscribe(
        [](const i32&, void* user) {
            auto* const context =
                static_cast<FObservableUnsubscribeContext*>(user);
            ++context->RemovingListenerCalls;
            if (context->Replacement.IsValid()) return;
            if (!context->Observable->Unsubscribe(context->Target)) return;

            ++context->SuccessfulRemovals;
            context->Replacement = context->Observable->Subscribe(
                context->ReplacementListener, context);
        },
        &context);
    context.Target = observable.Subscribe(
        [](const i32&, void* user) {
            auto* const context =
                static_cast<FObservableUnsubscribeContext*>(user);
            ++context->RemovedListenerCalls;
        },
        &context);

    observable.Set(1);

    EXPECT_EQ(context.RemovingListenerCalls, 1);
    EXPECT_EQ(context.SuccessfulRemovals, 1);
    EXPECT_EQ(context.RemovedListenerCalls, 0);
    EXPECT_TRUE(context.Replacement.IsValid());
    EXPECT_FALSE(context.Replacement == context.Target);
    // 通知中に解除した slot を即再利用すると、ここが 1 になり回帰を検出する。
    EXPECT_EQ(context.ReplacementListenerCalls, 0);
    EXPECT_FALSE(observable.Unsubscribe(context.Target));

    observable.Set(2);

    EXPECT_EQ(context.RemovingListenerCalls, 2);
    EXPECT_EQ(context.SuccessfulRemovals, 1);
    EXPECT_EQ(context.RemovedListenerCalls, 0);
    EXPECT_EQ(context.ReplacementListenerCalls, 1);
}

// ---- Observable: callback が所有 object ごと自身を破棄しても Notify は停止 ----
ACS_TEST(Mvvm, ObservableListenerMayDestroyOwningObject) {
    struct FOwner {
        TObservable<i32> Value{ 0 };
    };
    struct FDestroyContext {
        TUniquePtr<FOwner>* Owner = nullptr;
        i32 Calls = 0;
    };

    TUniquePtr<FOwner> owner = MakeUnique<FOwner>();
    FDestroyContext destroy_context{ &owner, 0 };
    i32 following_calls = 0;

    owner->Value.Subscribe(
        [](const i32&, void* user) {
            auto* const context = static_cast<FDestroyContext*>(user);
            ++context->Calls;
            context->Owner->Reset();
        },
        &destroy_context);
    owner->Value.Subscribe(
        [](const i32&, void* user) {
            ++(*static_cast<i32*>(user));
        },
        &following_calls);

    owner->Value.Set(1);
    EXPECT_FALSE(static_cast<bool>(owner));
    EXPECT_EQ(destroy_context.Calls, 1);
    EXPECT_EQ(following_calls, 0);
}

// ---- Observable: nested Notify 中の破棄・同一 storage 再構築を寿命分離する ----
ACS_TEST(Mvvm, ObservableNestedDestroyAndPlacementNewStayLifetimeSeparated) {
    using FScalarObservable = TObservable<i32>;
    alignas(FScalarObservable) u8 storage[sizeof(FScalarObservable)]{};

    struct FNestedDestroyContext {
        FScalarObservable* Current = nullptr;
        void* Storage = nullptr;
        i32 Phase = 0;
        i32 OldFollowingCalls = 0;
        i32 NewCalls = 0;
    } context;

    context.Storage = static_cast<void*>(storage);
    context.Current =
        ::new (context.Storage) FScalarObservable{ 0 };
    context.Current->Subscribe(
        [](const i32& value, void* user) {
            auto* const context =
                static_cast<FNestedDestroyContext*>(user);
            if (value == 1 && context->Phase == 0) {
                context->Phase = 1;
                FScalarObservable* const old_observable =
                    context->Current;
                old_observable->Set(2);
                return;
            }
            if (value != 2 || context->Phase != 1) return;

            context->Phase = 2;
            FScalarObservable* const old_observable =
                context->Current;
            old_observable->~FScalarObservable();
            context->Current =
                ::new (context->Storage) FScalarObservable{ 100 };
            context->Current->Subscribe(
                [](const i32&, void* nested_user) {
                    auto* const nested_context =
                        static_cast<FNestedDestroyContext*>(nested_user);
                    ++nested_context->NewCalls;
                },
                context);
        },
        &context);
    context.Current->Subscribe(
        [](const i32&, void* user) {
            auto* const context =
                static_cast<FNestedDestroyContext*>(user);
            ++context->OldFollowingCalls;
        },
        &context);

    context.Current->Set(1);
    EXPECT_EQ(context.Phase, 2);
    EXPECT_EQ(context.OldFollowingCalls, 0);
    EXPECT_EQ(context.NewCalls, 0);
    EXPECT_EQ(context.Current->Get(), 100);
    EXPECT_EQ(context.Current->SubscriberCount(), u32(1));

    context.Current->Set(101);
    EXPECT_EQ(context.NewCalls, 1);
    context.Current->~FScalarObservable();
    context.Current = nullptr;
}

// ---- TTwoWayBinder: 双方向同期 ---------------------------------------------
ACS_TEST(Mvvm, TwoWayBinderSync) {
    TObservable<f32> a{ 10.0f };
    TObservable<f32> b;
    {
        TTwoWayBinder<f32> bind(a, b);
        EXPECT_EQ(b.Get(), 10.0f);     // 初期同期で a → b

        a.Set(50.0f);
        EXPECT_EQ(b.Get(), 50.0f);     // a 変更 → b に伝搬

        b.Set(25.0f);
        EXPECT_EQ(a.Get(), 25.0f);     // b 変更 → a に伝搬
    }
    // bind 破棄後は同期しない
    a.Set(99.0f);
    EXPECT_EQ(b.Get(), 25.0f);
}

// ---- TTwoWayBinder: 伝播先通知中の同期 self-delete --------------------------
ACS_TEST(Mvvm, TwoWayBinderMayDestroyItselfFromEitherDirection) {
    using FScalarBinder = TTwoWayBinder<i32>;
    struct FDestroyBinderContext {
        TUniquePtr<FScalarBinder>* Binder = nullptr;
        i32 Calls = 0;
    };

    // a -> b の伝播中に b listener が binder を破棄する。
    {
        TObservable<i32> a{ 0 };
        TObservable<i32> b{ 0 };
        TUniquePtr<FScalarBinder> binder;
        FDestroyBinderContext context{ &binder, 0 };
        const FObservableHandle destroy_handle = b.Subscribe(
            [](const i32&, void* user) {
                auto* const context =
                    static_cast<FDestroyBinderContext*>(user);
                ++context->Calls;
                context->Binder->Reset();
                return;
            },
            &context);
        binder = MakeUnique<FScalarBinder>(a, b);

        a.Set(1);

        EXPECT_FALSE(static_cast<bool>(binder));
        EXPECT_EQ(context.Calls, 1);
        EXPECT_EQ(a.Get(), 1);
        EXPECT_EQ(b.Get(), 1);
        EXPECT_EQ(a.SubscriberCount(), u32(0));
        EXPECT_TRUE(b.Unsubscribe(destroy_handle));
    }

    // b -> a の伝播中に a listener が binder を破棄する。
    {
        TObservable<i32> a{ 0 };
        TObservable<i32> b{ 0 };
        TUniquePtr<FScalarBinder> binder;
        FDestroyBinderContext context{ &binder, 0 };
        const FObservableHandle destroy_handle = a.Subscribe(
            [](const i32&, void* user) {
                auto* const context =
                    static_cast<FDestroyBinderContext*>(user);
                ++context->Calls;
                context->Binder->Reset();
                return;
            },
            &context);
        binder = MakeUnique<FScalarBinder>(a, b);

        b.Set(2);

        EXPECT_FALSE(static_cast<bool>(binder));
        EXPECT_EQ(context.Calls, 1);
        EXPECT_EQ(a.Get(), 2);
        EXPECT_EQ(b.Get(), 2);
        EXPECT_EQ(b.SubscriberCount(), u32(0));
        EXPECT_TRUE(a.Unsubscribe(destroy_handle));
    }
}

// ---- TTwoWayBinder: 同一 storage 再構築と old callback の寿命分離 -----------
ACS_TEST(Mvvm, TwoWayBinderPlacementNewStaysLifetimeSeparated) {
    using FScalarBinder = TTwoWayBinder<i32>;
    alignas(FScalarBinder) u8 storage[sizeof(FScalarBinder)]{};

    TObservable<i32> a{ 0 };
    TObservable<i32> b{ 0 };
    struct FPlacementBinderContext {
        FScalarBinder* Current = nullptr;
        void* Storage = nullptr;
        TObservable<i32>* A = nullptr;
        TObservable<i32>* B = nullptr;
        i32 Phase = 0;
    } context;
    context.Storage = static_cast<void*>(storage);
    context.A = &a;
    context.B = &b;

    const FObservableHandle replace_handle = b.Subscribe(
        [](const i32& value, void* user) {
            auto* const context =
                static_cast<FPlacementBinderContext*>(user);
            if (value != 1 || context->Phase != 0) return;

            context->Phase = 1;
            FScalarBinder* const old_binder = context->Current;
            old_binder->~FScalarBinder();
            context->Current = ::new (context->Storage)
                FScalarBinder(*context->A, *context->B);
            context->Phase = 2;
        },
        &context);
    context.Current =
        ::new (context.Storage) FScalarBinder(a, b);

    a.Set(1);
    EXPECT_EQ(context.Phase, 2);
    EXPECT_EQ(a.Get(), 1);
    EXPECT_EQ(b.Get(), 1);

    // 新実体の購読 chain が old callback の epilogue で壊されていないことを確認する。
    b.Set(2);
    EXPECT_EQ(a.Get(), 2);
    EXPECT_EQ(b.Get(), 2);

    context.Current->~FScalarBinder();
    context.Current = nullptr;
    EXPECT_TRUE(b.Unsubscribe(replace_handle));
}

// ---- OneWayBinder: src → dst のみ ------------------------------------------
ACS_TEST(Mvvm, OneWayBinder) {
    TObservable<i32> src{ 7 };
    TObservable<i32> dst;
    TOneWayBinder<i32> bind(src, dst);
    EXPECT_EQ(dst.Get(), 7);

    src.Set(42);
    EXPECT_EQ(dst.Get(), 42);

    // 逆方向は伝搬しない
    dst.Set(0);
    EXPECT_EQ(src.Get(), 42);
}

// ---- Derived: 派生 Observable (lazy) --------------------------------------
ACS_TEST(Mvvm, DerivedRatio) {
    TObservable<f32> hp     { 100.0f };
    TObservable<f32> max_hp { 100.0f };

    TDerived<f32, f32> ratio(
        [](const f32& h, const f32& m) { return m > 0 ? h / m : 0.0f; },
        hp, max_hp);

    EXPECT_EQ(ratio.Get(), 1.0f);

    hp.Set(50.0f);
    EXPECT_EQ(ratio.Get(), 0.5f);

    max_hp.Set(200.0f);
    EXPECT_EQ(ratio.Get(), 0.25f);
}

// ---- TDerived: 出力通知中の同期 self-delete -------------------------------
ACS_TEST(Mvvm, DerivedOutputListenerMayDestroyOwner) {
    using FScalarDerived = TDerived<i32, i32>;
    TObservable<i32> dep{ 1 };
    TUniquePtr<FScalarDerived> derived =
        MakeUnique<FScalarDerived>(
            [](const i32& value) { return value * 2; }, dep);
    struct FDestroyDerivedContext {
        TUniquePtr<FScalarDerived>* Derived = nullptr;
        i32 Calls = 0;
    } context{ &derived, 0 };

    derived->Subscribe(
        [](const i32&, void* user) {
            auto* const context =
                static_cast<FDestroyDerivedContext*>(user);
            ++context->Calls;
            context->Derived->Reset();
            return;
        },
        &context);

    dep.Set(2);

    EXPECT_FALSE(static_cast<bool>(derived));
    EXPECT_EQ(context.Calls, 1);
    EXPECT_EQ(dep.SubscriberCount(), u32(0));
}

// ---- TDerived: 同一 storage 再構築と old callback の寿命分離 ---------------
ACS_TEST(Mvvm, DerivedPlacementNewStaysLifetimeSeparated) {
    using FScalarDerived = TDerived<i32, i32>;
    alignas(FScalarDerived) u8 storage[sizeof(FScalarDerived)]{};

    TObservable<i32> dep{ 1 };
    struct FPlacementDerivedContext {
        FScalarDerived* Current = nullptr;
        void* Storage = nullptr;
        TObservable<i32>* Dep = nullptr;
        i32 Phase = 0;
    } context;
    context.Storage = static_cast<void*>(storage);
    context.Dep = &dep;
    context.Current = ::new (context.Storage)
        FScalarDerived(
            [](const i32& value) { return value * 2; }, dep);
    context.Current->Subscribe(
        [](const i32& value, void* user) {
            auto* const context =
                static_cast<FPlacementDerivedContext*>(user);
            if (value != 4 || context->Phase != 0) return;

            context->Phase = 1;
            FScalarDerived* const old_derived = context->Current;
            old_derived->~FScalarDerived();
            context->Current = ::new (context->Storage)
                FScalarDerived(
                    [](const i32& dep_value) { return dep_value * 10; },
                    *context->Dep);
            context->Phase = 2;
        },
        &context);

    dep.Set(2);
    EXPECT_EQ(context.Phase, 2);
    EXPECT_EQ(context.Current->Get(), 20);
    EXPECT_EQ(dep.SubscriberCount(), u32(1));

    i32 new_output_calls = 0;
    context.Current->Subscribe(
        [](const i32&, void* user) {
            ++(*static_cast<i32*>(user));
        },
        &new_output_calls);
    dep.Set(3);
    EXPECT_EQ(context.Current->Get(), 30);
    EXPECT_EQ(new_output_calls, 1);

    context.Current->~FScalarDerived();
    context.Current = nullptr;
}

// ---- TDefaultConverter: 数値 ↔ FString 変換 ---------------------------------
ACS_TEST(Mvvm, DefaultConverterNumeric) {
    EXPECT_EQ((mvvm::TDefaultConverter<i32, f32>::Convert(5, nullptr)), 5.0f);
    EXPECT_EQ((mvvm::TDefaultConverter<f32, i32>::Convert(3.7f, nullptr)), 3);
    EXPECT_EQ((mvvm::TDefaultConverter<bool, i32>::Convert(true, nullptr)), 1);
    EXPECT_EQ((mvvm::TDefaultConverter<i32, bool>::Convert(0, nullptr)), false);
    EXPECT_EQ((mvvm::TDefaultConverter<i32, bool>::Convert(7, nullptr)), true);
}

ACS_TEST(Mvvm, DefaultConverterStringRoundTrip) {
    FString s = mvvm::TDefaultConverter<i32, FString>::Convert(42, nullptr);
    EXPECT_TRUE(s.Data() != nullptr);
    // 末尾チェックは厳密にやらず、値が "42" を含むかだけ確認
    EXPECT_TRUE(s.Data()[0] == '4' && s.Data()[1] == '2');

    i32 v = mvvm::TDefaultConverter<FString, i32>::Convert(FString{"123"}, nullptr);
    EXPECT_EQ(v, 123);

    bool b = mvvm::TDefaultConverter<FString, bool>::Convert(FString{"true"}, nullptr);
    EXPECT_TRUE(b);

    bool b2 = mvvm::TDefaultConverter<FString, bool>::Convert(FString{"false"}, nullptr);
    EXPECT_FALSE(b2);
}

// ---- Bind ファクトリ: 同型 OneWay -----------------------------------------
ACS_TEST(Mvvm, BindFactorySameType) {
    TObservable<f32> src{ 5.0f };
    TObservable<f32> dst;
    auto bind = Bind(src, dst);     // TOneWayBinder<f32>
    EXPECT_EQ(dst.Get(), 5.0f);

    src.Set(7.5f);
    EXPECT_EQ(dst.Get(), 7.5f);
}

// ---- Bind ファクトリ: 型違いで暗黙変換 (f32 → FString) ----------------------
ACS_TEST(Mvvm, BindFactoryConvert) {
    TObservable<i32>    src{ 100 };
    TObservable<FString> dst;
    auto bind = Bind(src, dst);     // TOneWayConvertBinder<i32, FString>
    EXPECT_TRUE(dst.Get().Data() != nullptr);
    EXPECT_TRUE(dst.Get().Data()[0] == '1');

    src.Set(5);
    EXPECT_EQ(dst.Get().Data()[0], '5');
}

// ---- Command: 実行 + can_execute -----------------------------------------
namespace {
int g_fired = 0;
void OnCmd(void*) { g_fired++; }
} // namespace

ACS_TEST(Mvvm, CommandExecuteAlways) {
    g_fired = 0;
    FCommand cmd(&OnCmd, nullptr);
    EXPECT_TRUE(cmd.CanExecute());
    cmd.Execute();
    EXPECT_EQ(g_fired, 1);
}

ACS_TEST(Mvvm, CommandConditional) {
    g_fired = 0;
    TObservable<bool> can{ false };
    FCommand cmd(&OnCmd, nullptr, &can);

    EXPECT_FALSE(cmd.CanExecute());
    cmd.Execute();              // can=false なので発火しない
    EXPECT_EQ(g_fired, 0);

    can.Set(true);
    EXPECT_TRUE(cmd.CanExecute());
    cmd.Execute();
    EXPECT_EQ(g_fired, 1);
}

// ---- ObservableArray: Inserted / Removed / Changed / Cleared -------------
namespace {
struct FArrayCtx {
    int inserted = 0, removed = 0, changed = 0, cleared = 0;
    int last_idx = -1;
    int last_val = -1;
};
void OnArr(EArrayChange k, usize idx, const i32* v, void* user) {
    auto* c = static_cast<FArrayCtx*>(user);
    c->last_idx = static_cast<int>(idx);
    if (v) c->last_val = *v;
    switch (k) {
        case EArrayChange::Inserted: c->inserted++; break;
        case EArrayChange::Removed:  c->removed++;  break;
        case EArrayChange::Changed:  c->changed++;  break;
        case EArrayChange::Cleared:  c->cleared++;  break;
    }
}
} // namespace

ACS_TEST(Mvvm, ObservableArrayLifecycle) {
    TObservableArray<i32> arr;
    FArrayCtx ctx;
    arr.Subscribe(&OnArr, &ctx);

    arr.PushBack(10);
    EXPECT_EQ(ctx.inserted, 1);
    EXPECT_EQ(ctx.last_idx, 0);
    EXPECT_EQ(ctx.last_val, 10);

    arr.PushBack(20);
    EXPECT_EQ(ctx.inserted, 2);
    EXPECT_EQ(ctx.last_idx, 1);
    EXPECT_EQ(ctx.last_val, 20);

    arr.SetAt(0, 99);
    EXPECT_EQ(ctx.changed, 1);
    EXPECT_EQ(ctx.last_idx, 0);
    EXPECT_EQ(ctx.last_val, 99);

    // 同値 SetAt はスキップ
    arr.SetAt(0, 99);
    EXPECT_EQ(ctx.changed, 1);

    EXPECT_TRUE(arr.Remove(20));
    EXPECT_EQ(ctx.removed, 1);
    EXPECT_EQ(ctx.last_idx, 1);
    EXPECT_EQ(arr.Size(), (usize)1);
    EXPECT_EQ(arr.At(0), 99);
    EXPECT_FALSE(arr.Remove(20));
    EXPECT_EQ(ctx.removed, 1);

    arr.PushBack(30);
    arr.RemoveAt(0);
    EXPECT_EQ(ctx.removed, 2);
    EXPECT_EQ(ctx.last_idx, 0);
    EXPECT_EQ(arr.Size(), (usize)1);
    EXPECT_EQ(arr.At(0), 30);

    arr.Clear();
    EXPECT_EQ(ctx.cleared, 1);
    EXPECT_EQ(arr.Size(), (usize)0);
}

// ---- ObservableArray: Remove 通知中に owner を破棄しても戻り値を返す ------
ACS_TEST(Mvvm, ObservableArrayRemoveListenerMayDestroyOwner) {
    struct FOwner {
        // 値の削除を通知する配列。
        TObservableArray<i32> Values;
    };
    struct FDestroyContext {
        // 通知中に破棄する配列 owner。
        TUniquePtr<FOwner>* Owner = nullptr;
        // owner を破棄した listener の呼び出し回数。
        i32 DestroyingCalls = 0;
        // owner 破棄後に呼ばれてはならない後続 listener の回数。
        i32 LaterCalls = 0;
    };

    // Remove の通知元を所有する object。
    TUniquePtr<FOwner> owner = MakeUnique<FOwner>();
    owner->Values.PushBack(7);
    // listener 間で owner と回数を共有する状態。
    FDestroyContext context{ &owner, 0, 0 };
    owner->Values.Subscribe(
        [](EArrayChange, usize, const i32*, void* user) {
            auto* const destroy = static_cast<FDestroyContext*>(user);
            ++destroy->DestroyingCalls;
            // owner と配列は無効になるため、破棄後は何も参照せず直ちに戻る。
            destroy->Owner->Reset();
            return;
        },
        &context);
    owner->Values.Subscribe(
        [](EArrayChange, usize, const i32*, void* user) {
            ++static_cast<FDestroyContext*>(user)->LaterCalls;
        },
        &context);

    // owner 破棄後も Remove が返す成功結果。
    const bool removed = owner->Values.Remove(7);

    EXPECT_TRUE(removed);
    EXPECT_FALSE(static_cast<bool>(owner));
    EXPECT_EQ(context.DestroyingCalls, 1);
    EXPECT_EQ(context.LaterCalls, 0);
}

// ---- MakeBind / MakeBindConvert: TUniquePtr 版 ------------------------------
ACS_TEST(Mvvm, MakeBindUniquePtr) {
    TObservable<f32> a{ 1.0f };
    TObservable<f32> b;
    auto bind = MakeBind(a, b);
    EXPECT_TRUE(bind.Get() != nullptr);
    EXPECT_EQ(b.Get(), 1.0f);

    a.Set(2.5f);
    EXPECT_EQ(b.Get(), 2.5f);

    bind.Reset();   // 同期解除
    a.Set(99.0f);
    EXPECT_EQ(b.Get(), 2.5f);
}
