// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/Reflect.h + Reflect.cpp の検証:
//   統一リフレクション / 型レジストリ (FTypeRegistry)。
//
// マクロ (ACS_COMPONENT / ACS_STRUCT / ACS_INTERFACE / ACS_REFLECT_ENUM) が
//   ・型を「その種類 (ETypeCategory)」としてマークし
//   ・名前・サイズ・フィールド (種別 / オフセット)・列挙値・ファクトリ・トレイトを反射し
//   ・静的初期化時に FTypeRegistry へ自動登録する
// ことを、エンジンが「型を知らずに」横断利用する経路 (名前/ID 検索・カテゴリ列挙・
// 汎用フィールド書込み・factory 生成/破棄) を実際に踏んで検証する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Reflect.h"
#include "gameframework/ReflectCatalog.h"   // AcsRegisterEngineTypes (実エンジン型カタログ)
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"

#include <cstring>   // std::strcmp

using namespace acs;
using namespace acs::game;

// ===== 反射対象のトイ型 (グローバル空間に定義 → 直後にマクロで反射) ============

// コンポーネント: スカラ 4 フィールド (offset / size を検証しやすい純 POD)。
struct FReflHealth {
    f32  hp;        // offset 0
    f32  max_hp;    // offset 4
    i32  lives;     // offset 8
    bool alive;     // offset 12
};

// 単純な値型 (Struct カテゴリ — Component と区別してカテゴリ絞り込みを検証)。
struct FReflTransform {
    f32 x;
    f32 y;
    f32 rot;
};

// 差し替え可能バックエンド interface (default 構築不可 = Abstract、フィールド 0 個)。
struct IReflBackend {
    virtual ~IReflBackend() noexcept = default;
    virtual void Submit() noexcept = 0;
};

// 列挙体 (value↔name の反射)。
enum class EReflWeapon : u8 { Sword, Bow, Staff };

/** factory の内部ヘッダを含めても過剰整列が維持されることを検証する型。 */
struct alignas(64) FReflAlignedAllocation {
    u64 value = 42u;
};

ACS_COMPONENT(FReflHealth,
    ACS_RFIELD(FReflHealth, hp,     acs::game::EFieldKind::F32),
    ACS_RFIELD(FReflHealth, max_hp, acs::game::EFieldKind::F32),
    ACS_RFIELD(FReflHealth, lives,  acs::game::EFieldKind::I32),
    ACS_RFIELD(FReflHealth, alive,  acs::game::EFieldKind::Bool))

ACS_STRUCT(FReflTransform,
    ACS_RFIELD(FReflTransform, x,   acs::game::EFieldKind::F32),
    ACS_RFIELD(FReflTransform, y,   acs::game::EFieldKind::F32),
    ACS_RFIELD(FReflTransform, rot, acs::game::EFieldKind::F32))

ACS_INTERFACE(IReflBackend)

ACS_REFLECT_ENUM(EReflWeapon,
    ACS_EVAL(EReflWeapon, Sword),
    ACS_EVAL(EReflWeapon, Bow),
    ACS_EVAL(EReflWeapon, Staff))

// ===== テスト ================================================================

ACS_TEST(Reflect, RegisteredByNameAndId) {
    auto& reg = FTypeRegistry::Get();
    const FTypeDesc* d = reg.FindByName("FReflHealth");
    EXPECT_TRUE(d != nullptr);
    if (d == nullptr) return;

    // ID は型名の FNV-1a ハッシュで安定。
    EXPECT_EQ(d->id, AcsTypeHash("FReflHealth"));
    // 同じ記述子が ID 検索からも引ける。
    EXPECT_TRUE(reg.FindById(d->id) == d);
    // カテゴリは Component。
    EXPECT_TRUE(d->category == ETypeCategory::Component);
    // sizeof / alignof が反射されている。
    EXPECT_EQ(d->size, static_cast<u32>(sizeof(FReflHealth)));
    EXPECT_EQ(d->align, static_cast<u32>(alignof(FReflHealth)));
}

ACS_TEST(Reflect, UnknownNameMisses) {
    auto& reg = FTypeRegistry::Get();
    EXPECT_TRUE(reg.FindByName("NoSuchType") == nullptr);
    EXPECT_TRUE(reg.FindById(0xDEADBEEFu) == nullptr);
}

ACS_TEST(Reflect, FieldsReflectKindOffsetSize) {
    const FTypeDesc* d = FTypeRegistry::Get().FindByName("FReflHealth");
    EXPECT_TRUE(d != nullptr);
    if (d == nullptr) return;

    EXPECT_EQ(d->field_count, 4u);
    EXPECT_TRUE(d->fields != nullptr);
    if (d->fields == nullptr || d->field_count < 4) return;

    // フィールド名 / 種別。
    EXPECT_TRUE(std::strcmp(d->fields[0].name, "hp") == 0);
    EXPECT_TRUE(d->fields[0].kind == EFieldKind::F32);
    EXPECT_TRUE(std::strcmp(d->fields[1].name, "max_hp") == 0);
    EXPECT_TRUE(d->fields[2].kind == EFieldKind::I32);
    EXPECT_TRUE(d->fields[3].kind == EFieldKind::Bool);

    // オフセットは offsetof と一致。
    EXPECT_EQ(d->fields[0].offset, static_cast<u32>(offsetof(FReflHealth, hp)));
    EXPECT_EQ(d->fields[1].offset, static_cast<u32>(offsetof(FReflHealth, max_hp)));
    EXPECT_EQ(d->fields[2].offset, static_cast<u32>(offsetof(FReflHealth, lives)));
    EXPECT_EQ(d->fields[3].offset, static_cast<u32>(offsetof(FReflHealth, alive)));

    // サイズも反射。
    EXPECT_EQ(d->fields[0].size, 4u);
    EXPECT_EQ(d->fields[3].size, static_cast<u32>(sizeof(bool)));
}

ACS_TEST(Reflect, FactoryCreateGenericFieldWriteDestroy) {
    auto& reg = FTypeRegistry::Get();
    const FTypeDesc* d = reg.FindByName("FReflHealth");
    EXPECT_TRUE(d != nullptr);
    if (d == nullptr) return;

    // 反射トレイトとして「生成できる」とマークされている。
    EXPECT_TRUE((d->traits & TRAIT_INSTANTIABLE) != 0u);

    // 型を知らずに名前だけで生成。
    void* obj = reg.Create("FReflHealth");
    EXPECT_TRUE(obj != nullptr);
    if (obj == nullptr) return;

    // エンジンが「型を知らずに」フィールドへ書き込む経路: name → offset → 生ポインタ。
    for (u32 i = 0; i < d->field_count; ++i) {
        if (std::strcmp(d->fields[i].name, "max_hp") == 0) {
            *reinterpret_cast<f32*>(static_cast<char*>(obj) + d->fields[i].offset) = 99.0f;
        } else if (std::strcmp(d->fields[i].name, "lives") == 0) {
            *reinterpret_cast<i32*>(static_cast<char*>(obj) + d->fields[i].offset) = 3;
        }
    }

    // 実型から見ても書き込めている。
    auto* h = static_cast<FReflHealth*>(obj);
    EXPECT_NEAR(h->max_hp, 99.0f, 0.001f);
    EXPECT_EQ(h->lives, 3);

    // ID 経由で破棄 (Create の戻り値を渡す)。
    reg.Destroy(d->id, obj);
}

ACS_TEST(Reflect, FactoryDestroyUsesAllocatorCapturedAtCreate)
{
    FSystemAllocator first_allocator;
    FSystemAllocator second_allocator;
    FAllocator* const original_allocator = &DefaultAllocator();

    SetDefaultAllocator(&first_allocator);
    void* const object = AcsConstruct<FReflAlignedAllocation>();
    EXPECT_TRUE(object != nullptr);
    EXPECT_TRUE((reinterpret_cast<uptr>(object) % alignof(FReflAlignedAllocation)) == 0u);
    EXPECT_TRUE(first_allocator.BytesAllocated() > 0u);

    // 破棄時の既定値は別物でも、生成時の first_allocator へ返さなければならない。
    SetDefaultAllocator(&second_allocator);
    AcsDestruct<FReflAlignedAllocation>(object);
    EXPECT_EQ(first_allocator.BytesAllocated(), 0ull);
    EXPECT_EQ(second_allocator.BytesAllocated(), 0ull);

    SetDefaultAllocator(original_allocator);
}

ACS_TEST(Reflect, EnumReflectsValuesAndNames) {
    auto& reg = FTypeRegistry::Get();
    const FTypeDesc* d = reg.FindByName("EReflWeapon");
    EXPECT_TRUE(d != nullptr);
    if (d == nullptr) return;

    EXPECT_TRUE(d->category == ETypeCategory::Enum);
    EXPECT_EQ(d->enum_count, 3u);
    EXPECT_TRUE(d->enum_values != nullptr);
    if (d->enum_values == nullptr || d->enum_count < 3) return;

    EXPECT_TRUE(std::strcmp(d->enum_values[0].name, "Sword") == 0);
    EXPECT_EQ(d->enum_values[0].value, static_cast<i64>(0));
    EXPECT_TRUE(std::strcmp(d->enum_values[1].name, "Bow") == 0);
    EXPECT_EQ(d->enum_values[1].value, static_cast<i64>(1));
    EXPECT_EQ(d->enum_values[2].value, static_cast<i64>(2));

    // Enum はフィールドを持たない。
    EXPECT_EQ(d->field_count, 0u);
    // 列挙体はスクリプトへ公開可能とマークされる。
    EXPECT_TRUE((d->traits & TRAIT_SCRIPTABLE) != 0u);
}

ACS_TEST(Reflect, CategoryFilteringEnumerates) {
    auto& reg = FTypeRegistry::Get();

    // Component カテゴリに FReflHealth が列挙される。
    u32 comp = reg.CountOfCategory(ETypeCategory::Component);
    EXPECT_TRUE(comp >= 1u);
    bool foundHealth = false;
    for (u32 i = 0; i < comp; ++i) {
        const FTypeDesc* d = reg.AtOfCategory(ETypeCategory::Component, i);
        if (d != nullptr && std::strcmp(d->name, "FReflHealth") == 0) foundHealth = true;
    }
    EXPECT_TRUE(foundHealth);

    // Struct カテゴリには FReflTransform。Component とは別カテゴリ。
    u32 strc = reg.CountOfCategory(ETypeCategory::Struct);
    EXPECT_TRUE(strc >= 1u);
    bool foundTransform = false;
    for (u32 i = 0; i < strc; ++i) {
        const FTypeDesc* d = reg.AtOfCategory(ETypeCategory::Struct, i);
        if (d != nullptr && std::strcmp(d->name, "FReflTransform") == 0) foundTransform = true;
    }
    EXPECT_TRUE(foundTransform);

    // 範囲外 nth は nullptr。
    EXPECT_TRUE(reg.AtOfCategory(ETypeCategory::Component, comp) == nullptr);
}

ACS_TEST(Reflect, AbstractInterfaceHasNoFactory) {
    auto& reg = FTypeRegistry::Get();
    const FTypeDesc* d = reg.FindByName("IReflBackend");
    EXPECT_TRUE(d != nullptr);
    if (d == nullptr) return;

    EXPECT_TRUE(d->category == ETypeCategory::Interface);
    // 純粋仮想を持つ → default 構築不可 → Abstract、Instantiable は立たない。
    EXPECT_TRUE((d->traits & TRAIT_ABSTRACT) != 0u);
    EXPECT_TRUE((d->traits & TRAIT_INSTANTIABLE) == 0u);
    // フィールド 0 個でも反射できる。
    EXPECT_EQ(d->field_count, 0u);
    // factory は生成不可 (nullptr)。
    EXPECT_TRUE(reg.Create("IReflBackend") == nullptr);
}

ACS_TEST(Reflect, TypeDescOfMatchesRegistry) {
    auto& reg = FTypeRegistry::Get();
    // コンパイル時の AcsTypeDescOf<T>() が実行時レジストリの記述子と一致する。
    EXPECT_TRUE(AcsTypeDescOf<FReflHealth>() == reg.FindByName("FReflHealth"));
    EXPECT_TRUE(AcsTypeDescOf<EReflWeapon>() == reg.FindByName("EReflWeapon"));
    EXPECT_TRUE(AcsTypeDescOf<FReflTransform>() == reg.FindByName("FReflTransform"));
}

ACS_TEST(Reflect, DynamicRegistrationSourcesRetireWithoutDangling)
{
    FTypeRegistry& registry = FTypeRegistry::Get();
    const u32 count_before = registry.Count();

    FTypeDesc first{};
    first.name = "FDynamicEditorContract";
    first.id = AcsTypeHash(first.name);
    first.category = ETypeCategory::Component;

    FTypeDesc second = first;
    EXPECT_TRUE(registry.Register(&first));
    EXPECT_TRUE(registry.Register(&second));
    EXPECT_EQ(registry.Count(), count_before + 1u);
    EXPECT_TRUE(registry.FindById(first.id) == &first);

    // 後から登録した同 ID の記述子だけを外しても、先行登録は維持される。
    EXPECT_TRUE(registry.Unregister(&second));
    EXPECT_TRUE(registry.FindById(first.id) == &first);

    // 先行登録を外した場合は、残っている次の登録元へ検索結果を切り替える。
    EXPECT_TRUE(registry.Register(&second));
    EXPECT_TRUE(registry.Unregister(&first));
    EXPECT_TRUE(registry.FindById(second.id) == &second);

    // 最後の登録元を外すと型そのものが列挙対象から消える。
    EXPECT_TRUE(registry.Unregister(&second));
    EXPECT_TRUE(registry.FindById(second.id) == nullptr);
    EXPECT_EQ(registry.Count(), count_before);
    EXPECT_TRUE(!registry.Unregister(&second));
}

ACS_TEST(Reflect, AutoRegistrationRetiresOnlyItsOwnSource)
{
    FTypeRegistry& registry = FTypeRegistry::Get();
    const u32 count_before = registry.Count();

    FTypeDesc first{};
    first.name = "FAutoRegistrationContract";
    first.id = AcsTypeHash(first.name);
    first.category = ETypeCategory::Component;

    FTypeDesc second = first;
    {
        FTypeAutoRegister first_registration(&first);
        EXPECT_TRUE(registry.Register(&second));
        EXPECT_EQ(registry.Count(), count_before + 1u);
        EXPECT_TRUE(registry.FindById(first.id) == &first);
    }

    // 自動登録元の破棄後も、同じ ID の別登録元を検索結果として維持する。
    EXPECT_TRUE(registry.FindById(second.id) == &second);
    EXPECT_TRUE(registry.Unregister(&second));
    EXPECT_TRUE(registry.FindById(second.id) == nullptr);
    EXPECT_EQ(registry.Count(), count_before);
}

// ===== 実エンジン型カタログ (ReflectCatalog) の検証 ==========================
// エンジン同梱の Component / System / Scene / Asset / enum が「型ヘッダ非侵襲」で
// レジストリに載り、エンジンが自分の型を名前/カテゴリで把握・生成できることを確認。

ACS_TEST(ReflectCatalog, RegistersConcreteEngineTypes) {
    // カタログ TU をリンクへ引き込み、自動登録を確定する (冪等)。
    u32 n = AcsRegisterEngineTypes();
    EXPECT_TRUE(n > 0u);

    auto& reg = FTypeRegistry::Get();

    const FTypeDesc* sprite = reg.FindByName("FSprite2DComponent");
    EXPECT_TRUE(sprite != nullptr);
    if (sprite != nullptr) EXPECT_TRUE(sprite->category == ETypeCategory::Component);

    const FTypeDesc* health = reg.FindByName("FHealthSystem");
    EXPECT_TRUE(health != nullptr);
    if (health != nullptr) EXPECT_TRUE(health->category == ETypeCategory::System);

    const FTypeDesc* scene = reg.FindByName("FScene2D");
    EXPECT_TRUE(scene != nullptr);
    if (scene != nullptr) EXPECT_TRUE(scene->category == ETypeCategory::Scene);

    const FTypeDesc* image = reg.FindByName("FImageAsset");
    EXPECT_TRUE(image != nullptr);
    if (image != nullptr) EXPECT_TRUE(image->category == ETypeCategory::Asset);

    // ID 検索も一致する。
    if (sprite != nullptr)
        EXPECT_TRUE(reg.FindById(AcsTypeHash("FSprite2DComponent")) == sprite);
}

ACS_TEST(ReflectCatalog, CategoryCounts) {
    AcsRegisterEngineTypes();
    auto& reg = FTypeRegistry::Get();
    // カタログが各カテゴリを網羅登録している (テスト型も含むので >= で確認)。
    EXPECT_TRUE(reg.CountOfCategory(ETypeCategory::Component) >= 9u);
    EXPECT_TRUE(reg.CountOfCategory(ETypeCategory::System)    >= 30u);
    EXPECT_TRUE(reg.CountOfCategory(ETypeCategory::Asset)     >= 6u);
    EXPECT_TRUE(reg.CountOfCategory(ETypeCategory::Scene)     >= 1u);
    EXPECT_TRUE(reg.CountOfCategory(ETypeCategory::Enum)      >= 10u);
}

ACS_TEST(ReflectCatalog, EngineEnumsReflected) {
    AcsRegisterEngineTypes();
    auto& reg = FTypeRegistry::Get();

    const FTypeDesc* blend = reg.FindByName("EBlendMode");
    EXPECT_TRUE(blend != nullptr);
    if (blend != nullptr) {
        EXPECT_TRUE(blend->category == ETypeCategory::Enum);
        EXPECT_EQ(blend->enum_count, 3u);
        if (blend->enum_count >= 3) {
            EXPECT_TRUE(std::strcmp(blend->enum_values[0].name, "Opaque") == 0);
            EXPECT_EQ(blend->enum_values[0].value, static_cast<i64>(0));
            EXPECT_TRUE(std::strcmp(blend->enum_values[2].name, "Additive") == 0);
        }
    }

    const FTypeDesc* bt = reg.FindByName("EBtStatus");
    EXPECT_TRUE(bt != nullptr);
    if (bt != nullptr) EXPECT_EQ(bt->enum_count, 3u);
}

// 補完した enum が「全列挙子」を反射していることを固定する (不完全反射の回帰防止)。
// 1 つでも欠けると serializer / editor が値→名前で破綻するため、件数で厳密に検証。
ACS_TEST(ReflectCatalog, EnumsAreComplete) {
    AcsRegisterEngineTypes();
    auto& reg = FTypeRegistry::Get();

    struct Expect { const char* name; u32 count; };
    const Expect kExpect[] = {
        { "ECompareFunc",   8u },   // Never..Always (既定 Always を含む)
        { "ECombatState",   6u },   // Peaceful..Retreat (実機が遷移する全状態)
        { "EResourceState", 9u },   // Common..DepthRead
        { "EPixelFormat",   6u },   // R8..R32G32B32A32_F (HDR を含む)
        { "EBtDecoratorOp", 4u },   // Inverter..Repeat
    };
    for (const auto& e : kExpect) {
        const FTypeDesc* d = reg.FindByName(e.name);
        EXPECT_TRUE(d != nullptr);
        if (d != nullptr) EXPECT_EQ(d->enum_count, e.count);
    }

    // 以前欠落していた代表値が name→value で解決できることを直接確認。
    const FTypeDesc* cmp = reg.FindByName("ECompareFunc");
    if (cmp != nullptr) {
        bool hasAlways = false;
        for (u32 i = 0; i < cmp->enum_count; ++i)
            if (std::strcmp(cmp->enum_values[i].name, "Always") == 0) hasAlways = true;
        EXPECT_TRUE(hasAlways);
    }
}

ACS_TEST(ReflectCatalog, FactoryRespectsConstructibility) {
    AcsRegisterEngineTypes();
    auto& reg = FTypeRegistry::Get();

    // FTriggerComponent は ctor が world& 必須 → 既定構築不可 → Abstract、factory なし。
    const FTypeDesc* trig = reg.FindByName("FTriggerComponent");
    EXPECT_TRUE(trig != nullptr);
    if (trig != nullptr) {
        EXPECT_TRUE((trig->traits & TRAIT_ABSTRACT) != 0u);
        EXPECT_TRUE(reg.Create("FTriggerComponent") == nullptr);
    }

    // INSTANTIABLE な型は factory で生成→破棄できる (FScene2D が既定構築可能なら踏む)。
    const FTypeDesc* scene = reg.FindByName("FScene2D");
    if (scene != nullptr && (scene->traits & TRAIT_INSTANTIABLE) != 0u) {
        void* obj = reg.Create("FScene2D");
        EXPECT_TRUE(obj != nullptr);
        if (obj != nullptr) reg.Destroy(scene->id, obj);
    }
}
