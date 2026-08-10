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

#include <cmath>

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
    const FTypeDesc* d = CTypeRegistry::Get().FindByName("FApplyMover");
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
    const FTypeDesc* d = CTypeRegistry::Get().FindByName("FApplyMover");
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
    const FTypeDesc* d = CTypeRegistry::Get().FindByName("FApplyMover");
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
    CTypeRegistry& reg = CTypeRegistry::Get();
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
    const FTypeDesc* d = CTypeRegistry::Get().FindByName("FRefHolder");
    EXPECT_TRUE(d != nullptr);
    const FReflectField* f = FindField(*d, "target");
    EXPECT_TRUE(f != nullptr);
    EXPECT_TRUE(f->kind == EFieldKind::ObjectRef);
    EXPECT_TRUE(f->defaults[0] == -1.0f);     // 既定は «なし»
}

// 参照 ID を実メンバへ書き込み / 読み出しできる。
ACS_TEST(ReflectApply, ObjectRefApplyAndRead) {
    const FTypeDesc* d = CTypeRegistry::Get().FindByName("FRefHolder");
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
// 明示的な ACS_REGISTER_METHOD で引数なし void メソッドを登録し、名前呼び出しと指定子フラグを検証する。
struct FMethodHost {
    virtual ~FMethodHost() noexcept = default;
    int counter = 0;
    void Bump() noexcept { counter += 10; }
};

ACS_REGISTER_METHOD(FMethodHost, Bump,
    ::acs::game::METHOD_BP_CALLABLE | ::acs::game::METHOD_CALL_IN_EDITOR)

ACS_TEST(ReflectMethod, RegisterAndInvoke) {
    FTypeId owner = AcsTypeHash("FMethodHost");
    auto& reg = CMethodRegistry::Get();
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

// 数値引数の変換結果と呼出し回数を保持する検証用型。
struct FNumericMethodHost {
    // f32メソッドの呼出し回数。
    int f32_calls = 0;
    // i32メソッドの呼出し回数。
    int i32_calls = 0;
    // 未知の引数種別メソッドの呼出し回数。
    int unknown_calls = 0;
    // 最後に受け取ったf32値。
    f32 f32_value = 0.0f;
    // 最後に受け取ったi32値。
    i32 i32_value = 0;
    // 未知の引数種別メソッドが受け取った文字列。
    const char* unknown_arg = nullptr;

    // f32値を保存して呼出し回数を増やす。
    void SetF32(f32 value) noexcept { ++f32_calls; f32_value = value; }
    // i32値を保存して呼出し回数を増やす。
    void SetI32(i32 value) noexcept { ++i32_calls; i32_value = value; }
    // 未知の引数種別でも文字列をそのまま保持する。
    void SetUnknown(const char* value) noexcept { ++unknown_calls; unknown_arg = value; }
};

ACS_REGISTER_METHOD_F32(FNumericMethodHost, SetF32, METHOD_NONE)
ACS_REGISTER_METHOD_I32(FNumericMethodHost, SetI32, METHOD_NONE)

// 厳密なf32/i32文字列の成功条件と失敗時の不変条件を検証する。
ACS_TEST(ReflectMethod, StrictNumericArguments) {
    // 検証対象型のID。
    const FTypeId owner = AcsTypeHash("FNumericMethodHost");
    // f32引数メソッドの登録。
    const FReflectMethod* f32_method = CMethodRegistry::Get().Find(owner, "SetF32");
    // i32引数メソッドの登録。
    const FReflectMethod* i32_method = CMethodRegistry::Get().Find(owner, "SetI32");
    EXPECT_TRUE(f32_method != nullptr && i32_method != nullptr && f32_method->invokeArg != nullptr && i32_method->invokeArg != nullptr);
    if (f32_method == nullptr || i32_method == nullptr || f32_method->invokeArg == nullptr || i32_method->invokeArg == nullptr) return;

    // 数値引数を受ける検証対象。
    FNumericMethodHost host;
    // f32の正常値と期待値を検証する補助処理。
    auto expect_f32 = [&](const char* text, f32 expected) {
        const int before_calls = host.f32_calls;
        EXPECT_TRUE(InvokeMethodByNameArg(owner, &host, "SetF32", text));
        EXPECT_EQ(host.f32_calls, before_calls + 1);
        EXPECT_TRUE(host.f32_value == expected);
    };
    // i32の正常値と期待値を検証する補助処理。
    auto expect_i32 = [&](const char* text, i32 expected) {
        const int before_calls = host.i32_calls;
        EXPECT_TRUE(InvokeMethodByNameArg(owner, &host, "SetI32", text));
        EXPECT_EQ(host.i32_calls, before_calls + 1);
        EXPECT_EQ(host.i32_value, expected);
    };
    // f32の不正値で呼出し先が実行されないことを検証する補助処理。
    auto expect_f32_invalid = [&](const char* text) {
        const int before_calls = host.f32_calls;
        const f32 before_value = host.f32_value;
        EXPECT_TRUE(!InvokeMethodByNameArg(owner, &host, "SetF32", text));
        EXPECT_EQ(host.f32_calls, before_calls);
        EXPECT_TRUE(host.f32_value == before_value);
    };
    // i32の不正値で呼出し先が実行されないことを検証する補助処理。
    auto expect_i32_invalid = [&](const char* text) {
        const int before_calls = host.i32_calls;
        const i32 before_value = host.i32_value;
        EXPECT_TRUE(!InvokeMethodByNameArg(owner, &host, "SetI32", text));
        EXPECT_EQ(host.i32_calls, before_calls);
        EXPECT_EQ(host.i32_value, before_value);
    };

    expect_f32("+1.5", 1.5f);
    expect_f32("+42", 42.0f);
    expect_f32(".5", 0.5f);
    expect_f32("1.", 1.0f);
    expect_f32("-1.25e+2", -125.0f);
    expect_f32("1e+2", 100.0f);
    expect_f32("-0", -0.0f);
    EXPECT_TRUE(std::signbit(host.f32_value));
    expect_f32("1.40129846e-45", 1.40129846e-45f);
    EXPECT_TRUE(host.f32_value > 0.0f && std::isfinite(host.f32_value));
    expect_f32_invalid("++1");
    expect_f32_invalid("+-1");
    expect_f32_invalid("1e");
    expect_f32_invalid("0x1p0");
    expect_f32_invalid("0x10");
    expect_f32_invalid("nan");
    expect_f32_invalid("NAN");
    expect_f32_invalid("-nan");
    expect_f32_invalid("inf");
    expect_f32_invalid("INF");
    expect_f32_invalid("+INF");
    expect_f32_invalid("+inf");
    expect_f32_invalid("1e-46");
    expect_f32_invalid("3.4028236e38");
    expect_f32_invalid("1.5suffix");
    expect_f32_invalid("1,5");
    expect_f32_invalid("");
    expect_f32_invalid(" ");
    expect_f32_invalid("\t");
    expect_f32_invalid(" 1.5");
    expect_f32_invalid("1.5 ");
    expect_f32_invalid("\t1.5");
    expect_f32_invalid(nullptr);

    expect_i32("+42", 42);
    expect_i32("-2147483648", static_cast<i32>(-2147483647 - 1));
    expect_i32("2147483647", 2147483647);
    expect_i32("+2147483647", 2147483647);
    expect_i32("0", 0);
    expect_i32_invalid("++1");
    expect_i32_invalid("+-1");
    expect_i32_invalid("1e2");
    expect_i32_invalid("0x10");
    expect_i32_invalid("2147483648");
    expect_i32_invalid("-2147483649");
    expect_i32_invalid("1suffix");
    expect_i32_invalid("1,0");
    expect_i32_invalid("");
    expect_i32_invalid(" ");
    expect_i32_invalid("\t");
    expect_i32_invalid(" 1");
    expect_i32_invalid("1 ");
    expect_i32_invalid(nullptr);
}

// 戻り値なしのarg付き経路が同じ数値検査を使うことを検証する。
ACS_TEST(ReflectMethod, StrictNumericReturnFallback) {
    // 検証対象型のID。
    const FTypeId owner = AcsTypeHash("FNumericMethodHost");
    // 数値引数を受ける検証対象。
    FNumericMethodHost host;
    // 数値引数の呼出し前後を確認する1文字出力領域。
    char out[1] = {'x'};
    // 有効なf32入力は出力領域を空にして呼べる。
    EXPECT_TRUE(InvokeMethodByNameRet(owner, &host, "SetF32", "+1.5", out, 1));
    EXPECT_TRUE(out[0] == '\0');
    EXPECT_EQ(host.f32_calls, 1);
    // 有効なi32入力は出力領域を空にして呼べる。
    out[0] = 'x';
    EXPECT_TRUE(InvokeMethodByNameRet(owner, &host, "SetI32", "-7", out, 1));
    EXPECT_TRUE(out[0] == '\0');
    EXPECT_EQ(host.i32_calls, 1);
    // 不正なf32入力は出力領域を空にして呼出し先を実行しない。
    out[0] = 'x';
    EXPECT_TRUE(!InvokeMethodByNameRet(owner, &host, "SetF32", "bad", out, 1));
    EXPECT_TRUE(out[0] == '\0');
    EXPECT_EQ(host.f32_calls, 1);
    // 不正なi32入力は出力領域を空にして呼出し先を実行しない。
    out[0] = 'x';
    EXPECT_TRUE(!InvokeMethodByNameRet(owner, &host, "SetI32", "2147483648", out, 1));
    EXPECT_TRUE(out[0] == '\0');
    EXPECT_EQ(host.i32_calls, 1);
}

// 数値サンクをdispatcherを経由せず呼んでも不正値をmethodへ渡さないことを検証する。
ACS_TEST(ReflectMethod, NumericThunkRejectsInvalid) {
    // 検証対象型のID。
    const FTypeId owner = AcsTypeHash("FNumericMethodHost");
    // f32引数メソッドの登録。
    const FReflectMethod* f32_method = CMethodRegistry::Get().Find(owner, "SetF32");
    // i32引数メソッドの登録。
    const FReflectMethod* i32_method = CMethodRegistry::Get().Find(owner, "SetI32");
    EXPECT_TRUE(f32_method != nullptr && i32_method != nullptr && f32_method->invokeArg != nullptr && i32_method->invokeArg != nullptr);
    if (f32_method == nullptr || i32_method == nullptr || f32_method->invokeArg == nullptr || i32_method->invokeArg == nullptr) return;
    // 直接サンクの検証対象。
    FNumericMethodHost host;
    // 不正入力後も保持する既存値。
    host.f32_value = 12.5f;
    host.i32_value = -9;
    f32_method->invokeArg(&host, "not-number");
    i32_method->invokeArg(&host, "9999999999");
    f32_method->invokeArg(&host, nullptr);
    i32_method->invokeArg(&host, nullptr);
    EXPECT_EQ(host.f32_calls, 0);
    EXPECT_EQ(host.i32_calls, 0);
    EXPECT_TRUE(host.f32_value == 12.5f);
    EXPECT_EQ(host.i32_value, -9);
    f32_method->invokeArg(&host, "+1.25");
    i32_method->invokeArg(&host, "-7");
    EXPECT_EQ(host.f32_calls, 1);
    EXPECT_EQ(host.i32_calls, 1);
}

// F32/I32以外の引数種別が従来どおり文字列を渡すことを検証する。
ACS_TEST(ReflectMethod, UnknownArgumentKindKeepsFallback) {
    // 検証対象型のID。
    const FTypeId owner = AcsTypeHash("FNumericMethodHost");
    // 未知の引数種別を持つ手動登録情報。
    const FReflectMethod unknown{owner, "SetUnknown", nullptr, ACS_RMETHOD_THUNK_STR(FNumericMethodHost, SetUnknown), nullptr, static_cast<u32>(255u), METHOD_ARG_NONE, METHOD_NONE};
    // 手動登録を管理する登録簿。
    CMethodRegistry& registry = CMethodRegistry::Get();
    registry.Register(unknown);
    // 未知の引数種別を受ける検証対象。
    FNumericMethodHost host;
    EXPECT_TRUE(InvokeMethodByNameArg(owner, &host, "SetUnknown", "future"));
    EXPECT_EQ(host.unknown_calls, 1);
    EXPECT_TRUE(host.unknown_arg != nullptr && host.unknown_arg[0] == 'f');
    EXPECT_TRUE(InvokeMethodByNameRet(owner, &host, "SetUnknown", nullptr, nullptr, 0));
    EXPECT_EQ(host.unknown_calls, 2);
    EXPECT_TRUE(host.unknown_arg != nullptr && host.unknown_arg[0] == '\0');
    EXPECT_TRUE(registry.Unregister(unknown));
}

// 数値種別でも引数サンクがない登録は従来のinvokeへ戻ることを検証する。
ACS_TEST(ReflectMethod, NumericKindWithoutArgThunkUsesInvokeFallback) {
    // 検証対象型のID。
    const FTypeId owner = AcsTypeHash("FMethodHost");
    // 数値種別と引数サンクなしを持つ手動登録情報。
    const FReflectMethod malformed{owner, "Fallback", ACS_RMETHOD_THUNK(FMethodHost, Bump), nullptr, nullptr, METHOD_ARG_F32, METHOD_ARG_NONE, METHOD_NONE};
    // 手動登録を管理する登録簿。
    CMethodRegistry& registry = CMethodRegistry::Get();
    registry.Register(malformed);
    // invoke fallbackの検証対象。
    FMethodHost host;
    EXPECT_TRUE(InvokeMethodByNameArg(owner, &host, "Fallback", "not-number"));
    EXPECT_EQ(host.counter, 10);
    EXPECT_TRUE(InvokeMethodByNameRet(owner, &host, "Fallback", "not-number", nullptr, 0));
    EXPECT_EQ(host.counter, 20);
    EXPECT_TRUE(registry.Unregister(malformed));
}

// 戻り値サンクの呼出し回数を保持する検証用型。
struct FReturnMethodHost {
    // f32 戻り値メソッドの呼出し回数。
    int f32_calls = 0;
    // i32 戻り値メソッドの呼出し回数。
    int i32_calls = 0;
    // 文字列戻り値メソッドの呼出し回数。
    int text_calls = 0;

    // f32 戻り値を返し、呼出し回数を記録する。
    f32 GetF32() noexcept { ++f32_calls; return 1.25f; }
    // i32 戻り値を返し、呼出し回数を記録する。
    i32 GetI32() noexcept { ++i32_calls; return 42; }
    // 文字列戻り値を返し、呼出し回数を記録する。
    const char* GetText() noexcept { ++text_calls; return "ok"; }
};

ACS_REGISTER_METHOD_RET_F32(FReturnMethodHost, GetF32, METHOD_NONE)
ACS_REGISTER_METHOD_RET_I32(FReturnMethodHost, GetI32, METHOD_NONE)
ACS_REGISTER_METHOD_RET_STR(FReturnMethodHost, GetText, METHOD_NONE)

// 戻り値用サンクと名前呼出しの出力領域失敗条件を検証する。
ACS_TEST(ReflectMethod, ReturnBufferGuards) {
    // 検証対象型のID。
    const FTypeId owner = AcsTypeHash("FReturnMethodHost");
    // メソッド登録簿。
    CMethodRegistry& registry = CMethodRegistry::Get();
    // f32戻り値メソッドの登録。
    const FReflectMethod* ret_f32 = registry.Find(owner, "GetF32");
    // i32戻り値メソッドの登録。
    const FReflectMethod* ret_i32 = registry.Find(owner, "GetI32");
    // 文字列戻り値メソッドの登録。
    const FReflectMethod* ret_text = registry.Find(owner, "GetText");
    EXPECT_TRUE(ret_f32 != nullptr && ret_i32 != nullptr && ret_text != nullptr && ret_f32->invokeRet != nullptr && ret_i32->invokeRet != nullptr && ret_text->invokeRet != nullptr);
    if (ret_f32 == nullptr || ret_i32 == nullptr || ret_text == nullptr || ret_f32->invokeRet == nullptr || ret_i32->invokeRet == nullptr || ret_text->invokeRet == nullptr) return;
    // 呼出し回数を記録する対象。
    FReturnMethodHost host;
    // 無効な容量でも確認値を保持する出力領域。
    char out[16] = {'x', 'x', '\0'};
    ret_f32->invokeRet(&host, "", nullptr, 0);
    EXPECT_EQ(host.f32_calls, 0);
    ret_i32->invokeRet(&host, "", out, -1);
    EXPECT_TRUE(host.i32_calls == 0 && out[0] == 'x');
    ret_text->invokeRet(&host, "", nullptr, -1);
    EXPECT_TRUE(host.text_calls == 0 && out[0] == 'x');
    EXPECT_TRUE(!InvokeMethodByNameRet(owner, &host, "GetF32", "", nullptr, 8));
    EXPECT_EQ(host.f32_calls, 0);
    EXPECT_TRUE(!InvokeMethodByNameRet(owner, &host, "GetI32", "", out, 0));
    EXPECT_TRUE(host.i32_calls == 0 && out[0] == 'x');
    EXPECT_TRUE(!InvokeMethodByNameRet(owner, &host, "GetText", "", out, -1));
    EXPECT_TRUE(host.text_calls == 0 && out[0] == 'x');
}

// 出力領域、未登録名、戻り値なし呼出しの結果を検証する。
ACS_TEST(ReflectMethod, ReturnBufferResults) {
    // 検証対象型のID。
    const FTypeId owner = AcsTypeHash("FReturnMethodHost");
    // 戻り値呼出し回数を記録する対象。
    FReturnMethodHost host;
    // 戻り値を書き込む出力領域。
    char out[16] = {};
    EXPECT_TRUE(InvokeMethodByNameRet(owner, &host, "GetF32", "", out, 16));
    EXPECT_TRUE(out[0] == '1' && out[1] == '.' && out[2] == '2' && out[3] == '5');
    EXPECT_TRUE(InvokeMethodByNameRet(owner, &host, "GetI32", "", out, 16));
    EXPECT_TRUE(out[0] == '4' && out[1] == '2');
    // 終端だけを保持する出力容量。
    char cap1[1] = {'x'};
    EXPECT_TRUE(InvokeMethodByNameRet(owner, &host, "GetF32", "", cap1, 1));
    EXPECT_TRUE(cap1[0] == '\0');
    // 1文字と終端を保持する出力容量。
    char cap2[2] = {'x', 'x'};
    EXPECT_TRUE(InvokeMethodByNameRet(owner, &host, "GetI32", "", cap2, 2));
    EXPECT_TRUE(cap2[1] == '\0');
    // 未登録名呼出し前の確認値。
    out[0] = 'x';
    EXPECT_TRUE(!InvokeMethodByNameRet(owner, &host, "Unknown", "", out, 16));
    EXPECT_TRUE(out[0] == '\0');
    // 戻り値なし呼出しの対象。
    FMethodHost void_host;
    // 戻り値なし呼出し前の出力確認値。
    out[0] = 'x';
    EXPECT_TRUE(InvokeMethodByNameRet(AcsTypeHash("FMethodHost"), &void_host, "Bump", "", out, 16));
    EXPECT_TRUE(out[0] == '\0');
    EXPECT_TRUE(InvokeMethodByNameRet(AcsTypeHash("FMethodHost"), &void_host, "Bump", "", nullptr, 0));
    EXPECT_TRUE(host.f32_calls == 2 && host.i32_calls == 2 && host.text_calls == 0 && void_host.counter == 20);
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

    CMethodRegistry& registry = CMethodRegistry::Get();
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
