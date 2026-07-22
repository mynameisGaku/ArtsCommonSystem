// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#include "gameframework/Material2D.h"
#include "render/SubstrateExpression.h"
#include "render/SubstrateMaterial.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace acs;
using namespace acs::game;

ACS_TEST(SubstrateExpressionPersistence,
         AcsmatRoundTripPreservesGraphBindingsAndTextures) {
    FMaterial2D source{};
    source.kind = EMaterialKind::Lit;
    source.substrate.enabled = true;
    source.substrate.root = 0;
    source.substrate.node_count = 1u;
    source.substrate.nodes[0].type = ESubstrateNodeType::Slab;

    FShaderExpressionGraph& graph =
        source.substrate.expression_graph;
    graph.root = 3;
    graph.node_count = 4u;

    graph.nodes[0].op = EShaderExpressionOp::UV0;
    graph.nodes[0].declared_type =
        EShaderExpressionValueType::Float2;

    graph.nodes[1].op =
        EShaderExpressionOp::TextureSample2D;
    graph.nodes[1].declared_type =
        EShaderExpressionValueType::Float4;
    graph.nodes[1].inputs[0] = 0;
    graph.nodes[1].texture_slot = 2u;
    graph.nodes[1].texture_flags =
        ShaderExpressionTextureFlag_LinearFilter |
        ShaderExpressionTextureFlag_Srgb;
    graph.nodes[1].texture_asset_id_low = 0x01234567u;
    graph.nodes[1].texture_asset_id_high = 0x89ABCDEFu;
    graph.nodes[1].value =
        FShaderExpressionValue{0.1f, 0.2f, 0.3f, 1.0f};

    graph.nodes[2].op =
        EShaderExpressionOp::ScalarParameter;
    graph.nodes[2].declared_type =
        EShaderExpressionValueType::Float1;
    graph.nodes[2].parameter_id =
        ShaderExpressionParameterId("Roughness");
    graph.nodes[2].value = FShaderExpressionValue{0.42f};

    graph.nodes[3].op = EShaderExpressionOp::Time;
    graph.nodes[3].declared_type =
        EShaderExpressionValueType::Float1;

    // A vector root contributes the lane associated with each RGB scalar.
    source.substrate.nodes[0].expressions.roots[0] = 1;
    source.substrate.nodes[0].expressions.roots[1] = 1;
    source.substrate.nodes[0].expressions.roots[2] = 1;
    source.substrate.nodes[0].expressions.roots[9] = 2;
    std::snprintf(
        source.substrateExpressionTexturePaths[2],
        sizeof(source.substrateExpressionTexturePaths[2]),
        "%s", "Textures/WaterColor.png");

    char text[64u * 1024u]{};
    const u32 written =
        WriteAcsmatText(source, text, sizeof(text));
    EXPECT_TRUE(written > 0u);

    FMaterial2D loaded{};
    const FMaterial2DLoadResult load =
        TryParseAcsmatText(text, written, loaded);
    EXPECT_TRUE(load.Succeeded());
    EXPECT_TRUE(loaded.substrate.enabled);
    EXPECT_EQ(loaded.substrate.root, 0);
    EXPECT_EQ(loaded.substrate.node_count, 1u);
    EXPECT_EQ(
        loaded.substrate.expression_graph.root,
        3);
    EXPECT_EQ(
        loaded.substrate.expression_graph.node_count,
        4u);

    const FShaderExpressionNode& texture =
        loaded.substrate.expression_graph.nodes[1];
    EXPECT_EQ(texture.op, EShaderExpressionOp::TextureSample2D);
    EXPECT_EQ(
        texture.declared_type,
        EShaderExpressionValueType::Float4);
    EXPECT_EQ(texture.inputs[0], 0);
    EXPECT_EQ(texture.texture_slot, 2u);
    EXPECT_EQ(
        texture.texture_flags,
        static_cast<u8>(
            ShaderExpressionTextureFlag_LinearFilter |
            ShaderExpressionTextureFlag_Srgb));
    EXPECT_EQ(texture.texture_asset_id_low, 0x01234567u);
    EXPECT_EQ(texture.texture_asset_id_high, 0x89ABCDEFu);
    EXPECT_NEAR(texture.value.z, 0.3f, 1.0e-6f);
    EXPECT_TRUE(
        std::strcmp(
            loaded.substrateExpressionTexturePaths[2],
            "Textures/WaterColor.png") == 0);

    for (u32 scalar = 0u; scalar < 3u; ++scalar) {
        EXPECT_EQ(
            loaded.substrate.nodes[0].expressions.roots[scalar],
            1);
    }
    EXPECT_EQ(
        loaded.substrate.nodes[0].expressions.roots[9],
        2);

    const FSubstrateExpressionLinkResult linked =
        CompileSubstrateExpressionLinks(loaded.substrate);
    EXPECT_TRUE(linked.Succeeded());
    EXPECT_EQ(linked.binding_count, 4u);
    EXPECT_EQ(
        linked.expression_program.original_types[1],
        EShaderExpressionValueType::Float4);
}

ACS_TEST(SubstrateExpressionPersistence,
         DuplicateExpressionKeyIsTransactional) {
    FMaterial2D source{};
    source.kind = EMaterialKind::Lit;
    source.substrate.enabled = true;
    source.substrate.root = 0;
    source.substrate.node_count = 1u;
    source.substrate.nodes[0].type = ESubstrateNodeType::Slab;
    source.substrate.expression_graph.root = 0;
    source.substrate.expression_graph.node_count = 1u;
    source.substrate.expression_graph.nodes[0].op =
        EShaderExpressionOp::Time;
    source.substrate.expression_graph.nodes[0].declared_type =
        EShaderExpressionValueType::Float1;

    char text[16u * 1024u]{};
    const u32 written =
        WriteAcsmatText(source, text, sizeof(text));
    EXPECT_TRUE(written > 0u);
    std::string duplicate{text, written};
    duplicate += "substrateExprRoot 0\n";

    FMaterial2D destination{};
    std::snprintf(
        destination.name, sizeof(destination.name),
        "%s", "Unchanged");
    destination.substrate.expression_graph.node_count = 7u;
    const FMaterial2DLoadResult result =
        TryParseAcsmatText(
            duplicate.data(), duplicate.size(), destination);
    EXPECT_EQ(
        result.error,
        EMaterial2DLoadError::DuplicateKey);
    EXPECT_TRUE(std::strcmp(destination.name, "Unchanged") == 0);
    EXPECT_EQ(
        destination.substrate.expression_graph.node_count,
        7u);
}

ACS_TEST(SubstrateExpressionPersistence,
         WriterRejectsInjectedTexturePathLine) {
    FMaterial2D material{};
    material.kind = EMaterialKind::Lit;
    material.substrate.enabled = true;
    material.substrate.root = 0;
    material.substrate.node_count = 1u;
    material.substrate.nodes[0].type =
        ESubstrateNodeType::Slab;
    std::snprintf(
        material.substrateExpressionTexturePaths[0],
        sizeof(
            material.substrateExpressionTexturePaths[0]),
        "%s",
        "Textures/Valid.png\nkind effect");

    char text[16u * 1024u]{};
    EXPECT_EQ(
        WriteAcsmatText(
            material, text, sizeof(text)),
        0u);
    EXPECT_EQ(text[0], '\0');
}
