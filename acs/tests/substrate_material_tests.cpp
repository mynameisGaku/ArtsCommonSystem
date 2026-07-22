// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/SubstrateMaterial.h"
#include "gameframework/Material2D.h"

#include <cstring>

using namespace acs;
using namespace acs::game;

namespace {

FSubstrateMaterial TwoSlabs(ESubstrateNodeType op, f32 factor = 0.5f) noexcept {
    FSubstrateMaterial m{};
    m.enabled = true;
    m.node_count = 3;
    m.root = 2;
    m.nodes[0].type = ESubstrateNodeType::Slab;
    m.nodes[0].input_a = m.nodes[0].input_b = kSubstrateInvalidNode;
    m.nodes[0].slab.diffuse_albedo = FVec3{1,0,0};
    m.nodes[1].type = ESubstrateNodeType::Slab;
    m.nodes[1].input_a = m.nodes[1].input_b = kSubstrateInvalidNode;
    m.nodes[1].slab.diffuse_albedo = FVec3{0,0,1};
    m.nodes[2].type = op;
    m.nodes[2].input_a = 0;
    m.nodes[2].input_b = 1;
    m.nodes[2].factor = factor;
    return m;
}

} // namespace

ACS_TEST(SubstrateMaterial, RejectsInvalidRootInputAndCycle) {
    FSubstrateMaterial invalid_root = TwoSlabs(ESubstrateNodeType::Add);
    invalid_root.root = 31;
    EXPECT_EQ(CompileSubstrateMaterial(invalid_root).error,
              ESubstrateCompileError::InvalidRoot);

    FSubstrateMaterial invalid_input = TwoSlabs(ESubstrateNodeType::Add);
    invalid_input.nodes[2].input_b = 9;
    EXPECT_EQ(CompileSubstrateMaterial(invalid_input).error,
              ESubstrateCompileError::InvalidInput);

    FSubstrateMaterial cycle = TwoSlabs(ESubstrateNodeType::HorizontalBlend);
    cycle.nodes[2].input_a = 2;
    const FSubstrateCompileResult cycle_result = CompileSubstrateMaterial(cycle);
    EXPECT_EQ(cycle_result.error, ESubstrateCompileError::Cycle);
    EXPECT_EQ(cycle_result.error_node, 2);
}

ACS_TEST(SubstrateMaterial, CoverageHorizontalAddAndSelect) {
    FSubstrateMaterial horizontal = TwoSlabs(ESubstrateNodeType::HorizontalBlend, 0.25f);
    FSubstrateCompileResult h = CompileSubstrateMaterial(horizontal);
    EXPECT_TRUE(h.Succeeded());
    EXPECT_EQ(h.lobe_count, 2u);
    EXPECT_NEAR(h.lobes[0].weight, 0.75f, 1e-5f);
    EXPECT_NEAR(h.lobes[1].weight, 0.25f, 1e-5f);

    FSubstrateMaterial coverage{};
    coverage.enabled = true;
    coverage.node_count = 2;
    coverage.root = 1;
    coverage.nodes[0].type = ESubstrateNodeType::Slab;
    coverage.nodes[0].input_a = coverage.nodes[0].input_b = -1;
    coverage.nodes[1].type = ESubstrateNodeType::CoverageWeight;
    coverage.nodes[1].input_a = 0;
    coverage.nodes[1].input_b = -1;
    coverage.nodes[1].factor = 0.3f;
    FSubstrateCompileResult c = CompileSubstrateMaterial(coverage);
    EXPECT_TRUE(c.Succeeded());
    EXPECT_EQ(c.lobe_count, 1u);
    EXPECT_NEAR(c.lobes[0].coverage, 0.3f, 1e-5f);

    FSubstrateMaterial add = TwoSlabs(ESubstrateNodeType::Add);
    FSubstrateCompileResult a = CompileSubstrateMaterial(add);
    EXPECT_TRUE(a.Succeeded());
    EXPECT_FALSE(a.stats.energy_conserving);
    EXPECT_TRUE((a.stats.feature_bits & SubstrateFeature_NonPhysicalAdd) != 0u);

    FSubstrateMaterial select = TwoSlabs(ESubstrateNodeType::Select, 1.0f);
    FSubstrateCompileResult s = CompileSubstrateMaterial(select);
    EXPECT_TRUE(s.Succeeded());
    EXPECT_EQ(s.lobe_count, 1u);
    EXPECT_NEAR(s.lobes[0].slab.diffuse_albedo.z, 1.0f, 1e-5f);
}

ACS_TEST(SubstrateMaterial, VerticalLayerUsesPhysicalTopThickness) {
    FSubstrateMaterial vertical = TwoSlabs(ESubstrateNodeType::VerticalLayer, 2.0f);
    // Top is input A.  Its authored transmittance is defined through 1 cm.
    vertical.nodes[0].slab.transmittance = FVec3{0.5f,0.5f,0.5f};
    vertical.nodes[0].slab.thickness_cm = 1.0f;
    FSubstrateCompileResult result = CompileSubstrateMaterial(vertical);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.lobe_count, 2u);
    // T(2 cm) = pow(T(1 cm), 2/1) = 0.25.
    EXPECT_NEAR(result.lobes[1].weight, 0.25f, 1e-4f);
    EXPECT_EQ(result.lobes[1].vertical_depth, 1u);
}

ACS_TEST(SubstrateMaterial, ParameterBlendingReducesClosureAndBudget) {
    FSubstrateMaterial full = TwoSlabs(ESubstrateNodeType::HorizontalBlend, 0.5f);
    const FSubstrateCompileResult unblended = CompileSubstrateMaterial(full);
    full.nodes[2].flags |= SubstrateNodeFlag_ParameterBlending;
    const FSubstrateCompileResult blended = CompileSubstrateMaterial(full);
    EXPECT_TRUE(unblended.Succeeded());
    EXPECT_TRUE(blended.Succeeded());
    EXPECT_EQ(unblended.stats.closure_count, 2u);
    EXPECT_EQ(blended.stats.closure_count, 1u);
    EXPECT_TRUE(blended.stats.estimated_bytes_per_pixel <
                unblended.stats.estimated_bytes_per_pixel);
    EXPECT_TRUE((blended.stats.feature_bits & SubstrateFeature_ParameterBlend) != 0u);
}

ACS_TEST(SubstrateMaterial, DedicatedWaterBsdfReportsComplexSpecial) {
    FSubstrateMaterial water{};
    water.enabled = true;
    water.node_count = 1u;
    water.root = 0;
    water.nodes[0].type = ESubstrateNodeType::Slab;
    water.nodes[0].input_a = water.nodes[0].input_b = kSubstrateInvalidNode;
    water.nodes[0].flags =
        static_cast<u32>(ESubstrateBsdfMode::SingleLayerWater)
        << SubstrateNodeFlag_BsdfModeShift;
    const FSubstrateCompileResult result = CompileSubstrateMaterial(water);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_TRUE((result.stats.feature_bits &
                 SubstrateFeature_SingleLayerWater) != 0u);
    EXPECT_EQ(result.stats.complexity, ESubstrateComplexity::ComplexSpecial);
    EXPECT_TRUE(result.stats.estimated_bytes_per_pixel > 28u);
}

ACS_TEST(SubstrateMaterial, Slab39CodecPreservesAllFields) {
    FSubstrateSlab src{};
    src.diffuse_albedo={0.1f,0.2f,0.3f}; src.f0={0.04f,0.05f,0.06f};
    src.f90={0.7f,0.8f,0.9f}; src.roughness=0.11f;
    src.second_roughness=0.72f; src.second_roughness_weight=0.31f;
    src.anisotropy=-0.45f; src.tangent={0,1,0};
    src.mean_free_path_cm={0.2f,0.4f,0.8f}; src.phase_anisotropy=0.35f;
    src.emissive={3.0f,2.0f,1.0f}; src.transmittance={0.9f,0.7f,0.5f};
    src.thickness_cm=2.25f; src.fuzz_color={0.8f,0.4f,0.2f};
    src.fuzz_amount=0.6f; src.fuzz_roughness=0.7f;
    src.thin_film_weight=0.8f; src.thin_film_thickness_nm=615.0f;
    src.thin_film_ior=1.33f; src.normal={0.2f,0.1f,0.97f};
    src.normal_strength=2.5f;
    f32 values[kSubstrateSlabScalarCount]{};
    EncodeSubstrateSlab(src, values);
    FSubstrateSlab dst{};
    EXPECT_TRUE(DecodeSubstrateSlab(values, dst));
    EXPECT_NEAR(dst.f90.z, 0.9f, 1e-6f);
    EXPECT_NEAR(dst.second_roughness_weight, 0.31f, 1e-6f);
    EXPECT_NEAR(dst.mean_free_path_cm.z, 0.8f, 1e-6f);
    EXPECT_NEAR(dst.thin_film_thickness_nm, 615.0f, 1e-5f);
    EXPECT_NEAR(dst.normal_strength, 2.5f, 1e-6f);
}

ACS_TEST(SubstrateMaterial,
         DirectSlabExpressionLinksPreserveVectorLanes) {
    FSubstrateMaterial material{};
    material.enabled = true;
    material.root = 0;
    material.node_count = 1u;
    material.nodes[0].type = ESubstrateNodeType::Slab;
    material.nodes[0].input_a =
        material.nodes[0].input_b =
            kSubstrateInvalidNode;

    FShaderExpressionGraph& graph =
        material.expression_graph;
    graph.root = 0;
    graph.node_count = 1u;
    graph.nodes[0].op =
        EShaderExpressionOp::Constant;
    graph.nodes[0].declared_type =
        EShaderExpressionValueType::Float3;
    graph.nodes[0].value =
        FShaderExpressionValue{0.2f, 0.4f, 0.6f};
    for (u32 scalar = 0u; scalar < 3u; ++scalar) {
        material.nodes[0].expressions.roots[scalar] = 0;
    }

    const FSubstrateExpressionLinkResult linked =
        CompileSubstrateExpressionLinks(material);
    EXPECT_TRUE(linked.Succeeded());
    EXPECT_EQ(linked.binding_count, 3u);
    for (u32 scalar = 0u; scalar < 3u; ++scalar) {
        EXPECT_EQ(
            SubstrateExpressionBindingTarget(
                linked.bindings[scalar]),
            scalar);
        EXPECT_EQ(
            SubstrateExpressionBindingInstruction(
                linked.bindings[scalar]),
            0u);
        EXPECT_EQ(
            SubstrateExpressionBindingComponent(
                linked.bindings[scalar]),
            scalar);
        EXPECT_NEAR(
            linked.bindings[scalar].coefficient,
            1.0f,
            1.0e-6f);
        EXPECT_NEAR(
            linked.bindings[scalar].authored_literal,
            0.8f,
            1.0e-6f);
    }
}

ACS_TEST(SubstrateMaterial,
         DynamicLinksRejectApproximateOrHostileTopology) {
    FSubstrateMaterial blended =
        TwoSlabs(
            ESubstrateNodeType::HorizontalBlend,
            0.5f);
    blended.expression_graph.root = 0;
    blended.expression_graph.node_count = 1u;
    blended.expression_graph.nodes[0].op =
        EShaderExpressionOp::Constant;
    blended.expression_graph.nodes[0].declared_type =
        EShaderExpressionValueType::Float1;
    blended.expression_graph.nodes[0].value =
        FShaderExpressionValue{0.25f};
    blended.nodes[0].expressions.roots[9] = 0;
    const FSubstrateExpressionLinkResult topology =
        CompileSubstrateExpressionLinks(blended);
    EXPECT_EQ(
        topology.error,
        ESubstrateExpressionLinkError::
            UnsupportedDynamicTopology);

    FSubstrateMaterial hostile{};
    hostile.enabled = true;
    hostile.node_count =
        static_cast<u8>(kSubstrateMaxNodes + 1u);
    const FSubstrateExpressionLinkResult bounded =
        CompileSubstrateExpressionLinks(hostile);
    EXPECT_EQ(
        bounded.error,
        ESubstrateExpressionLinkError::
            SubstrateCompileFailed);
}

ACS_TEST(SubstrateMaterial,
         DynamicLinkRejectsMissingVectorLane) {
    FSubstrateMaterial material{};
    material.enabled = true;
    material.root = 0;
    material.node_count = 1u;
    material.nodes[0].type =
        ESubstrateNodeType::Slab;
    material.expression_graph.root = 0;
    material.expression_graph.node_count = 1u;
    material.expression_graph.nodes[0].op =
        EShaderExpressionOp::Constant;
    material.expression_graph.nodes[0].declared_type =
        EShaderExpressionValueType::Float2;
    material.expression_graph.nodes[0].value =
        FShaderExpressionValue{0.1f, 0.2f};
    material.nodes[0].expressions.roots[2] = 0;

    const FSubstrateExpressionLinkResult linked =
        CompileSubstrateExpressionLinks(material);
    EXPECT_EQ(
        linked.error,
        ESubstrateExpressionLinkError::
            BindingTypeMismatch);
    EXPECT_EQ(linked.error_node, 0);
    EXPECT_EQ(linked.error_scalar, 2);
}

ACS_TEST(SubstrateMaterial, LegacyBuildingBlockAndAcsmatGraphRoundTrip) {
    FPbrParams2D legacy{};
    legacy.baseColor = FVec4{0.9f,0.3f,0.1f,1};
    legacy.metallic = 0.75f;
    legacy.roughness = 0.2f;
    legacy.clearcoat = 0.8f;
    legacy.clearcoatRoughness = 0.12f;
    FSubstrateMaterial legacy_graph = MakeLegacySubstrateMaterial(legacy);
    EXPECT_EQ(legacy_graph.node_count, 4u);
    EXPECT_EQ(legacy_graph.nodes[3].type, ESubstrateNodeType::VerticalLayer);
    EXPECT_TRUE(CompileSubstrateMaterial(legacy_graph).Succeeded());
    FSubstrateResolvedSurface legacy_surface{};
    EXPECT_TRUE(ResolveSubstrateMaterial(legacy_graph, legacy_surface, nullptr));
    EXPECT_NEAR(legacy_surface.coat_weight, 0.8f, 1e-5f);
    EXPECT_NEAR(legacy_surface.coat_roughness, 0.12f, 1e-5f);

    FMaterial2D src{};
    src.kind = EMaterialKind::Lit;
    src.substrate = TwoSlabs(ESubstrateNodeType::HorizontalBlend, 0.375f);
    src.substrate.nodes[0].slab.emissive = FVec3{3.0e30f,2.0e20f,1.0e10f};
    src.substrate.nodes[0].slab.thickness_cm = 3.0e20f;
    src.substrate.nodes[0].slab.thin_film_thickness_nm = 3.0e20f;
    src.substrate.nodes[0].slab.normal_strength = 4.0f;
    char text[64u * 1024u]{};
    const u32 bytes = WriteAcsmatText(src, text, sizeof(text));
    EXPECT_TRUE(bytes > 0u);
    EXPECT_TRUE(std::strstr(text, "substrateNodeCount 3") != nullptr);

    FMaterial2D parsed{};
    const FMaterial2DLoadResult load = TryParseAcsmatText(text, bytes, parsed);
    EXPECT_TRUE(load.Succeeded());
    EXPECT_TRUE(parsed.substrate.enabled);
    EXPECT_EQ(parsed.substrate.root, 2);
    EXPECT_EQ(parsed.substrate.nodes[2].type, ESubstrateNodeType::HorizontalBlend);
    EXPECT_NEAR(parsed.substrate.nodes[2].factor, 0.375f, 1e-6f);
    EXPECT_NEAR(parsed.substrate.nodes[0].slab.normal_strength, 4.0f, 1e-6f);
    EXPECT_NEAR(parsed.substrate.nodes[0].slab.emissive.x, 3.0e30f, 1.0e24f);
}

ACS_TEST(SubstrateMaterial, TransmissionSyncPreservesGlassTint) {
    FMaterial2D material{};
    material.substrate.enabled = true;
    material.substrate.node_count = 1;
    material.substrate.root = 0;
    material.substrate.nodes[0].type = ESubstrateNodeType::Slab;
    material.substrate.nodes[0].input_a = material.substrate.nodes[0].input_b = -1;
    FSubstrateSlab& glass = material.substrate.nodes[0].slab;
    glass.diffuse_albedo = FVec3{0,0,0};
    glass.f0 = SubstrateIorToF0(1.5f);
    glass.transmittance = FVec3{0.85f,0.45f,0.2f};
    glass.thickness_cm = 0.5f;
    EXPECT_TRUE(SyncLegacyPbrFromSubstrate(material));
    EXPECT_NEAR(material.pbr.transmission, 0.85f, 1e-5f);
    EXPECT_NEAR(material.pbr.baseColor.x, 0.85f, 1e-5f);
    EXPECT_NEAR(material.pbr.baseColor.y, 0.45f, 1e-5f);
    EXPECT_NEAR(material.pbr.baseColor.z, 0.2f, 1e-5f);
}

ACS_TEST(SubstrateMaterial, HugeDynamicKeySuffixCannotAliasNode) {
    const char* malformed =
        "ACSMAT 1\nkind pbr\nsubstrateEnabled 1\nsubstrateRoot 0\n"
        "substrateNodeCount 1\n"
        "substrateNode42949672960 0 -1 -1 1 0\n"
        "substrateSlab0 0.8 0.8 0.8 0.04 0.04 0.04 1 1 1 "
        "0.5 0.5 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0.01 "
        "1 1 1 0 0.5 0 400 1.4 0 0 1 1\n";
    FMaterial2D parsed{};
    const FMaterial2DLoadResult result =
        TryParseAcsmatText(malformed, std::strlen(malformed), parsed);
    EXPECT_FALSE(result.Succeeded());
}
