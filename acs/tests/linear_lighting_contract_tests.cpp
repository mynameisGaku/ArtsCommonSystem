// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Math.h"
#include "render/Ibl.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace acs;

namespace {

std::string ReadRenderSource(const char* filename) {
    const std::filesystem::path test_file{__FILE__};
    const std::filesystem::path source_path =
        test_file.parent_path().parent_path() /
        "src" / "render" / filename;
    std::ifstream stream(source_path, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} / "src" /
            "render" / filename,
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
    const std::size_t token_length =
        std::char_traits<char>::length(token);
    if (token_length == 0u) return 0u;

    std::size_t count = 0u;
    std::size_t position = 0u;
    while ((position = text.find(token, position)) != std::string::npos) {
        ++count;
        position += token_length;
    }
    return count;
}

std::string ExtractSection(
        const std::string& text,
        const char* begin_token,
        const char* end_token) {
    const std::size_t begin = text.find(begin_token);
    if (begin == std::string::npos) return {};
    const std::size_t end = text.find(end_token, begin);
    if (end == std::string::npos) return {};
    return text.substr(begin, end - begin);
}

std::string ExtractRawShader(
        const std::string& source, const char* declaration) {
    const std::size_t declaration_pos = source.find(declaration);
    if (declaration_pos == std::string::npos) return {};
    const std::size_t begin = source.find("R\"(", declaration_pos);
    if (begin == std::string::npos) return {};
    const std::size_t end = source.find(")\";", begin + 3u);
    if (end == std::string::npos) return {};
    return source.substr(begin + 3u, end - (begin + 3u));
}

f32 DecodeSrgbChannel(f32 value) {
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow(
            (value + 0.055f) / 1.055f,
            2.4f);
}

/** 放射輝度SH9を指定方向で評価する。 */
f32 EvaluateSh9RadianceForTest(const FVec4 coefficients[9], FVec3 direction) {
    const f32 x = direction.x;
    const f32 y = direction.y;
    const f32 z = direction.z;
    return coefficients[0].x * 0.282095f + coefficients[1].x * (0.488603f * y) + coefficients[2].x * (0.488603f * z) + coefficients[3].x * (0.488603f * x) + coefficients[4].x * (1.092548f * x * y) + coefficients[5].x * (1.092548f * y * z) + coefficients[6].x * (0.315392f * (3.0f * z * z - 1.0f)) + coefficients[7].x * (1.092548f * x * z) + coefficients[8].x * (0.546274f * (x * x - y * y));
}

/** 論文のSH9半球積分をACSのcubemapと同じE/πで評価する。 */
f32 EvaluateNormalizedSh9DiffuseForTest(const FVec4 coefficients[9], FVec3 normal) {
    constexpr f32 c1 = 0.429043f;
    constexpr f32 c2 = 0.511664f;
    constexpr f32 c3 = 0.743125f;
    constexpr f32 c4 = 0.886227f;
    constexpr f32 c5 = 0.247708f;
    const f32 x = normal.x;
    const f32 y = normal.y;
    const f32 z = normal.z;
    const f32 irradiance = c4 * coefficients[0].x + 2.0f * c2 * (coefficients[3].x * x + coefficients[1].x * y + coefficients[2].x * z) - c5 * coefficients[6].x + 2.0f * c1 * (coefficients[4].x * x * y + coefficients[7].x * x * z + coefficients[5].x * y * z) + c3 * coefficients[6].x * z * z + c1 * coefficients[8].x * (x * x - y * y);
    return irradiance / kPi;
}

/** GGX標本が受け持つ立体角から入力環境のmipを求める。 */
f32 EvaluateGgxEnvironmentMipForTest(f32 roughness, f32 normal_dot_half, f32 view_dot_half, u32 sample_count, f32 environment_resolution) {
    const f32 alpha = roughness * roughness;
    const f32 alpha_squared = alpha * alpha;
    const f32 denominator = normal_dot_half * normal_dot_half * (alpha_squared - 1.0f) + 1.0f;
    const f32 distribution = alpha_squared / (kPi * denominator * denominator);
    const f32 pdf = distribution * normal_dot_half / (4.0f * view_dot_half);
    const f32 sample_solid_angle = 1.0f / (static_cast<f32>(sample_count) * pdf);
    const f32 texel_solid_angle = 4.0f * kPi / (6.0f * environment_resolution * environment_resolution);
    return 0.5f * static_cast<f32>(std::log2(sample_solid_angle / texel_solid_angle));
}

} // namespace

ACS_TEST(LinearLightingContract, ExactSrgbTransferFunctionHasStableValues) {
    EXPECT_NEAR(DecodeSrgbChannel(0.0f), 0.0f, 1.0e-7f);
    EXPECT_NEAR(
        DecodeSrgbChannel(0.04045f),
        0.003130805f,
        1.0e-7f);
    EXPECT_NEAR(
        DecodeSrgbChannel(0.5f),
        0.21404114f,
        1.0e-6f);
    EXPECT_NEAR(DecodeSrgbChannel(1.0f), 1.0f, 1.0e-7f);
}

ACS_TEST(LinearLightingContract,
         RgbTransferBlendModesMatchAcrossRhisAndSpriteBatch) {
    const std::string diligent =
        ReadRenderSource("Diligent/DiligentCommon.h");
    const std::string dx12 =
        ReadRenderSource("Dx12/Dx12Pipeline.cpp");
    const std::string sprite =
        ReadRenderSource("SpriteBatch.cpp");
    EXPECT_TRUE(!diligent.empty());
    EXPECT_TRUE(!dx12.empty());
    EXPECT_TRUE(!sprite.empty());

    const std::string diligentMultiply = ExtractSection(
        diligent,
        "case EBlendMode::Multiply:",
        "case EBlendMode::AdditivePreserveAlpha:");
    const std::string diligentAdd = ExtractSection(
        diligent,
        "case EBlendMode::AdditivePreserveAlpha:",
        "inline FILTER_TYPE ToDiligentFilter");
    const std::string dx12Multiply = ExtractSection(
        dx12,
        "case EBlendMode::Multiply:",
        "case EBlendMode::AdditivePreserveAlpha:");
    const std::string dx12Add = ExtractSection(
        dx12,
        "case EBlendMode::AdditivePreserveAlpha:",
        "D3D12_COMPARISON_FUNC ToD3DCompare");
    EXPECT_TRUE(!diligentMultiply.empty());
    EXPECT_TRUE(!diligentAdd.empty());
    EXPECT_TRUE(!dx12Multiply.empty());
    EXPECT_TRUE(!dx12Add.empty());

    // Multiply is destination * source RGB while destination alpha survives.
    EXPECT_TRUE(Contains(
        diligentMultiply,
        "rt.SrcBlend       = BLEND_FACTOR_ZERO;"));
    EXPECT_TRUE(Contains(
        diligentMultiply,
        "rt.DestBlend      = BLEND_FACTOR_SRC_COLOR;"));
    EXPECT_TRUE(Contains(
        diligentMultiply,
        "rt.SrcBlendAlpha  = BLEND_FACTOR_ZERO;"));
    EXPECT_TRUE(Contains(
        diligentMultiply,
        "rt.DestBlendAlpha = BLEND_FACTOR_ONE;"));
    EXPECT_TRUE(Contains(
        dx12Multiply,
        "rt.SrcBlend = D3D12_BLEND_ZERO; "
        "rt.DestBlend = D3D12_BLEND_SRC_COLOR;"));
    EXPECT_TRUE(Contains(
        dx12Multiply,
        "rt.SrcBlendAlpha = D3D12_BLEND_ZERO; "
        "rt.DestBlendAlpha = D3D12_BLEND_ONE;"));

    // Scatter is an unweighted RGB sum and likewise preserves destination A.
    EXPECT_TRUE(Contains(
        diligentAdd,
        "rt.SrcBlend       = BLEND_FACTOR_ONE;"));
    EXPECT_TRUE(Contains(
        diligentAdd,
        "rt.DestBlend      = BLEND_FACTOR_ONE;"));
    EXPECT_TRUE(Contains(
        diligentAdd,
        "rt.SrcBlendAlpha  = BLEND_FACTOR_ZERO;"));
    EXPECT_TRUE(Contains(
        diligentAdd,
        "rt.DestBlendAlpha = BLEND_FACTOR_ONE;"));
    EXPECT_TRUE(Contains(
        dx12Add,
        "rt.SrcBlend = D3D12_BLEND_ONE; "
        "rt.DestBlend = D3D12_BLEND_ONE;"));
    EXPECT_TRUE(Contains(
        dx12Add,
        "rt.SrcBlendAlpha = D3D12_BLEND_ZERO; "
        "rt.DestBlendAlpha = D3D12_BLEND_ONE;"));
    EXPECT_TRUE(!Contains(
        diligentAdd, "BLEND_FACTOR_SRC_ALPHA"));
    EXPECT_TRUE(!Contains(
        dx12Add, "D3D12_BLEND_SRC_ALPHA"));

    // Public SpriteBatch blend selection must not silently map the new modes
    // back to its normal AlphaBlend pipeline.
    const std::string setBlend = ExtractSection(
        sprite,
        "void CSpriteBatch::SetBlendMode",
        "void CSpriteBatch::SetStencilMode");
    EXPECT_TRUE(Contains(
        sprite, "bool CSpriteBatch::EnsureOpaquePipeline()"));
    EXPECT_TRUE(Contains(
        sprite, "bool CSpriteBatch::EnsureMultiplyPipeline()"));
    EXPECT_TRUE(Contains(
        sprite,
        "bool CSpriteBatch::EnsureAdditivePreserveAlphaPipeline()"));
    EXPECT_TRUE(Contains(
        sprite, "pd.blend_mode = EBlendMode::Opaque;"));
    EXPECT_TRUE(Contains(
        sprite, "pd.blend_mode = EBlendMode::Multiply;"));
    EXPECT_TRUE(Contains(
        sprite,
        "pd.blend_mode = EBlendMode::AdditivePreserveAlpha;"));
    EXPECT_TRUE(Contains(
        setBlend, "case EBlendMode::Opaque:"));
    EXPECT_TRUE(Contains(
        setBlend, "if (!EnsureOpaquePipeline()) return;"));
    EXPECT_TRUE(Contains(
        setBlend, "pl = m_OpaquePipe.Get();"));
    EXPECT_TRUE(Contains(
        setBlend, "case EBlendMode::Multiply:"));
    EXPECT_TRUE(Contains(
        setBlend, "pl = m_MultiplyPipe.Get();"));
    EXPECT_TRUE(Contains(
        setBlend, "case EBlendMode::AdditivePreserveAlpha:"));
    EXPECT_TRUE(Contains(
        setBlend, "pl = m_AdditivePreserveAlphaPipe.Get();"));
    EXPECT_TRUE(Contains(sprite, "m_OpaquePipe.Reset();"));
    EXPECT_TRUE(Contains(sprite, "m_MultiplyPipe.Reset();"));
    EXPECT_TRUE(Contains(
        sprite, "m_AdditivePreserveAlphaPipe.Reset();"));
}

ACS_TEST(LinearLightingContract,
         StandardAndSkinnedDecodeOnlySampledAlbedo) {
    const std::string standard =
        ReadRenderSource("StandardShader.cpp");
    const std::string skinned =
        ReadRenderSource("SkinnedShader.cpp");
    EXPECT_TRUE(!standard.empty());
    EXPECT_TRUE(!skinned.empty());

    EXPECT_TRUE(Contains(
        standard,
        "float AcsStandardSrgbToLinearChannel(float value)"));
    EXPECT_TRUE(Contains(
        standard,
        "float3 albedo_texel_linear = AcsStandardDecodeAlbedo("));
    EXPECT_TRUE(Contains(
        standard,
        "albedo.Sample(albedo_sampler, v.uv).rgb);"));
    EXPECT_TRUE(Contains(
        standard,
        "float3 albedo_rgb = albedo_texel_linear * base_color.xyz;"));

    EXPECT_TRUE(Contains(
        skinned,
        "float AcsSkinnedSrgbToLinearChannel(float value)"));
    EXPECT_TRUE(Contains(
        skinned,
        "float3 albedo_texel_linear = AcsSkinnedDecodeAlbedo("));
    EXPECT_TRUE(Contains(
        skinned,
        "albedo.Sample(albedo_sampler, v.uv).rgb);"));
    EXPECT_TRUE(Contains(
        skinned,
        "float3 albedo_rgb = albedo_texel_linear * base_color.xyz;"));

    EXPECT_TRUE(!Contains(
        standard,
        "AcsStandardDecodeAlbedo(base_color"));
    EXPECT_TRUE(!Contains(
        skinned,
        "AcsSkinnedDecodeAlbedo(base_color"));
}

ACS_TEST(LinearLightingContract,
         LegacyShadersKeepFractionalPowBasesNonNegative) {
    const std::string standard_source =
        ReadRenderSource("StandardShader.cpp");
    const std::string skinned_source =
        ReadRenderSource("SkinnedShader.cpp");
    const std::string water_source =
        ReadRenderSource("WaterSurface3D.cpp");
    const std::string standard =
        ExtractRawShader(standard_source, "const char* kStandardHLSL");
    const std::string skinned =
        ExtractRawShader(skinned_source, "const char* kSkinnedHLSL");
    const std::string water =
        ExtractRawShader(water_source, "const char* kWaterSurface3DHlsl");
    EXPECT_TRUE(!standard.empty());
    EXPECT_TRUE(!skinned.empty());
    EXPECT_TRUE(!water.empty());

    EXPECT_TRUE(Contains(standard, "float safe_value = saturate(value);"));
    EXPECT_TRUE(Contains(
        standard,
        "pow(abs((safe_value + 0.055) / 1.055), 2.4)"));
    EXPECT_TRUE(Contains(
        standard,
        "float dir_spec_base = abs(saturate(dot(N, H)));"));
    EXPECT_TRUE(Contains(
        standard,
        "float point_spec_base = abs(saturate(dot(N, H)));"));

    EXPECT_TRUE(Contains(skinned, "float safe_value = saturate(value);"));
    EXPECT_TRUE(Contains(
        skinned,
        "pow(abs((safe_value + 0.055) / 1.055), 2.4)"));
    EXPECT_TRUE(Contains(
        skinned,
        "float dir_spec_base = abs(saturate(dot(N, H)));"));
    EXPECT_TRUE(Contains(
        skinned,
        "float point_spec_base = abs(saturate(dot(N, H)));"));

    EXPECT_TRUE(Contains(
        water,
        "ProjectWorldDirectionToScreenPixels(input.world_position, normal)"));
    EXPECT_TRUE(Contains(
        water,
        "ProjectWorldDirectionToScreenPixels(\n"
        "            input.world_position, refracted_direction)"));
    EXPECT_TRUE(Contains(
        water,
        "abs(dot(refracted_direction,\n"
        "                                    geometric_surface_normal))"));
    EXPECT_TRUE(Contains(
        water,
        "saturate(abs(environment_height)), 0.72"));
    EXPECT_TRUE(Contains(
        water,
        "lerp(environment_horizon.rgb,\n"
        "               environment_ground.rgb, environment_weight)"));
    EXPECT_TRUE(Contains(
        water,
        "pow(abs(sun_alignment), sun_disk_power)"));
    EXPECT_TRUE(!Contains(
        water,
        "float3 sky_zenith = float3("));
    EXPECT_TRUE(!Contains(
        water,
        "normal.xz / max(abs(normal.y)"));
    EXPECT_TRUE(!Contains(
        water,
        "abs(refracted_direction.y)"));
}

ACS_TEST(LinearLightingContract,
         ShadowHelpersUseInitializedSingleExitAndUniqueLoops) {
    const std::string standard_source =
        ReadRenderSource("StandardShader.cpp");
    const std::string water_source =
        ReadRenderSource("WaterSurface3D.cpp");
    const std::string skinned_source =
        ReadRenderSource("SkinnedShader.cpp");
    const std::string standard =
        ExtractRawShader(standard_source, "const char* kStandardHLSL");
    const std::string water =
        ExtractRawShader(water_source, "const char* kWaterSurface3DHlsl");
    const std::string skinned =
        ExtractRawShader(skinned_source, "const char* kSkinnedHLSL");
    const std::string standard_shadow = ExtractSection(
        standard, "float ComputeShadow(", "float4 PSMain(");
    const std::string water_shadow_sample = ExtractSection(water, "float SampleSunShadowCascade(", "float ComputeSunShadow(");
    const std::string water_shadow = ExtractSection(
        water, "float ComputeSunShadow(", "float4 PSMain(");
    EXPECT_TRUE(!standard_shadow.empty());
    EXPECT_TRUE(!water_shadow_sample.empty());
    EXPECT_TRUE(!water_shadow.empty());

    EXPECT_TRUE(Contains(
        standard_shadow, "float shadow_result = 1.0;"));
    EXPECT_EQ(
        CountOccurrences(standard_shadow, "return "),
        static_cast<std::size_t>(1u));
    EXPECT_TRUE(Contains(
        standard_shadow, "return shadow_result;"));
    EXPECT_TRUE(Contains(
        standard_shadow, "light_clip.w > 1e-5"));
    EXPECT_TRUE(Contains(
        standard_shadow, "max(abs(shadow_params.z), 1e-7)"));

    EXPECT_TRUE(Contains(
        water_shadow, "float shadow_result = 1.0;"));
    EXPECT_EQ(
        CountOccurrences(water_shadow, "return "),
        static_cast<std::size_t>(1u));
    EXPECT_TRUE(Contains(
        water_shadow, "return shadow_result;"));
    EXPECT_TRUE(Contains(water_shadow_sample, "light_clip.w > 1e-5"));
    EXPECT_TRUE(Contains(water_shadow_sample, "max(abs(shadow_params.z), 1e-7)"));
    EXPECT_TRUE(Contains(water_shadow_sample, "clamp(shadow_uv + offset, min_uv, max_uv)"));
    EXPECT_TRUE(Contains(water_shadow, "shadow_cascade_splits.x"));

    EXPECT_TRUE(Contains(standard, "blocker_sample_index"));
    EXPECT_TRUE(Contains(standard, "pcf_sample_index"));
    EXPECT_TRUE(Contains(standard, "dir_light_index"));
    EXPECT_TRUE(Contains(standard, "point_light_index"));
    EXPECT_TRUE(!Contains(standard, "for (int i ="));
    EXPECT_TRUE(!Contains(standard, "for (int j ="));
    EXPECT_TRUE(Contains(skinned, "dir_light_index"));
    EXPECT_TRUE(Contains(skinned, "point_light_index"));
    EXPECT_TRUE(!Contains(skinned, "for (int i ="));
    EXPECT_TRUE(!Contains(skinned, "for (int j ="));
}

ACS_TEST(LinearLightingContract,
         IblDirectDiscUsesNonBlackSymmetricInpaint) {
    const std::string source = ReadRenderSource("Ibl.cpp");
    EXPECT_TRUE(!source.empty());

    const std::string irradiance =
        ExtractRawShader(source, "const char* kIrradianceHLSL");
    const std::string environment_mip =
        ExtractRawShader(source, "const char* kEnvironmentMipHLSL");
    const std::string prefilter =
        ExtractRawShader(source, "const char* kPrefilterHLSL");
    EXPECT_TRUE(!irradiance.empty());
    EXPECT_TRUE(!environment_mip.empty());
    EXPECT_TRUE(!prefilter.empty());

    for (const std::string* shader : {&irradiance, &environment_mip}) {
        EXPECT_TRUE(Contains(*shader, shader == &irradiance ? "float3 SampleIndirectEnvironment(float3 direction)" : "float3 SampleSourceEnvironment(float3 direction)"));
        EXPECT_TRUE(Contains(*shader, "ring_center + tangent_offset"));
        EXPECT_TRUE(Contains(*shader, "ring_center - tangent_offset"));
        EXPECT_TRUE(Contains(*shader, "ring_center + bitangent_offset"));
        EXPECT_TRUE(Contains(*shader, "ring_center - bitangent_offset"));
        EXPECT_TRUE(!Contains(
            *shader,
            "return float3(0.0, 0.0, 0.0);"));
    }

    // 鏡面mipも直接光を補間済みの環境mip 0を使い、HDR太陽や黒い穴を戻さない。
    EXPECT_TRUE(Contains(prefilter, "if (r < 1.0e-3)"));
    EXPECT_TRUE(Contains(prefilter, "return float4(SampleFilteredEnvironment(N, 0.0), 1.0);"));
    EXPECT_TRUE(Contains(source, "data.exclude_direct_light = 1.0f;"));
    EXPECT_TRUE(Contains(source, "cl.SetTexture(0u, environment);"));
}

ACS_TEST(LinearLightingContract,
         IblHelpersUseInitializedSingleExitControlFlow) {
    const std::string source = ReadRenderSource("Ibl.cpp");
    const std::string capture =
        ExtractRawShader(source, "const char* kEnvCaptureHLSL");
    const std::string irradiance =
        ExtractRawShader(source, "const char* kIrradianceHLSL");
    const std::string environment_mip =
        ExtractRawShader(source, "const char* kEnvironmentMipHLSL");
    const std::string prefilter =
        ExtractRawShader(source, "const char* kPrefilterHLSL");
    const std::string equirect =
        ExtractRawShader(source, "const char* kEquirectToCubeHLSL");
    EXPECT_TRUE(!source.empty());

    for (const std::string* shader : {&capture, &irradiance, &environment_mip, &prefilter, &equirect}) {
        EXPECT_TRUE(!shader->empty());
        EXPECT_EQ(
            CountOccurrences(
                *shader,
                "float3 direction = float3(-m.x, -m.y, -1.0);"),
            static_cast<std::size_t>(1u));
        EXPECT_EQ(
            CountOccurrences(*shader, "return direction;"),
            static_cast<std::size_t>(1u));
    }

    const std::string irradiance_sample = ExtractSection(irradiance, "float3 SampleIndirectEnvironment(float3 direction)", "float3 IntegrateDiffuse(float3 N)");
    const std::string environment_mip_sample = ExtractSection(environment_mip, "float3 SampleSourceEnvironment(float3 direction)", "float4 PSMain(VSOut v)");
    for (const std::string* sample : {&irradiance_sample, &environment_mip_sample}) {
        EXPECT_TRUE(!sample->empty());
        EXPECT_TRUE(Contains(
            *sample,
            "float3 radiance = float3(0.0, 0.0, 0.0);"));
        EXPECT_TRUE(Contains(*sample, sample == &irradiance_sample ? "radiance = 0.25 * (" : "radiance = source_env.SampleLevel"));
        EXPECT_TRUE(Contains(*sample, "} else {"));
        EXPECT_TRUE(Contains(*sample, "SampleLevel("));
        EXPECT_EQ(
            CountOccurrences(*sample, "return "),
            static_cast<std::size_t>(1u));
        EXPECT_TRUE(Contains(*sample, "return radiance;"));
    }
}

ACS_TEST(LinearLightingContract, EquirectYawUsesTheSamePositiveRotationAsTrueHdri) {
    const std::string source = ReadRenderSource("Ibl.cpp");
    const std::string equirect =
        ExtractRawShader(source, "const char* kEquirectToCubeHLSL");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!equirect.empty());

    // 正の画像回転では、出力方向から逆回転した元画像の経度を読む。
    // これにより元の+Z方向の画素は+90度で+X方向へ移る。
    EXPECT_TRUE(Contains(equirect, "float sample_phi = phi - environment_rotation.x;"));
    EXPECT_TRUE(Contains(equirect, "uv.x = frac(sample_phi / TWO_PI + 0.5);"));
    EXPECT_TRUE(Contains(source, "ToRadians(Mod(environment_yaw_degrees, 360.0f))"));
    EXPECT_TRUE(Contains(source, "return LoadEquirectHdrFromMemory(device, cl, rgba_float, width, height, 0.0f);"));
}

ACS_TEST(LinearLightingContract, EquirectReplacementPublishesOnlyAfterCandidateRendering) {
    const std::string source = ReadRenderSource("Ibl.cpp");
    const std::string load = ExtractSection(source, "TResult<void> CImageBasedLighting::LoadEquirectHdrFromMemory(IRhiDevice& device", "void CImageBasedLighting::SetDirectLightExclusion(");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!load.empty());

    const std::size_t candidate_create = load.find("TUniquePtr<IRhiTexture> environment_candidate;");
    const std::size_t candidate_end = load.find("cl.EndRenderToTexture(*environment_candidate);");
    const std::size_t reset = load.find("ResetEnvCubemap();", candidate_end);
    const std::size_t publish = load.find("m_EnvCube = Move(environment_candidate);", reset);
    const std::size_t ready = load.find("m_bEnvBuilt = true;", publish);
    EXPECT_TRUE(candidate_create != std::string::npos);
    EXPECT_TRUE(candidate_end != std::string::npos);
    EXPECT_TRUE(reset != std::string::npos);
    EXPECT_TRUE(publish != std::string::npos);
    EXPECT_TRUE(ready != std::string::npos);
    EXPECT_TRUE(candidate_create < candidate_end);
    EXPECT_TRUE(candidate_end < reset);
    EXPECT_TRUE(reset < publish);
    EXPECT_TRUE(publish < ready);
    EXPECT_EQ(CountOccurrences(load, "ResetEnvCubemap();"), static_cast<std::size_t>(1u));
    EXPECT_FALSE(Contains(load, "BeginRenderToTextureSlice(*m_EnvCube"));
}

ACS_TEST(LinearLightingContract, Sh9ProjectionMatchesYUpRadianceAndNormalizedDiffuse) {
    constexpr u32 width = 256u;
    constexpr u32 height = 128u;
    TArray<f32> panorama;
    const bool allocated = panorama.TrySetNum(static_cast<usize>(width) * height * 4u);
    EXPECT_TRUE(allocated);
    if (!allocated) return;

    for (u32 y = 0u; y < height; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) * kPi / static_cast<f32>(height);
        const f32 radiance = 1.0f + 0.5f * Cos(theta);
        for (u32 x = 0u; x < width; ++x) {
            const usize index = (static_cast<usize>(y) * width + x) * 4u;
            panorama[index + 0u] = radiance;
            panorama[index + 1u] = radiance;
            panorama[index + 2u] = radiance;
            panorama[index + 3u] = 1.0f;
        }
    }

    FVec4 coefficients[9]{};
    CImageBasedLighting::ComputeSh9FromEquirect(panorama.GetData(), width, height, coefficients);
    EXPECT_NEAR(EvaluateSh9RadianceForTest(coefficients, FVec3{0.0f, 1.0f, 0.0f}), 1.5f, 2.0e-3f);
    EXPECT_NEAR(EvaluateSh9RadianceForTest(coefficients, FVec3{0.0f, -1.0f, 0.0f}), 0.5f, 2.0e-3f);
    EXPECT_NEAR(EvaluateSh9RadianceForTest(coefficients, FVec3{0.0f, 0.0f, 1.0f}), 1.0f, 2.0e-3f);
    EXPECT_NEAR(EvaluateNormalizedSh9DiffuseForTest(coefficients, FVec3{0.0f, 1.0f, 0.0f}), 4.0f / 3.0f, 2.0e-3f);
    EXPECT_NEAR(EvaluateNormalizedSh9DiffuseForTest(coefficients, FVec3{0.0f, -1.0f, 0.0f}), 2.0f / 3.0f, 2.0e-3f);
    EXPECT_NEAR(EvaluateNormalizedSh9DiffuseForTest(coefficients, FVec3{0.0f, 0.0f, 1.0f}), 1.0f, 2.0e-3f);
    CImageBasedLighting::ComputeSh9FromEquirect(panorama.GetData(), width, height, nullptr);
}

ACS_TEST(LinearLightingContract, PbrSh9UsesTheSameNormalizedIrradianceAsCubemap) {
    const std::string source = ReadRenderSource("PbrShader.cpp");
    const std::string sh9 = ExtractSection(source, "float3 Sh9Irradiance(float3 N, float4 L[9])", "float3 Sh9Radiance(float3 d, float4 L[9])");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!sh9.empty());
    EXPECT_TRUE(Contains(sh9, "float3 normalized_irradiance = e * (1.0 / PI);"));
    EXPECT_TRUE(Contains(sh9, "return max(normalized_irradiance, float3(0, 0, 0));"));
    EXPECT_FALSE(Contains(sh9, "return max(e, float3(0, 0, 0));"));
}

ACS_TEST(LinearLightingContract,
         IblPrefilterAllocatesSamplesByGgxLobeWidth) {
    const std::string source = ReadRenderSource("Ibl.cpp");
    const std::string prefilter =
        ExtractRawShader(source, "const char* kPrefilterHLSL");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!prefilter.empty());

    EXPECT_TRUE(Contains(
        prefilter,
        "const uint kSamples = max(\n"
        "        128u, (uint)(128.0 + saturate(roughness) * 896.0 + 0.5));"));
    EXPECT_TRUE(Contains(
        source, "constexpr u32 kPrefilterSize  = 512;"));
    EXPECT_TRUE(Contains(
        source, "constexpr u32 kPrefilterMips  = 7;"));

    u32 previous = 0u;
    for (u32 mip = 1u; mip < 7u; ++mip) {
        const f32 roughness = static_cast<f32>(mip) / 6.0f;
        const u32 samples =
            static_cast<u32>(128.0f + roughness * 896.0f + 0.5f);
        EXPECT_TRUE(samples >= previous);
        EXPECT_TRUE(samples >= 128u);
        EXPECT_TRUE(samples <= 1024u);
        previous = samples;
    }
    EXPECT_EQ(previous, 1024u);
}

ACS_TEST(LinearLightingContract, IblPrefilterFiltersEachGgxSampleAtItsSolidAngle) {
    const std::string source = ReadRenderSource("Ibl.cpp");
    const std::string prefilter = ExtractRawShader(source, "const char* kPrefilterHLSL");
    const std::string environment_mip = ExtractRawShader(source, "const char* kEnvironmentMipHLSL");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!prefilter.empty());
    EXPECT_TRUE(!environment_mip.empty());

    EXPECT_TRUE(Contains(prefilter, "float pdf = DistributionGGX(NoH, roughness) * NoH"));
    EXPECT_TRUE(Contains(prefilter, "float sample_solid_angle = 1.0"));
    EXPECT_TRUE(Contains(prefilter, "float texel_solid_angle = 4.0 * PI"));
    EXPECT_TRUE(Contains(prefilter, "0.5 * log2(sample_solid_angle / texel_solid_angle)"));
    EXPECT_TRUE(Contains(prefilter, "SampleFilteredEnvironment(L, source_mip)"));
    EXPECT_FALSE(Contains(prefilter, "SampleIndirectEnvironment(L)"));
    EXPECT_TRUE(Contains(source, "pyramid_desc.mip_levels = mip_count;"));
    EXPECT_TRUE(Contains(source, "cl.SetTexture(0u, *pyramid_candidate);"));
    EXPECT_TRUE(Contains(source, "cl.SetTexture(0u, *scratch);"));
    EXPECT_TRUE(Contains(environment_mip, "TextureCube source_env"));

    const f32 full_roughness_mip = EvaluateGgxEnvironmentMipForTest(1.0f, 1.0f, 1.0f, 1024u, 1024.0f);
    const f32 twice_sampled_mip = EvaluateGgxEnvironmentMipForTest(1.0f, 1.0f, 1.0f, 2048u, 1024.0f);
    const f32 half_resolution_mip = EvaluateGgxEnvironmentMipForTest(1.0f, 1.0f, 1.0f, 1024u, 512.0f);
    EXPECT_NEAR(full_roughness_mip, 6.292481f, 1.0e-5f);
    EXPECT_NEAR(full_roughness_mip - twice_sampled_mip, 0.5f, 1.0e-5f);
    EXPECT_NEAR(full_roughness_mip - half_resolution_mip, 1.0f, 1.0e-5f);
}

ACS_TEST(LinearLightingContract, SkyboxUsesCameraRelativeRaysAtFarWorldCoordinates) {
    const std::string source = ReadRenderSource("Ibl.cpp");
    const std::string skybox = ExtractRawShader(source, "const char* kSkyboxHLSL");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!skybox.empty());

    // 環境空は巨大なワールド位置を復元せず、無限遠の同次座標も視線として扱う。
    EXPECT_TRUE(Contains(skybox, "float4x4 camera_relative_inv_view_proj;"));
    EXPECT_TRUE(Contains(skybox, "float3 CameraRelativeViewDirection(float2 ndc)"));
    EXPECT_TRUE(Contains(skybox, "float3 dir=CameraRelativeViewDirection(float2(v.ndc.x,-v.ndc.y));"));
    EXPECT_FALSE(Contains(skybox, "farHomogeneous.xyz/farHomogeneous.w"));
    EXPECT_FALSE(Contains(skybox, "wp.xyz - eye.xyz"));
    EXPECT_FALSE(Contains(skybox, "float4   eye;"));

    // 旧入口を互換アダプターとして残し、高精度入口は不正行列を描画しない。
    EXPECT_TRUE(Contains(source, "void CImageBasedLighting::DrawSkyboxCameraRelative("));
    EXPECT_TRUE(Contains(source, "void CImageBasedLighting::DrawEnvSkyboxCameraRelative("));
    EXPECT_TRUE(Contains(source, "Inverse(view_proj) * FMat4::Translation("));
    EXPECT_TRUE(Contains(source, "if (!IsFiniteSkyboxMatrix_Internal(camera_relative_inv_view_proj)) return;"));
    EXPECT_TRUE(Contains(source, "cb.camera_relative_inv_view_proj = camera_relative_inv_view_proj;"));
}

ACS_TEST(LinearLightingContract,
         PhysicalSunDiscIsAnalyticCircularAndDisplayResolution) {
    const std::string source = ReadRenderSource("Ibl.cpp");
    const std::string skybox =
        ExtractRawShader(source, "const char* kSkyboxHLSL");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!skybox.empty());

    // The environment sample remains the low-frequency atmosphere.  The
    // direct solar emitter is reconstructed from the per-pixel world ray,
    // rather than magnifying a rectangular group of equirect texels.
    EXPECT_TRUE(Contains(skybox, "float4   sun_dir_radius;"));
    EXPECT_TRUE(Contains(skybox, "float4   sun_radiance;"));
    EXPECT_TRUE(Contains(
        skybox, "float sun_distance = length(dir - sun_dir);"));
    EXPECT_TRUE(Contains(
        skybox, "float sun_radius = 2.0 * sin("));

    // Screen derivatives provide resolution-independent edge coverage, while
    // limb darkening and the horizon mask avoid a flat emissive sticker.
    EXPECT_TRUE(Contains(
        skybox, "float edge_width = max(fwidth(sun_distance)"));
    EXPECT_TRUE(Contains(skybox, "float limb_mu ="));
    EXPECT_TRUE(Contains(skybox, "float horizon_visibility ="));
    EXPECT_TRUE(Contains(
        skybox, "sky += sun_radiance.rgb *"));
}
