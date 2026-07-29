// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/ReflectApply.h の検証:
//   ACS_RFIELD_D (offset + 既定値) で反射した «多態 (vtable 付き)» コンポーネント型へ、
//   reflection 経由で authored 値を実メンバへ書き込めることを確認する。
//   = 長年保留だった「エディタ編集値 → 実コンポーネントインスタンス」ブリッジの土台。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Reflect.h"
#include "gameframework/ReflectApply.h"
#include "gameframework/ReflectMethod.h"
#include "math/Vec.h"

using namespace acs;
using namespace acs::game;

static_assert(ReflectFieldDispatchSupported(EFieldKind::Vec4));
static_assert(ReflectFieldDispatchSupported(EFieldKind::ObjectRef));
static_assert(!ReflectFieldDispatchSupported(EFieldKind::String));
static_assert(!ReflectFieldDispatchSupported(static_cast<EFieldKind>(255u)));

// 実コンポーネント同様に «仮想デストラクタ (vtable)» を持つ多態型。public メンバ。
struct FApplyMover {
    virtual ~FApplyMover() noexcept = default;
    f32   speed  = 1.0f;
    i32   count  = 5;
    bool  active = true;
    FVec2 vel{ 0.0f, 0.0f };
};

ACS_REGISTER_COMPONENT(FApplyMover,
    ACS_RFIELD_D(FApplyMover, speed,  acs::game::EFieldKind::F32,  1.0f, 0, 0, 0),
    ACS_RFIELD_D(FApplyMover, count,  acs::game::EFieldKind::I32,  5,    0, 0, 0),
    ACS_RFIELD_D(FApplyMover, active, acs::game::EFieldKind::Bool, 1,    0, 0, 0),
    ACS_RFIELD_D(FApplyMover, vel,    acs::game::EFieldKind::Vec2, 0,   0, 0, 0))

namespace {
const FReflectField* FindField(const FTypeDesc& d, const char* name) noexcept {
    for (u32 i = 0; i < d.field_count; ++i) {
        const char* a = d.fields[i].name; const char* b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) return &d.fields[i];
    }
    return nullptr;
}
} // namespace

// 多態型でも offsetof が «実行時の実オフセット» と一致することを確認 (これが効けば値適用が成立)。
ACS_TEST(ReflectApply, OffsetMatchesRuntimeForPolymorphic) {
    const FTypeDesc* d = FTypeRegistry::Get().FindByName("FApplyMover");
    EXPECT_TRUE(d != nullptr);
    EXPECT_EQ(d->field_count, (u32)4);

    FApplyMover m;
    auto rt = [&](void* member) { return (u32)((unsigned char*)member - (unsigned char*)&m); };

    const FReflectField* fs = FindField(*d, "speed");
    EXPECT_TRUE(fs != nullptr);
    EXPECT_EQ(fs->offset, rt(&m.speed));        // 反射オフセット == 実オフセット
    EXPECT_TRUE(fs->size != 0u);                // 実メンバを持つ (ACS_RPROP ではない)
    EXPECT_TRUE(fs->offset != 0u);              // vptr の後 (= 多態を正しく考慮)
    EXPECT_EQ(FindField(*d, "count")->offset, rt(&m.count));
    EXPECT_EQ(FindField(*d, "vel")->offset,   rt(&m.vel));
}

// 反射 defaults でインスタンスを初期化できる。
ACS_TEST(ReflectApply, ApplyDefaults) {
    const FTypeDesc* d = FTypeRegistry::Get().FindByName("FApplyMover");
    FApplyMover m;
    m.speed = 99.0f; m.count = -1; m.active = false; m.vel = FVec2{ 8, 8 };   // 汚す
    ApplyDefaults(&m, *d);
    EXPECT_TRUE(m.speed == 1.0f);
    EXPECT_EQ(m.count, 5);
    EXPECT_TRUE(m.active == true);
    EXPECT_TRUE(m.vel.x == 0.0f && m.vel.y == 0.0f);
}

// authored 値を名前で実メンバへ書き込める (= エディタ編集値の適用)。
ACS_TEST(ReflectApply, ApplyAuthoredValuesByName) {
    const FTypeDesc* d = FTypeRegistry::Get().FindByName("FApplyMover");
    FApplyMover m;
    f32 v[4];

    v[0] = 7.5f;  EXPECT_TRUE(ApplyValueByName(&m, *d, "speed",  v));
    v[0] = 42;    EXPECT_TRUE(ApplyValueByName(&m, *d, "count",  v));
    v[0] = 0;     EXPECT_TRUE(ApplyValueByName(&m, *d, "active", v));
    v[0] = 3; v[1] = 4; EXPECT_TRUE(ApplyValueByName(&m, *d, "vel", v));

    EXPECT_TRUE(m.speed == 7.5f);          // 実メンバに反映
    EXPECT_EQ(m.count, 42);
    EXPECT_TRUE(m.active == false);
    EXPECT_TRUE(m.vel.x == 3.0f && m.vel.y == 4.0f);

    EXPECT_TRUE(!ApplyValueByName(&m, *d, "nope", v));   // 無い名前は false

    // 読み戻しも一致。
    f32 out[4];
    ReadFieldValue(&m, *FindField(*d, "speed"), out);
    EXPECT_TRUE(out[0] == 7.5f);
    ReadFieldValue(&m, *FindField(*d, "vel"), out);
    EXPECT_TRUE(out[0] == 3.0f && out[1] == 4.0f);
}

// factory で実体化 → 既定値適用 → authored 値適用 → 破棄、の一連が動く。
ACS_TEST(ReflectApply, FactoryCreateThenApply) {
    FTypeRegistry& reg = FTypeRegistry::Get();
    const FTypeDesc* d = reg.FindByName("FApplyMover");
    EXPECT_TRUE(d != nullptr);

    void* obj = reg.Create("FApplyMover");        // engine アロケータで実体化
    EXPECT_TRUE(obj != nullptr);
    ApplyDefaults(obj, *d);
    auto* mover = static_cast<FApplyMover*>(obj);
    EXPECT_TRUE(mover->speed == 1.0f);

    f32 v[4] = { 12.0f, 0, 0, 0 };
    EXPECT_TRUE(ApplyValueByName(obj, *d, "speed", v));
    EXPECT_TRUE(mover->speed == 12.0f);

    reg.Destroy(d->id, obj);
}

// ----- オブジェクト参照プロパティ (EFieldKind::ObjectRef) -----
// 参照先の安定 ID を i32 メンバへ apply/read できる (実行時に id→ノード解決する土台)。
struct FRefHolder {
    virtual ~FRefHolder() noexcept = default;
    i32 target = -1;   // 参照先 ID (-1 = なし)
    f32 speed  = 2.0f;
};

ACS_REGISTER_COMPONENT(FRefHolder,
    ACS_RFIELD_REF(FRefHolder, target),
    ACS_RFIELD_D(FRefHolder, speed, acs::game::EFieldKind::F32, 2.0f, 0, 0, 0))

// target フィールドが ObjectRef 種別で反射され、既定 -1 を持つ。
ACS_TEST(ReflectApply, ObjectRefFieldKind) {
    const FTypeDesc* d = FTypeRegistry::Get().FindByName("FRefHolder");
    EXPECT_TRUE(d != nullptr);
    const FReflectField* f = FindField(*d, "target");
    EXPECT_TRUE(f != nullptr);
    EXPECT_TRUE(f->kind == EFieldKind::ObjectRef);
    EXPECT_TRUE(f->defaults[0] == -1.0f);     // 既定は «なし»
}

// 参照 ID を実メンバへ書き込み / 読み出しできる。
ACS_TEST(ReflectApply, ObjectRefApplyAndRead) {
    const FTypeDesc* d = FTypeRegistry::Get().FindByName("FRefHolder");
    const FReflectField* f = FindField(*d, "target");
    FRefHolder h;
    f32 v[4] = { 42.0f, 0, 0, 0 };            // 参照先 ID = 42
    ApplyFieldValue(&h, *f, v);
    EXPECT_EQ(h.target, 42);
    f32 out[4] = { 0, 0, 0, 0 };
    ReadFieldValue(&h, *f, out);
    EXPECT_TRUE(out[0] == 42.0f);
}

// ----- 関数リフレクション (ACS_FUNCTION / BlueprintCallable / CallInEditor) -----
// 引数なし void メソッドを登録 → 名前で呼べ、フラグが取れる (= 将来の Blueprint/ボタンの土台)。
struct FMethodHost {
    virtual ~FMethodHost() noexcept = default;
    int counter = 0;
    void Bump() noexcept { counter += 10; }
};

ACS_REGISTER_METHOD(FMethodHost, Bump,
    ::acs::game::METHOD_BP_CALLABLE | ::acs::game::METHOD_CALL_IN_EDITOR)

ACS_TEST(ReflectMethod, RegisterAndInvoke) {
    FTypeId owner = AcsTypeHash("FMethodHost");
    auto& reg = FMethodRegistry::Get();
    EXPECT_TRUE(reg.CountOfOwner(owner) >= 1u);
    const FReflectMethod* m = reg.Find(owner, "Bump");
    EXPECT_TRUE(m != nullptr);
    EXPECT_TRUE((m->flags & METHOD_BP_CALLABLE) != 0u);
    EXPECT_TRUE((m->flags & METHOD_CALL_IN_EDITOR) != 0u);

    FMethodHost host;
    EXPECT_TRUE(InvokeMethodByName(owner, &host, "Bump"));
    EXPECT_EQ(host.counter, 10);
    EXPECT_TRUE(InvokeMethodByName(owner, &host, "Bump"));
    EXPECT_EQ(host.counter, 20);
    EXPECT_TRUE(!InvokeMethodByName(owner, &host, "Nope"));   // 不明メソッドは false
}

namespace {

void FirstDynamicMethod(void* self) noexcept
{
    *static_cast<int*>(self) = 1;
}

void SecondDynamicMethod(void* self) noexcept
{
    *static_cast<int*>(self) = 2;
}

} // namespace

ACS_TEST(ReflectMethod, DuplicateSourcesPromoteOnUnregister)
{
    const FTypeId owner = AcsTypeHash("FDynamicMethodLifetimeContract");
    const FReflectMethod first{owner,   "Run",           &FirstDynamicMethod, nullptr,
                               nullptr, METHOD_ARG_NONE, METHOD_ARG_NONE,     METHOD_BP_CALLABLE};
    const FReflectMethod second{owner,   "Run",           &SecondDynamicMethod, nullptr,
                                nullptr, METHOD_ARG_NONE, METHOD_ARG_NONE,      METHOD_CALL_IN_EDITOR};

    FMethodRegistry& registry = FMethodRegistry::Get();
    registry.Register(first);
    registry.Register(second);
    EXPECT_EQ(registry.CountOfOwner(owner), 1u);

    int value = 0;
    EXPECT_TRUE(InvokeMethodByName(owner, &value, "Run"));
    EXPECT_EQ(value, 1);

    // 先に有効だった module/source を外すと、残る登録元へ昇格する。
    EXPECT_TRUE(registry.Unregister(first));
    value = 0;
    EXPECT_TRUE(InvokeMethodByName(owner, &value, "Run"));
    EXPECT_EQ(value, 2);

    EXPECT_TRUE(registry.Unregister(second));
    EXPECT_TRUE(registry.Find(owner, "Run") == nullptr);
    EXPECT_TRUE(!registry.Unregister(second));
}
