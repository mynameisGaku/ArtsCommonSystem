// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// コンポーネント実体化 enabling layer の検証:
//   反射名 → 生成 (engine allocator) → 非テンプレート attach → 汎用列挙 → 反射名取得、
//   そして node 破棄でアロケータ整合に解放されること (#2フル/#3 Play モードの前提)。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ComponentFactory.h"
#include "gameframework/AComponent.h"
#include "gameframework/ANode.h"
#include "gameframework/ReflectCatalog.h"

#include <cstring>   // std::strcmp

using namespace acs;
using namespace acs::game;

// 反射名で生成 → attach → 列挙 → ReflectName 一致 → owner 設定 → 破棄まで一気通貫。
ACS_TEST(ComponentFactory, CreateAttachEnumerate) {
    AcsRegisterEngineTypes();

    TUniquePtr<AComponent> comp = CreateComponentByName("ASprite2DComponent");
    EXPECT_TRUE(comp.Get() != nullptr);
    if (comp.Get() == nullptr) return;

    // インスタンス→反射名の橋 (ACS_GAME_COMPONENT_KIND が #T で実装)。
    EXPECT_TRUE(comp->ReflectName() != nullptr);
    if (comp->ReflectName() != nullptr)
        EXPECT_TRUE(std::strcmp(comp->ReflectName(), "ASprite2DComponent") == 0);

    auto node = NewObject<ANode>();
    AComponent& ref = node->AttachComponent(Move(comp));

    EXPECT_EQ(node->ComponentCount(), 1u);
    EXPECT_TRUE(node->ComponentAt(0) == &ref);
    EXPECT_TRUE(node->ComponentAt(1) == nullptr);
    EXPECT_TRUE(node->ComponentAt(0)->HasOwner());
    EXPECT_TRUE(&node->ComponentAt(0)->Owner() == node.Get());
    EXPECT_TRUE(std::strcmp(node->ComponentAt(0)->ReflectName(), "ASprite2DComponent") == 0);

    // node が scope を抜けるとコンポーネントも解放される。
    // AcsConstruct と TUniquePtr が同一 (engine) アロケータなのでここでクラッシュしない。
}

// 未登録 / 非 Component / Abstract は空を返す (誤実体化しない)。
ACS_TEST(ComponentFactory, UnknownNonComponentAbstractReturnEmpty) {
    AcsRegisterEngineTypes();
    EXPECT_TRUE(CreateComponentByName("NoSuchComponent").Get() == nullptr);   // 未登録
    EXPECT_TRUE(CreateComponentByName("FHealthSystem").Get()   == nullptr);   // System (非 Component)
    EXPECT_TRUE(CreateComponentByName("ATriggerComponent").Get() == nullptr); // 抽象型 (world& ctor)
    EXPECT_TRUE(CreateComponentByName(nullptr).Get() == nullptr);             // null 安全
}

// 複数コンポーネントの attach / 列挙順。
ACS_TEST(ComponentFactory, MultipleComponentsEnumerateInOrder) {
    AcsRegisterEngineTypes();
    auto node = NewObject<ANode>();

    TUniquePtr<AComponent> a = CreateComponentByName("ASprite2DComponent");
    TUniquePtr<AComponent> b = CreateComponentByName("AFire2DComponent");
    EXPECT_TRUE(a.Get() != nullptr && b.Get() != nullptr);
    if (a.Get() == nullptr || b.Get() == nullptr) return;

    node->AttachComponent(Move(a));
    node->AttachComponent(Move(b));
    EXPECT_EQ(node->ComponentCount(), 2u);
    EXPECT_TRUE(std::strcmp(node->ComponentAt(0)->ReflectName(), "ASprite2DComponent") == 0);
    EXPECT_TRUE(std::strcmp(node->ComponentAt(1)->ReflectName(), "AFire2DComponent") == 0);
}
