// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#include "render/PbrShader.h"
#include "render/SubstrateExpression.h"
#include "render/SubstrateMaterial.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <cmath>
#include <string>

using namespace acs;

namespace {

std::string ReadPbrShaderSource() {
    const std::filesystem::path test_file{__FILE__};
    const std::filesystem::path source_path =
        test_file.parent_path().parent_path() /
        "src" / "render" / "PbrShader.cpp";
    std::ifstream stream(source_path, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} / "src" /
            "render" / "PbrShader.cpp",
            std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

std::string ReadEditorAbiSource() {
    const std::filesystem::path test_file{__FILE__};
    const std::filesystem::path source_path =
        test_file.parent_path().parent_path() /
        "src" / "editor_abi" / "EditorAbi.cpp";
    std::ifstream stream(source_path, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} / "src" /
            "editor_abi" / "EditorAbi.cpp",
            std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

std::string ReadIblSource() {
    const std::filesystem::path test_file{__FILE__};
    const std::filesystem::path source_path =
        test_file.parent_path().parent_path() /
        "src" / "render" / "Ibl.cpp";
    std::ifstream stream(source_path, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} / "src" /
            "render" / "Ibl.cpp",
            std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

bool Contains(const std::string& text, const char* token) {
    return text.find(token) != std::string::npos;
}

std::size_t CountOccurrences(
        const std::string& text, const char* token) {
    const std::string needle{token};
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

f32 DecodeSrgbChannel(f32 value) {
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow(
            (value + 0.055f) / 1.055f,
            2.4f);
}

} // namespace

ACS_TEST(PbrExpressionContract, FlatGpuAbiRemainsBoundedAndAligned) {
    EXPECT_EQ(kShaderExpressionMaxNodes, 64u);
    EXPECT_EQ(kShaderExpressionMaxTextureSlots, 4u);
    EXPECT_EQ(kShaderExpressionMaxParameters, 32u);
    EXPECT_EQ(kSubstrateSlabScalarCount, 39u);
    EXPECT_EQ(sizeof(FShaderExpressionInstruction), 48u);
    EXPECT_EQ(alignof(FShaderExpressionInstruction), 16u);
    EXPECT_EQ(sizeof(FSubstrateExpressionBinding), 16u);
    EXPECT_EQ(alignof(FSubstrateExpressionBinding), 16u);
    EXPECT_EQ(
        static_cast<u32>(EShaderExpressionOp::Component),
        16u);

    for (u32 opcode = 0u; opcode <= 16u; ++opcode) {
        const char* name = ShaderExpressionOpName(
            static_cast<EShaderExpressionOp>(opcode));
        EXPECT_TRUE(name != nullptr);
        EXPECT_TRUE(std::string{name} != "Unknown");
    }
}

ACS_TEST(PbrExpressionContract, ShaderSourceMatchesExpressionVmContract) {
    const std::string source = ReadPbrShaderSource();
    EXPECT_TRUE(!source.empty());

    static constexpr const char* kOpcodeTokens[]{
        "ACS_EXPR_CONSTANT = 0u",
        "ACS_EXPR_SCALAR_PARAMETER = 1u",
        "ACS_EXPR_VECTOR_PARAMETER = 2u",
        "ACS_EXPR_TEXTURE_SAMPLE_2D = 3u",
        "ACS_EXPR_UV0 = 4u",
        "ACS_EXPR_TIME = 5u",
        "ACS_EXPR_WORLD_POSITION = 6u",
        "ACS_EXPR_WORLD_NORMAL = 7u",
        "ACS_EXPR_ADD = 8u",
        "ACS_EXPR_MULTIPLY = 9u",
        "ACS_EXPR_LERP = 10u",
        "ACS_EXPR_CLAMP = 11u",
        "ACS_EXPR_POWER = 12u",
        "ACS_EXPR_DOT = 13u",
        "ACS_EXPR_NORMALIZE = 14u",
        "ACS_EXPR_NOISE = 15u",
        "ACS_EXPR_COMPONENT = 16u",
    };
    for (const char* token : kOpcodeTokens) {
        EXPECT_TRUE(Contains(source, token));
    }

    EXPECT_TRUE(Contains(
        source, "substrate_expr_instructions[64 * 3]"));
    EXPECT_TRUE(Contains(source, "substrate_expr_bindings[39]"));
    EXPECT_TRUE(Contains(source, "substrate_expr_parameter_meta[32]"));
    EXPECT_TRUE(Contains(source, "substrate_expr_parameter_values[32]"));
    EXPECT_TRUE(Contains(source, "AcsEvaluateExpressions("));
    EXPECT_TRUE(Contains(source, "binding_index < 39u"));

    EXPECT_TRUE(Contains(
        source, "expression_texture0 : register(t11)"));
    EXPECT_TRUE(Contains(
        source, "expression_texture1 : register(t12)"));
    EXPECT_TRUE(Contains(
        source, "expression_texture2 : register(t13)"));
    EXPECT_TRUE(Contains(
        source, "expression_texture3 : register(t14)"));
    EXPECT_TRUE(Contains(source, "pd.texture_slots = 16"));
    EXPECT_TRUE(Contains(source, "pd.static_sampler_count = 16"));
    EXPECT_TRUE(Contains(
        source, "cmd.SetTexture(11u + slot, *texture);"));
    EXPECT_TRUE(Contains(
        source, "cmd.SetTexture(11u + slot, *m_White);"));
    EXPECT_TRUE(Contains(
        source, "m_SubstrateExpressionTextureMask = 0u;"));
    EXPECT_TRUE(Contains(
        source, "m_SubstrateExpressionTextures[slot] = nullptr;"));
    EXPECT_TRUE(Contains(
        source, "void CPbrShader::SetSubstrateExpressionTime("));
    EXPECT_TRUE(Contains(
        source, "if (!all(isfinite(value))) value = 0.0.xxxx;"));
    EXPECT_TRUE(Contains(
        source, "offsetof(FObjectCbLayout, substrate_expr_instructions)"));
    EXPECT_TRUE(Contains(
        source, "sizeof(FObjectCbLayout) % 16u == 0u"));
}

ACS_TEST(PbrExpressionContract, CloudTransmittanceOnlyModulatesPrimaryDirectLight) {
    const std::string source = ReadPbrShaderSource();
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(Contains(source, "cloud_shadow_transmittance : register(t15)"));
    EXPECT_TRUE(Contains(source, "float ComputeCloudShadowTransmittance(float3 world_p)"));
    EXPECT_TRUE(Contains(source, "float transmittance = 1.0;"));
    EXPECT_TRUE(Contains(source, "? (shadow * contact_shadow * cloud_shadow)"));
    EXPECT_TRUE(Contains(source, "cmd.SetTexture(15u, *m_CloudShadowTransmittance);"));
    EXPECT_TRUE(Contains(source, "cb.cloud_shadow_world_origin = m_CloudShadowWorldOrigin;"));
    EXPECT_FALSE(Contains(source, "base_ibl * cloud_shadow"));
    EXPECT_FALSE(Contains(source, "emissive * cloud_shadow"));
}

ACS_TEST(PbrExpressionContract,
         AlbedoAndNormalStrengthUseLinearPhysicalInputs) {
    const std::string source = ReadPbrShaderSource();
    EXPECT_TRUE(!source.empty());

    EXPECT_NEAR(
        DecodeSrgbChannel(0.5f),
        0.21404114f,
        1.0e-6f);
    EXPECT_NEAR(
        DecodeSrgbChannel(-0.5f),
        -0.5f / 12.92f,
        1.0e-6f);
    EXPECT_TRUE(Contains(
        source,
        "float3 albedo_texel_linear = AcsExprDecodeSrgb("));
    EXPECT_TRUE(Contains(
        source,
        "albedo.Sample(albedo_sampler, v.uv), 8u).rgb;"));
    EXPECT_TRUE(Contains(
        source,
        "float3 albedo_rgb = albedo_texel_linear *"));

    EXPECT_TRUE(Contains(
        source,
        "w=normal-map strength"));
    EXPECT_TRUE(Contains(
        source,
        "texture_slope *= texture_strength * substrate_strength;"));
    EXPECT_TRUE(Contains(
        source,
        "authored_slope *= substrate_strength;"));
    EXPECT_TRUE(Contains(
        source,
        "m_NormalMapStrength"));
    EXPECT_TRUE(Contains(
        source,
        "m_NormalMap != nullptr"));
}

ACS_TEST(PbrExpressionContract,
         HlslWarningSafetyContractsAreExplicit) {
    const std::string source = ReadPbrShaderSource();
    EXPECT_TRUE(!source.empty());

    EXPECT_TRUE(Contains(
        source,
        "bool projection_valid = abs(lp.w) > 1.0e-5;"));
    EXPECT_TRUE(Contains(
        source,
        "float shadow_value = 1.0;"));
    EXPECT_TRUE(Contains(
        source,
        "float3 brdf_value = float3(0, 0, 0);"));
    EXPECT_TRUE(Contains(source, "float  D = 0.0;"));
    EXPECT_TRUE(Contains(source, "float  G = 0.0;"));
    EXPECT_TRUE(Contains(
        source,
        "float4 canonical = 0.0.xxxx;"));
    EXPECT_TRUE(Contains(
        source,
        "float power_value = 1.0;"));
    EXPECT_TRUE(Contains(
        source,
        "float power_base = max((value + 0.055) / 1.055, 0.0);"));
    EXPECT_EQ(
        CountOccurrences(
            source, "uint width = 1u, height = 1u;"),
        static_cast<std::size_t>(4));
    EXPECT_TRUE(Contains(
        source,
        "registers[register_index] = 0.0.xxxx;"));
    EXPECT_TRUE(Contains(
        source,
        "float average_density = 1.0;"));
}

ACS_TEST(PbrExpressionContract, RuntimeAcceptsCompiledDirectSlabProgram) {
    FSubstrateMaterial material{};
    material.enabled = true;
    material.root = 0;
    material.node_count = 1u;
    material.nodes[0].type = ESubstrateNodeType::Slab;

    FShaderExpressionGraph& graph = material.expression_graph;
    graph.root = kShaderExpressionInvalidNode;
    graph.node_count = 1u;
    graph.nodes[0].op = EShaderExpressionOp::ScalarParameter;
    graph.nodes[0].declared_type =
        EShaderExpressionValueType::Float1;
    graph.nodes[0].parameter_id =
        ShaderExpressionParameterId("Roughness");
    graph.nodes[0].value = FShaderExpressionValue{0.35f};
    material.nodes[0].expressions.roots[9] = 0;

    CPbrShader shader{};
    EXPECT_TRUE(shader.SetSubstrateMaterial(material, 1.25f));

    FShaderExpressionParameter parameter{};
    parameter.id = ShaderExpressionParameterId("Roughness");
    parameter.type = EShaderExpressionValueType::Float1;
    parameter.value = FShaderExpressionValue{0.7f};
    shader.SetSubstrateExpressionParameters(&parameter, 1u);
    shader.SetSubstrateExpressionTime(2.5f);
    shader.SetSubstrateExpressionTime(
        std::numeric_limits<f32>::infinity());
    shader.SetSubstrateExpressionTexture(0u, nullptr);
    shader.ClearSubstrateSurface();
}

ACS_TEST(PbrExpressionContract, StaticSurfaceClearsPreviousExpressionState) {
    FSubstrateMaterial material{};
    material.enabled = true;
    material.root = 0;
    material.node_count = 1u;
    material.nodes[0].type = ESubstrateNodeType::Slab;
    material.expression_graph.node_count = 1u;
    material.expression_graph.nodes[0].op =
        EShaderExpressionOp::Constant;
    material.expression_graph.nodes[0].declared_type =
        EShaderExpressionValueType::Float1;
    material.expression_graph.nodes[0].value =
        FShaderExpressionValue{0.25f};
    material.nodes[0].expressions.roots[9] = 0;

    CPbrShader shader{};
    EXPECT_TRUE(shader.SetSubstrateMaterial(material, 2.0f));
    EXPECT_EQ(shader.SubstrateExpressionInstructionCount(), 1u);
    EXPECT_EQ(shader.SubstrateExpressionBindingCount(), 1u);

    FSubstrateResolvedSurface static_surface{};
    shader.SetSubstrateSurface(static_surface);
    EXPECT_EQ(shader.SubstrateExpressionInstructionCount(), 0u);
    EXPECT_EQ(shader.SubstrateExpressionBindingCount(), 0u);
    EXPECT_EQ(shader.SubstrateExpressionTextureMask(), 0u);
}

ACS_TEST(PbrExpressionContract, EditorPreviewUsesProductionSphereAndVm) {
    const std::string source = ReadEditorAbiSource();
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(Contains(source, "RenderPbrMaterialPreview("));
    EXPECT_TRUE(Contains(source, "EnsurePreviewPbr(h, size)"));
    EXPECT_TRUE(Contains(
        source, "shader.SetSubstrateMaterial("));
    EXPECT_TRUE(Contains(
        source, "shader.SetSubstrateExpressionTexture("));
    EXPECT_TRUE(Contains(
        source, "material.substrateExpressionTexturePaths[slot]"));
    EXPECT_TRUE(Contains(
        source, "shader.DrawMesh("));
    EXPECT_TRUE(Contains(source, "h.preview_mesh_sphere"));
    EXPECT_TRUE(Contains(
        source,
        "Primitive::MakeSphere(0.5f, 128, 64)"));
    EXPECT_TRUE(Contains(source, "h.preview_depth.Get()"));
    EXPECT_TRUE(Contains(
        source, "host->preview_pbr3d.Shutdown()"));
    EXPECT_TRUE(Contains(
        source,
        "shader.SetNormalMap(normal_texture.Get(), pbr.normalStrength);"));
    EXPECT_TRUE(Contains(
        source,
        "lit ? pbr.normalStrength : 1.0f"));
    EXPECT_TRUE(Contains(
        source,
        "if (mat.pbr.shadingMode == 1)"));
    EXPECT_TRUE(Contains(
        source,
        "game::ToLitParams(mat.pbr)"));
}

ACS_TEST(PbrExpressionContract,
         IblDirectSunUsesSymmetricRingInpaint) {
    const std::string source = ReadIblSource();
    EXPECT_TRUE(!source.empty());

    // Diffuse irradiance and specular prefilter must use the same replacement
    // for the analytic HDR sun.  A zero fill removes fireflies but leaves a
    // black hole in roughness-zero reflections.
    EXPECT_EQ(
        CountOccurrences(source, "float3 SampleIndirectEnvironment("),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(source, "radiance = 0.25 * ("),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(source, "return radiance;"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(
            source,
            "env.SampleLevel(env_sampler, direction, 0).rgb"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(source, "ring_center + tangent_offset"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(source, "ring_center - tangent_offset"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(source, "ring_center + bitangent_offset"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(source, "ring_center - bitangent_offset"),
        static_cast<std::size_t>(2));
    EXPECT_TRUE(Contains(
        source,
        "cone_cos - max(3.5e-4, 0.5 * (1.0 - cone_cos))"));
    EXPECT_TRUE(Contains(
        source,
        "return float4(SampleIndirectEnvironment(N), 1.0);"));
    EXPECT_TRUE(!Contains(
        source,
        "return float3(0.0, 0.0, 0.0);"));
}
