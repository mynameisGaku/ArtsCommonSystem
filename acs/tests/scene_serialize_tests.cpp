// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/SceneSerialize.{h,cpp} の検証:
//   ANode ツリーの構造 + 各ノードの FTransform3D + 描画フラグが、
//   バイト列へ往復 (save→load) しても完全に一致することを確認する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/SceneSerialize.h"
#include "gameframework/ANode.h"
#include "gameframework/AComponent.h"
#include "gameframework/ComponentFactory.h"
#include "gameframework/Reflect.h"
#include "gameframework/ReflectCatalog.h"
#include "gameframework/Transform2D.h"
#include "gameframework/Transform3D.h"
#include "math/Vec.h"
#include "container/Array.h"

#include <cstring>

using namespace acs;
using namespace acs::game;

// field-reflected なテスト用コンポーネント (public フィールド → 値も往復する)。
struct ASceneTestComponent : public AComponent {
    ACS_GAME_COMPONENT_KIND(ASceneTestComponent)
    f32 power   = 0.0f;
    i32 charges = 0;
};

ACS_COMPONENT(ASceneTestComponent,
    ACS_RFIELD(ASceneTestComponent, power,   acs::game::EFieldKind::F32),
    ACS_RFIELD(ASceneTestComponent, charges, acs::game::EFieldKind::I32))

// NUL 終端のない異常 ReflectName を安全に検証するためのテスト専用コンポーネント。
struct AUnterminatedSceneNameComponent : public AComponent {
    AUnterminatedSceneNameComponent() noexcept {
        for (u32 i = 0; i < sizeof(Name); ++i) Name[i] = 'X';
    }
    const void* Kind() const noexcept override {
        return ComponentKindOf<AUnterminatedSceneNameComponent>();
    }
    const char* ReflectName() const noexcept override { return Name; }
    char Name[256];
};

// 3 階層 (root → A → grandchild, root → B) を非既定 transform / フラグで作り、往復一致を見る。
ACS_TEST(SceneSerialize, RoundTripStructureAndTransforms) {
    auto root = NewObject<ANode>();
    root->Local().position = FVec3{10.0f, 20.0f, 30.0f};

    ANode& a = root->AddChild(NewObject<ANode>());
    a.Local() = FTransform3D{
        FVec3{1.0f, 2.0f, 3.0f},
        FQuat::Euler(0.25f, -0.5f, 0.75f),
        FVec3{2.0f, 3.0f, 4.0f}
    };
    a.SetVisible(false);
    a.SetDrawLayer(5);
    a.SetDrawPriority(7);
    a.SetYSortEnabled(true);
    a.SetYSortBias(1.5f);

    ANode& g = a.AddChild(NewObject<ANode>());
    g.SetPosition2D(FVec2{ -3.0f, -4.0f });
    g.SetEnabled(false);

    ANode& b = root->AddChild(NewObject<ANode>());
    b.SetPosition2D(FVec2{ 7.0f, 8.0f });

    u8 buf[4096];
    const u32 n = SaveNodeTree(root.Get(), buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    FSceneLoadResult load_result = TryLoadNodeTree(buf, n);
    EXPECT_TRUE(load_result.Succeeded());
    EXPECT_EQ(load_result.BytesRead, n);
    EXPECT_EQ(load_result.FormatVersion, kSceneSerializeVersion);
    EXPECT_EQ(load_result.DepthCappedNodeCount, 0u);
    TObjectPtr<ANode> loaded = Move(load_result.Root);
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() == nullptr) return;

    // root: 2 children + transform。
    EXPECT_EQ(loaded->ChildCount(), 2u);
    EXPECT_NEAR(loaded->Position2D().x, 10.0f, 1e-4f);
    EXPECT_NEAR(loaded->Position2D().y, 20.0f, 1e-4f);
    EXPECT_NEAR(loaded->Local().position.z, 30.0f, 1e-4f);

    // child A: transform + 全フラグ。
    ANode* la = loaded->Child(0);
    EXPECT_TRUE(la != nullptr);
    if (la != nullptr) {
        EXPECT_EQ(la->ChildCount(), 1u);
        EXPECT_NEAR(la->Position2D().x, 1.0f, 1e-4f);
        EXPECT_NEAR(la->Local().position.z, 3.0f, 1e-4f);
        EXPECT_NEAR(la->Local().rotation.x, a.Local().rotation.x, 1e-4f);
        EXPECT_NEAR(la->Local().rotation.y, a.Local().rotation.y, 1e-4f);
        EXPECT_NEAR(la->Local().rotation.z, a.Local().rotation.z, 1e-4f);
        EXPECT_NEAR(la->Local().rotation.w, a.Local().rotation.w, 1e-4f);
        EXPECT_NEAR(la->Scale2D().x, 2.0f, 1e-4f);
        EXPECT_NEAR(la->Scale2D().y, 3.0f, 1e-4f);
        EXPECT_NEAR(la->Local().scale.z, 4.0f, 1e-4f);
        EXPECT_TRUE(!la->IsVisible());
        EXPECT_EQ(la->DrawLayer(), 5);
        EXPECT_EQ(la->DrawPriority(), 7);
        EXPECT_TRUE(la->IsYSortEnabled());
        EXPECT_NEAR(la->YSortBias(), 1.5f, 1e-4f);

        // grandchild。
        ANode* lg = la->Child(0);
        EXPECT_TRUE(lg != nullptr);
        if (lg != nullptr) {
            EXPECT_NEAR(lg->Position2D().x, -3.0f, 1e-4f);
            EXPECT_NEAR(lg->Position2D().y, -4.0f, 1e-4f);
            EXPECT_TRUE(!lg->IsEnabled());
        }
    }

    // child B。
    ANode* lb = loaded->Child(1);
    EXPECT_TRUE(lb != nullptr);
    if (lb != nullptr) EXPECT_NEAR(lb->Position2D().y, 8.0f, 1e-4f);
}

// 小さすぎるバッファは 0、magic 破損は null (壊れたデータでツリーを作らない)。
ACS_TEST(SceneSerialize, RejectsTinyBufferAndCorruptHeader) {
    auto root = NewObject<ANode>();
    root->AddChild(NewObject<ANode>());

    u8 buf[256];
    const u32 n = SaveNodeTree(root.Get(), buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    u8 tiny[4];
    EXPECT_EQ(SaveNodeTree(root.Get(), tiny, sizeof(tiny)), 0u);   // header すら入らない

    buf[0] ^= 0xFFu;   // magic 破損
    EXPECT_TRUE(LoadNodeTree(buf, n).Get() == nullptr);
    const FSceneLoadResult invalid_magic = TryLoadNodeTree(buf, n);
    EXPECT_TRUE(!invalid_magic.Succeeded());
    EXPECT_TRUE(invalid_magic.Root.Get() == nullptr);
    EXPECT_EQ(static_cast<u32>(invalid_magic.Error),
              static_cast<u32>(ESceneSerializeError::InvalidMagic));
    EXPECT_TRUE(std::strcmp(SceneSerializeErrorName(invalid_magic.Error), "invalid_magic") == 0);

    const FSceneLoadResult null_input = TryLoadNodeTree(nullptr, n);
    EXPECT_EQ(static_cast<u32>(null_input.Error),
              static_cast<u32>(ESceneSerializeError::NullInput));
}

ACS_TEST(SceneSerialize, TrySaveReportsExactSizeAndDoesNotPartiallyWrite) {
    auto root = NewObject<ANode>();
    root->AddChild(NewObject<ANode>());

    const FSceneSaveResult query = TrySaveNodeTree(root.Get(), nullptr, 0u);
    EXPECT_EQ(static_cast<u32>(query.Error),
              static_cast<u32>(ESceneSerializeError::BufferTooSmall));
    EXPECT_EQ(query.BytesWritten, 0u);
    EXPECT_EQ(query.NodeCount, 2u);
    EXPECT_EQ(query.ComponentCount, 0u);
    EXPECT_TRUE(query.RequiredBytes > 0u);

    u8 tiny[32];
    for (u32 i = 0u; i < sizeof(tiny); ++i) tiny[i] = 0xA5u;
    const FSceneSaveResult insufficient =
        TrySaveNodeTree(root.Get(), tiny, sizeof(tiny));
    EXPECT_EQ(static_cast<u32>(insufficient.Error),
              static_cast<u32>(ESceneSerializeError::BufferTooSmall));
    EXPECT_EQ(insufficient.BytesWritten, 0u);
    EXPECT_EQ(insufficient.RequiredBytes, query.RequiredBytes);
    for (u32 i = 0u; i < sizeof(tiny); ++i) EXPECT_EQ(tiny[i], 0xA5u);

    u8 exact[256];
    const FSceneSaveResult saved = TrySaveNodeTree(root.Get(), exact, sizeof(exact));
    EXPECT_TRUE(saved.Succeeded());
    EXPECT_EQ(saved.BytesWritten, saved.RequiredBytes);
    EXPECT_EQ(saved.RequiredBytes, query.RequiredBytes);

    u8 legacy[256];
    EXPECT_EQ(SaveNodeTree(root.Get(), legacy, sizeof(legacy)), saved.BytesWritten);
    EXPECT_TRUE(std::memcmp(exact, legacy, saved.BytesWritten) == 0);
}

ACS_TEST(SceneSerialize, TrySaveRejectsNullArgumentsWithStableDiagnostics) {
    u8 byte = 0u;
    const FSceneSaveResult null_root = TrySaveNodeTree(nullptr, &byte, 1u);
    EXPECT_EQ(static_cast<u32>(null_root.Error),
              static_cast<u32>(ESceneSerializeError::NullRoot));

    auto root = NewObject<ANode>();
    const FSceneSaveResult null_output = TrySaveNodeTree(root.Get(), nullptr, 1u);
    EXPECT_EQ(static_cast<u32>(null_output.Error),
              static_cast<u32>(ESceneSerializeError::NullOutput));
    EXPECT_TRUE(null_output.RequiredBytes > 0u);
    EXPECT_TRUE(std::strcmp(SceneSerializeErrorName(null_output.Error), "null_output") == 0);
}

ACS_TEST(SceneSerialize, TrySaveSeesValidTreeAfterPublicApiRejectsDuplicateNode) {
    auto root = NewObject<ANode>();
    auto shared_child = NewObject<ANode>();
    ANode& added = root->AddChild(shared_child);
    ANode& rejected = root->AddChild(shared_child);
    EXPECT_TRUE(&added == shared_child.Get());
    EXPECT_TRUE(&rejected == root.Get());
    EXPECT_EQ(root->ChildCount(), 1u);

    u8 bytes[256];
    const FSceneSaveResult result = TrySaveNodeTree(root.Get(), bytes, sizeof(bytes));
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.NodeCount, 2u);
    EXPECT_EQ(SaveNodeTree(root.Get(), bytes, sizeof(bytes)), result.BytesWritten);
}

ACS_TEST(SceneSerialize, TrySaveRejectsUnterminatedComponentName) {
    auto root = NewObject<ANode>();
    root->AddComponent<AUnterminatedSceneNameComponent>();

    u8 bytes[256];
    const FSceneSaveResult result = TrySaveNodeTree(root.Get(), bytes, sizeof(bytes));
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(ESceneSerializeError::InvalidComponentName));
    EXPECT_EQ(result.BytesWritten, 0u);
}

ACS_TEST(SceneSerialize, TrySaveRejectsComponentLimitBeforeWriting) {
    auto root = NewObject<ANode>();
    for (u32 i = 0u; i <= kSceneSerializeMaxComponentCountPerNode; ++i) {
        root->AddComponent<ASceneTestComponent>();
    }

    u8 bytes[256];
    const FSceneSaveResult result = TrySaveNodeTree(root.Get(), bytes, sizeof(bytes));
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(ESceneSerializeError::ComponentLimitExceeded));
    EXPECT_EQ(result.BytesWritten, 0u);
    EXPECT_EQ(result.NodeCount, 1u);
}

// コンポーネント込みの往復: field-reflected は値も復元、engine コンポーネントは attach を復元。
ACS_TEST(SceneSerialize, ComponentRoundTrip) {
    AcsRegisterEngineTypes();
    auto root = NewObject<ANode>();
    ANode& child = root->AddChild(NewObject<ANode>());

    // field-reflected コンポーネントを非デフォルト値で attach。
    TUniquePtr<AComponent> tc = CreateComponentByName("ASceneTestComponent");
    EXPECT_TRUE(tc.Get() != nullptr);
    if (tc.Get() == nullptr) return;
    static_cast<ASceneTestComponent*>(tc.Get())->power   = 42.5f;
    static_cast<ASceneTestComponent*>(tc.Get())->charges = 9;
    child.AttachComponent(Move(tc));

    // private フィールドのみ (RPROP) の engine コンポーネントも attach。
    TUniquePtr<AComponent> sprite = CreateComponentByName("ASprite2DComponent");
    EXPECT_TRUE(sprite.Get() != nullptr);
    if (sprite.Get() != nullptr) child.AttachComponent(Move(sprite));

    u8 buf[8192];
    const u32 n = SaveNodeTree(root.Get(), buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    TObjectPtr<ANode> loaded = LoadNodeTree(buf, n);
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() == nullptr) return;

    ANode* lc = loaded->Child(0);
    EXPECT_TRUE(lc != nullptr);
    if (lc == nullptr) return;
    EXPECT_EQ(lc->ComponentCount(), 2u);

    // field-reflected コンポーネントの値が復元される。
    ASceneTestComponent* rtc = lc->GetComponent<ASceneTestComponent>();
    EXPECT_TRUE(rtc != nullptr);
    if (rtc != nullptr) {
        EXPECT_NEAR(rtc->power, 42.5f, 1e-4f);
        EXPECT_EQ(rtc->charges, 9);
    }

    // engine コンポーネントも (attach として) 復元されている。
    bool hasSprite = false;
    for (u32 i = 0; i < lc->ComponentCount(); ++i) {
        const AComponent* c = lc->ComponentAt(i);
        if (c != nullptr && c->ReflectName() != nullptr
            && std::strcmp(c->ReflectName(), "ASprite2DComponent") == 0) hasSprite = true;
    }
    EXPECT_TRUE(hasSprite);
}

// 単一ノード (子なし) も往復する。
ACS_TEST(SceneSerialize, SingleNodeRoundTrips) {
    auto root = NewObject<ANode>();
    root->SetRotation2D(1.25f);

    u8 buf[128];
    const u32 n = SaveNodeTree(root.Get(), buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    TObjectPtr<ANode> loaded = LoadNodeTree(buf, n);
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() != nullptr) {
        EXPECT_EQ(loaded->ChildCount(), 0u);
        EXPECT_NEAR(loaded->Rotation2D(), 1.25f, 1e-4f);
    }
}

namespace {

/** テスト用: LoadNodeTree のバイナリフォーマットでノード 1 件を手書きする簡易ライタ。 */
struct CSceneBinWriter {
    TArray<u8> bytes;
    void U8 (u8 v)  noexcept { bytes.PushBack(v); }
    void U32(u32 v) noexcept { for (u32 i = 0; i < 4; ++i) bytes.PushBack(static_cast<u8>((v >> (8u * i)) & 0xFFu)); }
    void I32(i32 v) noexcept { U32(static_cast<u32>(v)); }
    void F32(f32 v) noexcept { u32 u = 0; std::memcpy(&u, &v, 4u); U32(u); }
    void Bytes(const char* data, u32 size) noexcept {
        for (u32 i = 0; i < size; ++i) U8(static_cast<u8>(data[i]));
    }
    void Node(i32 parent, u32 component_count = 0u) noexcept {
        I32(parent);
        F32(0.0f); F32(0.0f); F32(0.0f);              // position
        F32(0.0f); F32(0.0f); F32(0.0f); F32(1.0f);   // quaternion
        F32(1.0f); F32(1.0f); F32(1.0f);              // scale
        U8(1u); U8(1u);         // enabled / visible
        I32(0);                 // sortLayer
        I32(0);                 // drawPriority
        U8(0u);                 // ySortEnabled
        F32(0.0f);              // ySortBias
        U32(component_count);
    }
    void LegacyV3Node(i32 parent) noexcept {
        I32(parent);
        F32(6.0f); F32(7.0f);   // position
        F32(0.25f);             // rotation
        F32(2.0f); F32(3.0f);   // scale
        U8(1u); U8(1u);         // enabled / visible
        I32(4);                 // sortLayer
        I32(5);                 // drawPriority
        U8(1u);                 // ySortEnabled
        F32(2.5f);              // ySortBias
        U32(0u);                // component count
    }
    void LegacyV2Node(i32 parent) noexcept {
        I32(parent);
        F32(0.0f); F32(0.0f);   // position
        F32(0.0f);              // rotation
        F32(1.0f); F32(1.0f);   // scale
        U8(1u); U8(1u);         // enabled / visible
        I32(4);                 // sortLayer
        F32(2.5f);              // ySortBias
        U8(2u);                 // childDrawOrder = LayerThenY
        U32(0u);                // component count
    }
};

} // namespace

// 途中で切れた入力は、どの境界でも部分ツリーを返さない。
ACS_TEST(SceneSerialize, RejectsEveryTruncatedPrefixWithoutPartialTree) {
    auto root = NewObject<ANode>();
    root->AddChild(NewObject<ANode>());

    u8 bytes[256];
    const u32 size = SaveNodeTree(root.Get(), bytes, sizeof(bytes));
    EXPECT_TRUE(size > 0u);
    for (u32 prefix_size = 0u; prefix_size < size; ++prefix_size) {
        const FSceneLoadResult result = TryLoadNodeTree(bytes, prefix_size);
        EXPECT_TRUE(!result.Succeeded());
        EXPECT_TRUE(result.Root.Get() == nullptr);
        EXPECT_EQ(static_cast<u32>(result.Error),
                  static_cast<u32>(ESceneSerializeError::TruncatedData));
    }
}

// 親は DFS pre-order 上ですでに出現したノードだけを参照できる。
ACS_TEST(SceneSerialize, RejectsInvalidParentIndexTransactionally) {
    CSceneBinWriter w;
    w.U32(kSceneSerializeMagic);
    w.U32(kSceneSerializeVersion);
    w.U32(2u);
    w.Node(-1);
    w.Node(2);   // 自分自身より後ろを指す不正 index

    const FSceneLoadResult result =
        TryLoadNodeTree(w.bytes.Data(), static_cast<u32>(w.bytes.Size()));
    EXPECT_TRUE(!result.Succeeded());
    EXPECT_TRUE(result.Root.Get() == nullptr);
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(ESceneSerializeError::InvalidStructure));
}

// ヘッダ値だけで巨大確保へ進まないよう、ノード・コンポーネント・payload を上限検証する。
ACS_TEST(SceneSerialize, RejectsDeclaredResourceLimitsBeforeAllocation) {
    CSceneBinWriter excessive_nodes;
    excessive_nodes.U32(kSceneSerializeMagic);
    excessive_nodes.U32(kSceneSerializeVersion);
    excessive_nodes.U32(kSceneSerializeMaxNodeCount + 1u);
    const FSceneLoadResult node_result =
        TryLoadNodeTree(excessive_nodes.bytes.Data(), static_cast<u32>(excessive_nodes.bytes.Size()));
    EXPECT_EQ(static_cast<u32>(node_result.Error),
              static_cast<u32>(ESceneSerializeError::NodeLimitExceeded));

    CSceneBinWriter excessive_components;
    excessive_components.U32(kSceneSerializeMagic);
    excessive_components.U32(kSceneSerializeVersion);
    excessive_components.U32(1u);
    excessive_components.Node(-1, kSceneSerializeMaxComponentCountPerNode + 1u);
    const FSceneLoadResult component_result =
        TryLoadNodeTree(excessive_components.bytes.Data(),
                        static_cast<u32>(excessive_components.bytes.Size()));
    EXPECT_EQ(static_cast<u32>(component_result.Error),
              static_cast<u32>(ESceneSerializeError::ComponentLimitExceeded));

    CSceneBinWriter excessive_payload;
    excessive_payload.U32(kSceneSerializeMagic);
    excessive_payload.U32(kSceneSerializeVersion);
    excessive_payload.U32(1u);
    excessive_payload.Node(-1, 1u);
    excessive_payload.U8(1u);
    excessive_payload.Bytes("X", 1u);
    excessive_payload.U32(kSceneSerializeMaxComponentPayloadBytes + 1u);
    const FSceneLoadResult payload_result =
        TryLoadNodeTree(excessive_payload.bytes.Data(),
                        static_cast<u32>(excessive_payload.bytes.Size()));
    EXPECT_EQ(static_cast<u32>(payload_result.Error),
              static_cast<u32>(ESceneSerializeError::ComponentPayloadLimitExceeded));
}

ACS_TEST(SceneSerialize, RejectsEmptyComponentName) {
    CSceneBinWriter w;
    w.U32(kSceneSerializeMagic);
    w.U32(kSceneSerializeVersion);
    w.U32(1u);
    w.Node(-1, 1u);
    w.U8(0u);
    w.U32(0u);

    const FSceneLoadResult result =
        TryLoadNodeTree(w.bytes.Data(), static_cast<u32>(w.bytes.Size()));
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(ESceneSerializeError::InvalidComponentName));
    EXPECT_TRUE(result.Root.Get() == nullptr);
}

ACS_TEST(SceneSerialize, RejectsCorruptKnownComponentPayloadTransactionally) {
    constexpr const char* kComponentName = "ASceneTestComponent";
    constexpr u32 kComponentNameLength = 19u;

    CSceneBinWriter w;
    w.U32(kSceneSerializeMagic);
    w.U32(kSceneSerializeVersion);
    w.U32(1u);
    w.Node(-1, 1u);
    w.U8(static_cast<u8>(kComponentNameLength));
    w.Bytes(kComponentName, kComponentNameLength);
    w.U32(12u);
    for (u32 i = 0u; i < 12u; ++i) w.U8(0u); // magic/type/count がすべて不正。

    const FSceneLoadResult result =
        TryLoadNodeTree(w.bytes.Data(), static_cast<u32>(w.bytes.Size()));
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(ESceneSerializeError::InvalidComponentPayload));
    EXPECT_TRUE(result.Root.Get() == nullptr);
    EXPECT_TRUE(std::strcmp(SceneSerializeErrorName(result.Error),
                            "invalid_component_payload") == 0);
}

ACS_TEST(SceneSerialize, LoadsLegacyV2DrawSettings) {
    CSceneBinWriter w;
    w.U32(kSceneSerializeMagic);
    w.U32(2u);
    w.U32(1u);
    w.LegacyV2Node(-1);

    FSceneLoadResult result =
        TryLoadNodeTree(w.bytes.Data(), static_cast<u32>(w.bytes.Size()));
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.DepthCappedNodeCount, 0u);
    TObjectPtr<ANode> loaded = Move(result.Root);
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() == nullptr) return;
    EXPECT_EQ(loaded->DrawLayer(), 4);
    EXPECT_EQ(loaded->DrawPriority(), 0);
    EXPECT_TRUE(loaded->IsYSortEnabled());
    EXPECT_NEAR(loaded->YSortBias(), 2.5f, 1e-4f);
}

ACS_TEST(SceneSerialize, LoadsLegacyV3TransformAndDrawSettings) {
    CSceneBinWriter w;
    w.U32(kSceneSerializeMagic);
    w.U32(3u);
    w.U32(1u);
    w.LegacyV3Node(-1);

    FSceneLoadResult result =
        TryLoadNodeTree(w.bytes.Data(), static_cast<u32>(w.bytes.Size()));
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.DepthCappedNodeCount, 0u);
    TObjectPtr<ANode> loaded = Move(result.Root);
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() == nullptr) return;
    EXPECT_NEAR(loaded->Position2D().x, 6.0f, 1e-4f);
    EXPECT_NEAR(loaded->Position2D().y, 7.0f, 1e-4f);
    EXPECT_NEAR(loaded->Local().position.z, 0.0f, 1e-4f);
    EXPECT_NEAR(loaded->Rotation2D(), 0.25f, 1e-4f);
    EXPECT_NEAR(loaded->Scale2D().x, 2.0f, 1e-4f);
    EXPECT_NEAR(loaded->Scale2D().y, 3.0f, 1e-4f);
    EXPECT_NEAR(loaded->Local().scale.z, 1.0f, 1e-4f);
    EXPECT_EQ(loaded->DrawLayer(), 4);
    EXPECT_EQ(loaded->DrawPriority(), 5);
    EXPECT_TRUE(loaded->IsYSortEnabled());
    EXPECT_NEAR(loaded->YSortBias(), 2.5f, 1e-4f);
}

// 回帰テスト: 敵対的なバイト列で数万深の親チェーンを作られても、深度上限 (512) で
// root 直下へ付け替えて受け付ける。ANode の破棄は子 TArray 経由の再帰なので、
// 上限なしで深いままだとテスト終了時のデストラクタでスタックオーバーフローする。
ACS_TEST(SceneSerialize, HostileDeepChainIsDepthCappedWithoutLosingNodes) {
    constexpr u32 kNodes = 20000u;
    CSceneBinWriter w;
    w.U32(kSceneSerializeMagic);
    w.U32(kSceneSerializeVersion);
    w.U32(kNodes);
    w.Node(-1);                                              // root
    for (u32 i = 1; i < kNodes; ++i) w.Node(static_cast<i32>(i - 1));  // 一本鎖

    FSceneLoadResult result =
        TryLoadNodeTree(w.bytes.Data(), static_cast<u32>(w.bytes.Size()));
    EXPECT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.DepthCappedNodeCount > 0u);
    TObjectPtr<ANode> loaded = Move(result.Root);
    EXPECT_TRUE(loaded.Get() != nullptr);
    if (loaded.Get() == nullptr) return;

    // 明示スタックで全ノードを数え、最大深度を測る (再帰しない)。
    u32 total = 0, max_depth = 0;
    TArray<const ANode*> stack_nodes;
    TArray<u32>            stack_depths;
    stack_nodes.PushBack(loaded.Get());
    stack_depths.PushBack(0u);
    while (stack_nodes.Size() > 0) {
        const ANode* n = stack_nodes[stack_nodes.Size() - 1];
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
    EXPECT_TRUE(max_depth <= kSceneSerializeMaxTreeDepth); // 深度は上限以下に畳まれる
}
