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

using namespace acs;

// ---- Observable: Subscribe / Set / 通知 ------------------------------------
namespace {
struct Counter { int hits = 0; f32 last = 0; };
void OnFloatChanged(const f32& v, void* user) {
    auto* c = static_cast<Counter*>(user);
    c->hits++;
    c->last = v;
}
} // namespace

ACS_TEST(Mvvm, ObservableSetTriggersListeners) {
    Observable<f32> hp{ 100.0f };
    Counter c;
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

// ---- TwoWayBinder: 双方向同期 ---------------------------------------------
ACS_TEST(Mvvm, TwoWayBinderSync) {
    Observable<f32> a{ 10.0f };
    Observable<f32> b;
    {
        TwoWayBinder<f32> bind(a, b);
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

// ---- OneWayBinder: src → dst のみ ------------------------------------------
ACS_TEST(Mvvm, OneWayBinder) {
    Observable<i32> src{ 7 };
    Observable<i32> dst;
    OneWayBinder<i32> bind(src, dst);
    EXPECT_EQ(dst.Get(), 7);

    src.Set(42);
    EXPECT_EQ(dst.Get(), 42);

    // 逆方向は伝搬しない
    dst.Set(0);
    EXPECT_EQ(src.Get(), 42);
}

// ---- Derived: 派生 Observable (lazy) --------------------------------------
ACS_TEST(Mvvm, DerivedRatio) {
    Observable<f32> hp     { 100.0f };
    Observable<f32> max_hp { 100.0f };

    Derived<f32, f32> ratio(
        [](const f32& h, const f32& m) { return m > 0 ? h / m : 0.0f; },
        hp, max_hp);

    EXPECT_EQ(ratio.Get(), 1.0f);

    hp.Set(50.0f);
    EXPECT_EQ(ratio.Get(), 0.5f);

    max_hp.Set(200.0f);
    EXPECT_EQ(ratio.Get(), 0.25f);
}

// ---- DefaultConverter: 数値 ↔ String 変換 ---------------------------------
ACS_TEST(Mvvm, DefaultConverterNumeric) {
    EXPECT_EQ((mvvm::DefaultConverter<i32, f32>::Convert(5, nullptr)), 5.0f);
    EXPECT_EQ((mvvm::DefaultConverter<f32, i32>::Convert(3.7f, nullptr)), 3);
    EXPECT_EQ((mvvm::DefaultConverter<bool, i32>::Convert(true, nullptr)), 1);
    EXPECT_EQ((mvvm::DefaultConverter<i32, bool>::Convert(0, nullptr)), false);
    EXPECT_EQ((mvvm::DefaultConverter<i32, bool>::Convert(7, nullptr)), true);
}

ACS_TEST(Mvvm, DefaultConverterStringRoundTrip) {
    String s = mvvm::DefaultConverter<i32, String>::Convert(42, nullptr);
    EXPECT_TRUE(s.Data() != nullptr);
    // 末尾チェックは厳密にやらず、値が "42" を含むかだけ確認
    EXPECT_TRUE(s.Data()[0] == '4' && s.Data()[1] == '2');

    i32 v = mvvm::DefaultConverter<String, i32>::Convert(String{"123"}, nullptr);
    EXPECT_EQ(v, 123);

    bool b = mvvm::DefaultConverter<String, bool>::Convert(String{"true"}, nullptr);
    EXPECT_TRUE(b);

    bool b2 = mvvm::DefaultConverter<String, bool>::Convert(String{"false"}, nullptr);
    EXPECT_FALSE(b2);
}

// ---- Bind ファクトリ: 同型 OneWay -----------------------------------------
ACS_TEST(Mvvm, BindFactorySameType) {
    Observable<f32> src{ 5.0f };
    Observable<f32> dst;
    auto bind = Bind(src, dst);     // OneWayBinder<f32>
    EXPECT_EQ(dst.Get(), 5.0f);

    src.Set(7.5f);
    EXPECT_EQ(dst.Get(), 7.5f);
}

// ---- Bind ファクトリ: 型違いで暗黙変換 (f32 → String) ----------------------
ACS_TEST(Mvvm, BindFactoryConvert) {
    Observable<i32>    src{ 100 };
    Observable<String> dst;
    auto bind = Bind(src, dst);     // OneWayConvertBinder<i32, String>
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
    Command cmd(&OnCmd, nullptr);
    EXPECT_TRUE(cmd.CanExecute());
    cmd.Execute();
    EXPECT_EQ(g_fired, 1);
}

ACS_TEST(Mvvm, CommandConditional) {
    g_fired = 0;
    Observable<bool> can{ false };
    Command cmd(&OnCmd, nullptr, &can);

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
struct ArrayCtx {
    int inserted = 0, removed = 0, changed = 0, cleared = 0;
    int last_idx = -1;
    int last_val = -1;
};
void OnArr(ArrayChange k, usize idx, const i32* v, void* user) {
    auto* c = static_cast<ArrayCtx*>(user);
    c->last_idx = static_cast<int>(idx);
    if (v) c->last_val = *v;
    switch (k) {
        case ArrayChange::Inserted: c->inserted++; break;
        case ArrayChange::Removed:  c->removed++;  break;
        case ArrayChange::Changed:  c->changed++;  break;
        case ArrayChange::Cleared:  c->cleared++;  break;
    }
}
} // namespace

ACS_TEST(Mvvm, ObservableArrayLifecycle) {
    ObservableArray<i32> arr;
    ArrayCtx ctx;
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

    arr.RemoveAt(0);
    EXPECT_EQ(ctx.removed, 1);
    EXPECT_EQ(arr.Size(), (usize)1);

    arr.Clear();
    EXPECT_EQ(ctx.cleared, 1);
    EXPECT_EQ(arr.Size(), (usize)0);
}

// ---- MakeBind / MakeBindConvert: UniquePtr 版 ------------------------------
ACS_TEST(Mvvm, MakeBindUniquePtr) {
    Observable<f32> a{ 1.0f };
    Observable<f32> b;
    auto bind = MakeBind(a, b);
    EXPECT_TRUE(bind.Get() != nullptr);
    EXPECT_EQ(b.Get(), 1.0f);

    a.Set(2.5f);
    EXPECT_EQ(b.Get(), 2.5f);

    bind.Reset();   // 同期解除
    a.Set(99.0f);
    EXPECT_EQ(b.Get(), 2.5f);
}
