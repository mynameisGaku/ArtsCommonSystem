// SPDX-License-Identifier: Apache-2.0
// スマートポインタ群のテスト:
//   TSharedPtr / TWeakPtr / TSharedFromThis（SharedPtr.h）
//   AObject / TObjectPtr / TWeakObjectPtr（ObjectPtr.h）
//   TRc / MakeRc 互換エイリアス（Rc.h）
#include "test/Test.h"
#include "test/Expect.h"
#include "memory/SharedPtr.h"
#include "memory/UniquePtr.h"
#include "memory/ObjectPtr.h"
#include "memory/Rc.h"

using namespace acs;

static_assert(IsSameV<FObject, AObject>, "旧名はAObjectの互換別名である必要があります");

namespace {

int g_live = 0;   // 生存中インスタンス数（各テスト先頭で 0 に戻す）

struct FTracked {
    int value;
    explicit FTracked(int v = 0) noexcept : value(v) { ++g_live; }
    ~FTracked() noexcept { --g_live; }
};

struct FBase    { virtual ~FBase() noexcept = default; int b = 1; };
struct FDerived : FBase { int d = 2; };

struct FSftNode : TSharedFromThis<FSftNode> {
    int id = 0;
    TSharedPtr<FSftNode> Self() noexcept { return AsShared(); }
};

struct ATestObject : AObject {
    int hp;
    explicit ATestObject(int h = 100) noexcept : hp(h) { ++g_live; }
    ~ATestObject() noexcept override { --g_live; }
};

struct AResurrectionProbe : AObject {
    TObjectPtr<AResurrectionProbe>* Destination = nullptr;

    explicit AResurrectionProbe(TObjectPtr<AResurrectionProbe>* InDestination) noexcept
        : Destination(InDestination)
    {
    }

    ~AResurrectionProbe() noexcept override
    {
        *Destination = TObjectPtr<AResurrectionProbe>(this);
    }
};

} // namespace

ACS_TEST(SmartPtr, UniqueDefaultResetCapturesAllocator)
{
    g_live = 0;
    {
        TUniquePtr<FTracked> pointer;
        FTracked* raw = New<FTracked>(DefaultAllocator(), 17);
        EXPECT_TRUE(raw != nullptr);
        pointer.Reset(raw);
        EXPECT_EQ(pointer->value, 17);
        EXPECT_EQ(g_live, 1);
    }
    EXPECT_EQ(g_live, 0);
}

ACS_TEST(SmartPtr, UniqueSelfResetKeepsOwnership)
{
    g_live = 0;
    {
        auto pointer = MakeUnique<FTracked>(23);
        FTracked* raw = pointer.Get();
        pointer.Reset(raw);
        EXPECT_TRUE(pointer.Get() == raw);
        EXPECT_EQ(g_live, 1);
    }
    EXPECT_EQ(g_live, 0);
}

// ---- TSharedPtr -------------------------------------------------------------

ACS_TEST(SmartPtr, SharedBasicRefcount) {
    g_live = 0;
    {
        auto p = MakeShared<FTracked>(42);
        EXPECT_TRUE(p.IsValid());
        EXPECT_EQ(p->value, 42);
        EXPECT_EQ(p.UseCount(), (u32)1);
        EXPECT_EQ(g_live, 1);
        {
            auto q = p;                          // コピー → 強参照 +1
            EXPECT_EQ(p.UseCount(), (u32)2);
            EXPECT_EQ(q->value, 42);
        }
        EXPECT_EQ(p.UseCount(), (u32)1);         // q 破棄
        EXPECT_EQ(g_live, 1);
    }
    EXPECT_EQ(g_live, 0);                         // p 破棄 → 対象も破棄
}

ACS_TEST(SmartPtr, SharedMoveAndReset) {
    g_live = 0;
    auto p = MakeShared<FTracked>(7);
    auto m = static_cast<TSharedPtr<FTracked>&&>(p);   // ムーブ
    EXPECT_TRUE(!p.IsValid());
    EXPECT_TRUE(m.IsValid());
    EXPECT_EQ(m.UseCount(), (u32)1);
    m.Reset();
    EXPECT_TRUE(!m.IsValid());
    EXPECT_EQ(g_live, 0);
}

ACS_TEST(SmartPtr, UpcastDerivedToBase) {
    TSharedPtr<FDerived> d = MakeShared<FDerived>();
    TSharedPtr<FBase> b = d;                      // 派生 → 基底アップキャスト
    EXPECT_TRUE(b.IsValid());
    EXPECT_EQ(b->b, 1);
    EXPECT_EQ(d.UseCount(), (u32)2);
}

// ---- TWeakPtr ---------------------------------------------------------------

ACS_TEST(SmartPtr, WeakLockWhileAlive) {
    g_live = 0;
    auto p = MakeShared<FTracked>(5);
    TWeakPtr<FTracked> w = p;
    EXPECT_TRUE(!w.Expired());
    auto s = w.Lock();
    EXPECT_TRUE(s.IsValid());
    EXPECT_EQ(s->value, 5);
    EXPECT_EQ(p.UseCount(), (u32)2);             // p + s
}

ACS_TEST(SmartPtr, WeakExpiresAfterStrongGone) {
    g_live = 0;
    TWeakPtr<FTracked> w;
    {
        auto p = MakeShared<FTracked>(9);
        w = p;
        EXPECT_TRUE(!w.Expired());
    }
    EXPECT_EQ(g_live, 0);                         // 強参照ゼロで対象は破棄済
    EXPECT_TRUE(w.Expired());                     // 弱参照は残るが期限切れ
    EXPECT_TRUE(!w.Lock().IsValid());             // Lock は空を返す
}

// ---- TSharedFromThis --------------------------------------------------------

ACS_TEST(SmartPtr, SharedFromThisSelf) {
    auto n = MakeShared<FSftNode>();
    n->id = 11;
    auto self = n->Self();                        // メンバ内から自分の TSharedPtr
    EXPECT_TRUE(self.IsValid());
    EXPECT_EQ(self->id, 11);
    EXPECT_EQ(n.UseCount(), (u32)2);              // n と self は同じカウントを共有
}

// ---- TRc 互換エイリアス -----------------------------------------------------

ACS_TEST(SmartPtr, RcAliasStillWorks) {
    TRc<FTracked> p = MakeRc<FTracked>(3);          // 旧名 API
    EXPECT_TRUE(p.IsValid());
    EXPECT_EQ(p->value, 3);
    EXPECT_EQ(p.UseCount(), (u32)1);
}

// ---- TObjectPtr / TWeakObjectPtr -------------------------------------------

ACS_TEST(SmartPtr, ObjectStrongKeepsAlive) {
    g_live = 0;
    {
        TObjectPtr<ATestObject> o = NewObject<ATestObject>(250);
        EXPECT_TRUE(o.IsValid());
        EXPECT_EQ(o->hp, 250);
        EXPECT_EQ(o.UseCount(), (u32)1);
        EXPECT_EQ(g_live, 1);
        TObjectPtr<ATestObject> o2 = o;                  // コピー
        EXPECT_EQ(o.UseCount(), (u32)2);
    }
    EXPECT_EQ(g_live, 0);
}

ACS_TEST(SmartPtr, WeakObjectNullsOnDestroy) {
    g_live = 0;
    TWeakObjectPtr<ATestObject> w;
    {
        TObjectPtr<ATestObject> o = NewObject<ATestObject>(30);
        w = o;
        EXPECT_TRUE(w.IsValid());
        EXPECT_TRUE(w.Get() != nullptr);
        EXPECT_EQ(w.Get()->hp, 30);
    }
    EXPECT_EQ(g_live, 0);
    EXPECT_TRUE(!w.IsValid());                    // 破棄後は無効
    EXPECT_TRUE(w.Get() == nullptr);              // Get は null
    EXPECT_TRUE(!w.Pin().IsValid());              // Pin も空
}

ACS_TEST(SmartPtr, WeakObjectFromRawPointer) {
    g_live = 0;
    TObjectPtr<ATestObject> o = NewObject<ATestObject>(77);
    ATestObject* raw = o.Get();
    TWeakObjectPtr<ATestObject> w(raw);                   // 生ポインタから弱参照を作れる
    EXPECT_TRUE(w.IsValid());
    EXPECT_EQ(w.Get()->hp, 77);
    auto pinned = w.Pin();                        // 生きていれば強参照に昇格
    EXPECT_TRUE(pinned.IsValid());
    EXPECT_EQ(o.UseCount(), (u32)2);              // o + pinned
}

ACS_TEST(SmartPtr, RawObjectPointerCannotResurrectDestroyingObject)
{
    TObjectPtr<AResurrectionProbe> Resurrected;
    TObjectPtr<AResurrectionProbe> Owner = NewObject<AResurrectionProbe>(&Resurrected);
    EXPECT_TRUE(Owner.IsValid());

    Owner.Reset();

    EXPECT_FALSE(Resurrected.IsValid());
}

ACS_TEST(SmartPtr, ReferenceCountsRejectOverflowWithoutWrapping)
{
    sp_detail::FControlBlock Control;
    Control.strong.Store(~u32(0), EMemoryOrder::Release);
    Control.weak.Store(~u32(0), EMemoryOrder::Release);

    EXPECT_FALSE(Control.TryAddStrong());
    EXPECT_FALSE(Control.TryAddWeak());
    EXPECT_EQ(Control.strong.Load(EMemoryOrder::Acquire), ~u32(0));
    EXPECT_EQ(Control.weak.Load(EMemoryOrder::Acquire), ~u32(0));
}
