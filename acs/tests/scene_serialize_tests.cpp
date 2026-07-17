// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/SceneSerialize.{h,cpp} の検証:
//   FNode2D ツリーの構造 + 各ノードの FTransform2D + 描画フラグが、
//   バイト列へ往復 (save→load) しても完全に一致することを確認する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/SceneSerialize.h"
#include "gameframework/Node2D.h"
#include "gameframework/Component2D.h"
#include "gameframework/ComponentFactory.h"
#include "gameframework/Reflect.h"
#include "gameframework/ReflectCatalog.h"
#include "gameframework/Transform2D.h"
#include "math/Vec.h"
#include "container/Array.h"

#include <cstring>

using namespace acs;
using namespace acs::game;

// field-reflected なテスト用コンポーネント (public フィールド → 値も往復する)。
struct FSceneTestComp : public FComponent2D {
    ACS_GAME_COMPONENT_KIND(FSceneTestComp)
    f32 power   = 0.0f;
    i32 charges = 0;
};

ACS_COMPONENT(FSceneTestComp,
    ACS_RFIELD(FSceneTestComp, power,   acs::game::EFieldKind::F32),
    ACS_RFIELD(FSceneTestComp, charges, acs::game::EFieldKind::I32))

// 3 階層 (root → A → grandchild, root → B) を非既定 transform / フラグで作り、往復一致を見る。
ACS_TEST(SceneSerialize, RoundTripStructureAndTransforms) {
    auto root = MakeUnique<FNode2D>();
    root->Local().position = FVec2{ 10.0f, 20.0f };

    FNode2D& a = root->AddChild(MakeUnique<FNode2D>());
    a.Local().position = FVec2{ 1.0f, 2.0f };
    a.Local().rotation = 0.5f;
    a.Local().scale    = FVec2{ 2.0f, 3.0f };
    a.SetVisible(false);
    a.SetSortLayer(5);
    a.SetYSortBias(1.5f);
    a.SetChildDrawOrder(FNode2D::EChildDrawOrder::LayerThenY);

    FNode2D& g = a.AddChild(MakeUnique<FNode2D>());
    g.Local().position = FVec2{ -3.0f, -4.0f };
    g.SetEnabled(false);

    FNode2D& b = root->AddChild(MakeUnique<FNode2D>());
    b.Local().position = FVec2{ 7.0f, 8.0f };

    u8 buf[4096];
    const u32 n = SaveNodeTree(root.Get(), buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    TUniquePtr<FNode2D> loaded = LoadNodeTree(buf, n);
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() == nullptr) return;

    // root: 2 children + transform。
    EXPECT_EQ(loaded->ChildCount(), 2u);
    EXPECT_NEAR(loaded->Local().position.x, 10.0f, 1e-4f);
    EXPECT_NEAR(loaded->Local().position.y, 20.0f, 1e-4f);

    // child A: transform + 全フラグ。
    FNode2D* la = loaded->Child(0);
    EXPECT_TRUE(la != nullptr);
    if (la != nullptr) {
        EXPECT_EQ(la->ChildCount(), 1u);
        EXPECT_NEAR(la->Local().position.x, 1.0f, 1e-4f);
        EXPECT_NEAR(la->Local().rotation, 0.5f, 1e-4f);
        EXPECT_NEAR(la->Local().scale.x, 2.0f, 1e-4f);
        EXPECT_NEAR(la->Local().scale.y, 3.0f, 1e-4f);
        EXPECT_TRUE(!la->IsVisible());
        EXPECT_EQ(la->SortLayer(), 5);
        EXPECT_NEAR(la->YSortBias(), 1.5f, 1e-4f);
        EXPECT_TRUE(la->ChildDrawOrder() == FNode2D::EChildDrawOrder::LayerThenY);

        // grandchild。
        FNode2D* lg = la->Child(0);
        EXPECT_TRUE(lg != nullptr);
        if (lg != nullptr) {
            EXPECT_NEAR(lg->Local().position.x, -3.0f, 1e-4f);
            EXPECT_NEAR(lg->Local().position.y, -4.0f, 1e-4f);
            EXPECT_TRUE(!lg->IsEnabled());
        }
    }

    // child B。
    FNode2D* lb = loaded->Child(1);
    EXPECT_TRUE(lb != nullptr);
    if (lb != nullptr) EXPECT_NEAR(lb->Local().position.y, 8.0f, 1e-4f);
}

// 小さすぎるバッファは 0、magic 破損は null (壊れたデータでツリーを作らない)。
ACS_TEST(SceneSerialize, RejectsTinyBufferAndCorruptHeader) {
    auto root = MakeUnique<FNode2D>();
    root->AddChild(MakeUnique<FNode2D>());

    u8 buf[256];
    const u32 n = SaveNodeTree(root.Get(), buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    u8 tiny[4];
    EXPECT_EQ(SaveNodeTree(root.Get(), tiny, sizeof(tiny)), 0u);   // header すら入らない

    buf[0] ^= 0xFFu;   // magic 破損
    EXPECT_TRUE(LoadNodeTree(buf, n).Get() == nullptr);
}

// コンポーネント込みの往復: field-reflected は値も復元、engine コンポーネントは attach を復元。
ACS_TEST(SceneSerialize, ComponentRoundTrip) {
    AcsRegisterEngineTypes();
    auto root = MakeUnique<FNode2D>();
    FNode2D& child = root->AddChild(MakeUnique<FNode2D>());

    // field-reflected コンポーネントを非デフォルト値で attach。
    TUniquePtr<FComponent2D> tc = CreateComponentByName("FSceneTestComp");
    EXPECT_TRUE(tc.Get() != nullptr);
    if (tc.Get() == nullptr) return;
    static_cast<FSceneTestComp*>(tc.Get())->power   = 42.5f;
    static_cast<FSceneTestComp*>(tc.Get())->charges = 9;
    child.AttachComponent(Move(tc));

    // private フィールドのみ (RPROP) の engine コンポーネントも attach。
    TUniquePtr<FComponent2D> sprite = CreateComponentByName("FSprite2DComponent");
    EXPECT_TRUE(sprite.Get() != nullptr);
    if (sprite.Get() != nullptr) child.AttachComponent(Move(sprite));

    u8 buf[8192];
    const u32 n = SaveNodeTree(root.Get(), buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    TUniquePtr<FNode2D> loaded = LoadNodeTree(buf, n);
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() == nullptr) return;

    FNode2D* lc = loaded->Child(0);
    EXPECT_TRUE(lc != nullptr);
    if (lc == nullptr) return;
    EXPECT_EQ(lc->ComponentCount(), 2u);

    // field-reflected コンポーネントの値が復元される。
    FSceneTestComp* rtc = lc->GetComponent<FSceneTestComp>();
    EXPECT_TRUE(rtc != nullptr);
    if (rtc != nullptr) {
        EXPECT_NEAR(rtc->power, 42.5f, 1e-4f);
        EXPECT_EQ(rtc->charges, 9);
    }

    // engine コンポーネントも (attach として) 復元されている。
    bool hasSprite = false;
    for (u32 i = 0; i < lc->ComponentCount(); ++i) {
        const FComponent2D* c = lc->ComponentAt(i);
        if (c != nullptr && c->ReflectName() != nullptr
            && std::strcmp(c->ReflectName(), "FSprite2DComponent") == 0) hasSprite = true;
    }
    EXPECT_TRUE(hasSprite);
}

// 単一ノード (子なし) も往復する。
ACS_TEST(SceneSerialize, SingleNodeRoundTrips) {
    auto root = MakeUnique<FNode2D>();
    root->Local().rotation = 1.25f;

    u8 buf[128];
    const u32 n = SaveNodeTree(root.Get(), buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    TUniquePtr<FNode2D> loaded = LoadNodeTree(buf, n);
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() != nullptr) {
        EXPECT_EQ(loaded->ChildCount(), 0u);
        EXPECT_NEAR(loaded->Local().rotation, 1.25f, 1e-4f);
    }
}

namespace {

/** テスト用: LoadNodeTree のバイナリフォーマットでノード 1 件を手書きする簡易ライタ。 */
struct FSceneBinWriter {
    TArray<u8> bytes;
    void U8 (u8 v)  noexcept { bytes.PushBack(v); }
    void U32(u32 v) noexcept { for (u32 i = 0; i < 4; ++i) bytes.PushBack(static_cast<u8>((v >> (8u * i)) & 0xFFu)); }
    void I32(i32 v) noexcept { U32(static_cast<u32>(v)); }
    void F32(f32 v) noexcept { u32 u = 0; std::memcpy(&u, &v, 4u); U32(u); }
    void Node(i32 parent) noexcept {
        I32(parent);
        F32(0.0f); F32(0.0f);   // position
        F32(0.0f);              // rotation
        F32(1.0f); F32(1.0f);   // scale
        U8(1u); U8(1u);         // enabled / visible
        I32(0);                 // sortLayer
        F32(0.0f);              // ySortBias
        U8(0u);                 // childDrawOrder
        U32(0u);                // component count
    }
};

} // namespace

// 回帰テスト: 敵対的なバイト列で数万深の親チェーンを作られても、深度上限 (512) で
// root 直下へ付け替えて受け付ける。FNode2D の破棄は子 TArray 経由の再帰なので、
// 上限なしで深いままだとテスト終了時のデストラクタでスタックオーバーフローする。
ACS_TEST(SceneSerialize, HostileDeepChainIsDepthCappedWithoutLosingNodes) {
    constexpr u32 kNodes = 20000u;
    FSceneBinWriter w;
    w.U32(kSceneSerializeMagic);
    w.U32(kSceneSerializeVersion);
    w.U32(kNodes);
    w.Node(-1);                                              // root
    for (u32 i = 1; i < kNodes; ++i) w.Node(static_cast<i32>(i - 1));  // 一本鎖

    TUniquePtr<FNode2D> loaded = LoadNodeTree(w.bytes.Data(), static_cast<u32>(w.bytes.Size()));
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() == nullptr) return;

    // 明示スタックで全ノードを数え、最大深度を測る (再帰しない)。
    u32 total = 0, max_depth = 0;
    TArray<const FNode2D*> stack_nodes;
    TArray<u32>            stack_depths;
    stack_nodes.PushBack(loaded.Get());
    stack_depths.PushBack(0u);
    while (stack_nodes.Size() > 0) {
        const FNode2D* n = stack_nodes[stack_nodes.Size() - 1];
        const u32      d = stack_depths[stack_depths.Size() - 1];
        stack_nodes.PopBack();
        stack_depths.PopBack();
        ++total;
        if (d > max_depth) max_depth = d;
        for (u32 c = 0; c < n->ChildCount(); ++c) {
            stack_nodes.PushBack(n->Child(c));
            stack_depths.PushBack(d + 1u);
        }
    }
    EXPECT_EQ(total, kNodes);            // ノードは 1 つも失わない
    EXPECT_TRUE(max_depth <= 512u);      // 深度は上限以下に畳まれる
}
