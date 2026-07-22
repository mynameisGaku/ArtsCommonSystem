// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#include "math/Mat.h"
#include "math/Vec.h"
#include "memory/MemorySystem.h"
#include "render/IRhiDevice.h"
#include "render/HiZ.h"
#include "render/MotionVector.h"
#include "render/NormalMatrix.h"
#include "render/PbrShader.h"
#include "render/PostProcess.h"
#include "render/RefractionShader.h"
#include "render/Ssao.h"
#include "render/Ssgi.h"
#include "render/Ssr.h"
#include "render/Sky.h"
#include "render/SkinnedShader.h"
#include "render/StandardShader.h"
#include "render/WaterSurface3D.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace acs;

namespace {

std::string ReadWorkspaceSource(const char* relative_path)
{
    const std::filesystem::path test_file{__FILE__};
    const std::filesystem::path source_path =
        test_file.parent_path().parent_path() /
        std::filesystem::path{relative_path};
    std::ifstream stream(source_path, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} /
                std::filesystem::path{relative_path},
            std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

std::string ExtractRawShader(const std::string& source,
                             const char* declaration)
{
    const std::size_t declaration_begin = source.find(declaration);
    if (declaration_begin == std::string::npos) return {};
    const std::size_t raw_begin = source.find("R\"(", declaration_begin);
    if (raw_begin == std::string::npos) return {};
    const std::size_t content_begin = raw_begin + 3;
    const std::size_t content_end = source.find(")\";", content_begin);
    if (content_end == std::string::npos) return {};
    return source.substr(content_begin, content_end - content_begin);
}

std::string ExtractFunction(const std::string& shader,
                            const char* signature)
{
    const std::size_t signature_begin = shader.find(signature);
    if (signature_begin == std::string::npos) return {};
    const std::size_t body_begin = shader.find('{', signature_begin);
    if (body_begin == std::string::npos) return {};

    u32 depth = 0;
    for (std::size_t i = body_begin; i < shader.size(); ++i) {
        if (shader[i] == '{') {
            ++depth;
        } else if (shader[i] == '}') {
            if (depth == 0) return {};
            --depth;
            if (depth == 0)
                return shader.substr(signature_begin, i - signature_begin + 1);
        }
    }
    return {};
}

std::size_t CountOccurrences(const std::string& text, const char* needle)
{
    const std::string token{needle};
    if (token.empty()) return 0;
    std::size_t count = 0;
    std::size_t cursor = 0;
    while ((cursor = text.find(token, cursor)) != std::string::npos) {
        ++count;
        cursor += token.size();
    }
    return count;
}

std::string ReadDrawScene3DSource()
{
    const std::string source =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::size_t begin =
        source.find("void DrawScene3D(FEditorHost& h");
    if (begin == std::string::npos) return {};
    const std::size_t end =
        source.find("\n}\n\n} // namespace", begin);
    if (end == std::string::npos) return {};
    return source.substr(begin, end - begin);
}

} // namespace

ACS_TEST(PostEffects, FixedTapGatherShadersKeepRolledQualityLoops)
{
    const std::string source =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string dof = ExtractRawShader(source, "kDof3DHLSL");
    const std::string motion =
        ExtractRawShader(source, "kMotionBlur3DHLSL");

    EXPECT_TRUE(!dof.empty());
    EXPECT_TRUE(!motion.empty());
    if (dof.empty() || motion.empty()) return;

    // Rolling is a compile-time choice only: the quality budgets and ordered
    // sample equations remain explicit so startup optimization cannot silently
    // reduce either effect's taps.
    EXPECT_TRUE(dof.find("const int N = 32;") != std::string::npos);
    EXPECT_TRUE(dof.find("[loop] for (int i = 0; i < N; ++i)") !=
                std::string::npos);
    EXPECT_TRUE(dof.find("sqrt((float(i) + 0.5) / float(N)) * radiusPx") !=
                std::string::npos);
    EXPECT_TRUE(dof.find("[unroll] for (int i = 0; i < N; ++i)") ==
                std::string::npos);

    EXPECT_TRUE(motion.find("const int N = 17;") != std::string::npos);
    EXPECT_TRUE(motion.find("[loop] for (int i = 0; i < N; ++i)") !=
                std::string::npos);
    EXPECT_TRUE(motion.find("float t = float(i) / float(N - 1) - 0.5") !=
                std::string::npos);
    EXPECT_TRUE(motion.find("[unroll] for (int i = 0; i < N; ++i)") ==
                std::string::npos);
}

ACS_TEST(PostEffects, EditorCompositeOrderKeepsCloudsInRefractionBackground)
{
    const std::string draw = ReadDrawScene3DSource();
    EXPECT_TRUE(!draw.empty());
    if (draw.empty()) return;

    const std::size_t opaque_done =
        draw.find("// Phase2: HDR RT → FPostProcess");
    const std::size_t motion_blur =
        draw.find("// --- モーションブラー", opaque_done);
    const std::size_t aerial_perspective =
        draw.find("h.sky_atmo.CompositeAerialPerspective", motion_blur);
    const std::size_t clouds =
        draw.find("h.vclouds3d.Composite", aerial_perspective);
    const std::size_t cloud_atmosphere =
        draw.find("apVol, apTransVol, kFogVolumeMaxDist", clouds);
    const std::size_t local_fog =
        draw.find("h.sky_atmo.CompositeLocalFog", cloud_atmosphere);
    const std::size_t fog_only_argument =
        draw.find("*localFogVol", local_fog);
    const std::size_t fog_short_range =
        draw.find("h.sky_atmo.LocalFogMaxDistance()", fog_only_argument);
    const std::size_t refraction_stage =
        draw.find("// --- 屈折 (ガラス/水): transmission>0", local_fog);
    const std::size_t background_capture =
        draw.find("cl->SetTexture(0, *hdrRt)", refraction_stage);
    const std::size_t transparent_draw =
        draw.find("h.refr3d.DrawMesh", background_capture);
    const std::size_t depth_of_field =
        draw.find("// --- 被写界深度", transparent_draw);
    const std::size_t editor_overlay =
        draw.find("DrawGizmo3DOverlay", depth_of_field);
    const std::size_t post_process =
        draw.find("h.post3d.Render", editor_overlay);

    EXPECT_TRUE(opaque_done != std::string::npos);
    EXPECT_TRUE(motion_blur != std::string::npos);
    EXPECT_TRUE(aerial_perspective != std::string::npos);
    EXPECT_TRUE(local_fog != std::string::npos);
    EXPECT_TRUE(fog_only_argument != std::string::npos);
    EXPECT_TRUE(fog_short_range != std::string::npos);
    EXPECT_TRUE(clouds != std::string::npos);
    EXPECT_TRUE(cloud_atmosphere != std::string::npos);
    EXPECT_TRUE(refraction_stage != std::string::npos);
    EXPECT_TRUE(background_capture != std::string::npos);
    EXPECT_TRUE(transparent_draw != std::string::npos);
    EXPECT_TRUE(depth_of_field != std::string::npos);
    EXPECT_TRUE(editor_overlay != std::string::npos);
    EXPECT_TRUE(post_process != std::string::npos);

    EXPECT_TRUE(opaque_done < motion_blur);
    EXPECT_TRUE(motion_blur < aerial_perspective);
    EXPECT_TRUE(aerial_perspective < clouds);
    EXPECT_TRUE(clouds < cloud_atmosphere);
    EXPECT_TRUE(cloud_atmosphere < local_fog);
    EXPECT_TRUE(local_fog < fog_only_argument);
    EXPECT_TRUE(fog_only_argument < fog_short_range);
    EXPECT_TRUE(fog_short_range < refraction_stage);
    EXPECT_TRUE(cloud_atmosphere < refraction_stage);
    EXPECT_TRUE(clouds < refraction_stage);
    EXPECT_TRUE(refraction_stage < background_capture);
    EXPECT_TRUE(background_capture < transparent_draw);
    EXPECT_TRUE(transparent_draw < depth_of_field);
    EXPECT_TRUE(depth_of_field < editor_overlay);
    EXPECT_TRUE(editor_overlay < post_process);

    // AP-off + local-fog-on must not leak Rayleigh/Mie into the long-range
    // atmosphere volume. Fog is a short-range pass after clouds, so both
    // geometry and cloud/sky pixels receive it exactly once.
    const std::size_t ap_scale_gate =
        draw.find("h.q_ap_on ? 0.001f : 0.0f");
    const std::size_t stable_ap_matrix =
        draw.find("*adev, *cl, Inverse(vp_nojit), eye, h.sun_dir");
    const std::size_t ap_result_gate =
        draw.find("if (h.q_ap_on && builtAp != nullptr)", ap_scale_gate);
    const std::size_t ap_pair_assignment =
        draw.find("apVol = builtAp;", ap_result_gate);
    const std::size_t ap_pair_fail_open =
        draw.find("if (apTransVol == nullptr)", ap_pair_assignment);
    const std::size_t analytic_fog_fallback =
        draw.find("ShouldUseAnalyticLocalFog(");
    const std::size_t analytic_fog_shared_decision =
        draw.find("canCompositeLocalFog))", analytic_fog_fallback);
    EXPECT_TRUE(ap_scale_gate != std::string::npos);
    EXPECT_TRUE(stable_ap_matrix != std::string::npos);
    EXPECT_TRUE(ap_result_gate != std::string::npos);
    EXPECT_TRUE(ap_pair_assignment != std::string::npos);
    EXPECT_TRUE(ap_pair_fail_open != std::string::npos);
    EXPECT_TRUE(analytic_fog_fallback != std::string::npos);
    EXPECT_TRUE(analytic_fog_shared_decision != std::string::npos);
    EXPECT_TRUE(stable_ap_matrix < ap_scale_gate);
    EXPECT_TRUE(ap_scale_gate < ap_result_gate);
    EXPECT_TRUE(ap_result_gate < ap_pair_assignment);
    EXPECT_TRUE(ap_pair_assignment < ap_pair_fail_open);
    EXPECT_TRUE(ap_pair_fail_open < analytic_fog_fallback);
    EXPECT_TRUE(
        analytic_fog_fallback < analytic_fog_shared_decision);
}

ACS_TEST(PostEffects, AnimatedCloudsDoNotReceiveASecondGlobalTaaHistory)
{
    const std::string draw = ReadDrawScene3DSource();
    const std::string post =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    EXPECT_TRUE(!draw.empty());
    EXPECT_TRUE(!post.empty());
    if (draw.empty() || post.empty()) return;

    const std::size_t cloud_policy =
        draw.find("const bool animatedCloudsRequested =");
    const std::size_t taa_policy =
        draw.find("!animatedCloudsRequested;", cloud_policy);
    const std::size_t jitter =
        draw.find("if (taaOn)", taa_policy);
    EXPECT_TRUE(cloud_policy != std::string::npos);
    EXPECT_TRUE(taa_policy != std::string::npos);
    EXPECT_TRUE(jitter != std::string::npos);
    EXPECT_TRUE(cloud_policy < taa_policy);
    EXPECT_TRUE(taa_policy < jitter);

    const std::size_t taa_gate =
        post.find("if (params.taa_enabled) {");
    const std::size_t disabled_history_reset =
        post.find("m_TaaFrame = 0;", taa_gate);
    const std::size_t bloom_stage =
        post.find("if (params.bloom_enabled)", disabled_history_reset);
    EXPECT_TRUE(taa_gate != std::string::npos);
    EXPECT_TRUE(disabled_history_reset != std::string::npos);
    EXPECT_TRUE(bloom_stage != std::string::npos);
    EXPECT_TRUE(taa_gate < disabled_history_reset);
    EXPECT_TRUE(disabled_history_reset < bloom_stage);
}

ACS_TEST(PostEffects, TemporalShaderBranchesPublishInitializedValues)
{
    const std::string post_source =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    const std::string ssgi_source =
        ReadWorkspaceSource("src/render/Ssgi.cpp");
    const std::string ssr_source =
        ReadWorkspaceSource("src/render/Ssr.cpp");
    EXPECT_TRUE(!post_source.empty());
    EXPECT_TRUE(!ssgi_source.empty());
    EXPECT_TRUE(!ssr_source.empty());
    if (post_source.empty() || ssgi_source.empty() || ssr_source.empty())
        return;

    const std::string taa_shader =
        ExtractRawShader(post_source, "const char* kTaaResolvePS");
    const std::string tonemap_shader =
        ExtractRawShader(post_source, "const char* kTonemapPS");
    const std::string ssgi_shader =
        ExtractRawShader(ssgi_source, "const char* kSsgiTemporalHLSL");
    const std::string ssr_shader =
        ExtractRawShader(ssr_source, "const char* kSsrHLSL");
    const std::string ssr_temporal_shader =
        ExtractRawShader(ssr_source, "const char* kSsrTemporalHLSL");
    EXPECT_TRUE(!taa_shader.empty());
    EXPECT_TRUE(!tonemap_shader.empty());
    EXPECT_TRUE(!ssgi_shader.empty());
    EXPECT_TRUE(!ssr_shader.empty());
    EXPECT_TRUE(!ssr_temporal_shader.empty());

    const std::string taa_reproject =
        ExtractFunction(taa_shader, "float2 ComputeMotionUv(float2 uv)");
    const std::string tonemap =
        ExtractFunction(tonemap_shader, "float3 Tonemap(float3 c, int kind)");
    const std::string ssgi_reproject =
        ExtractFunction(ssgi_shader, "float2 ReprojectUv(float2 uv)");
    const std::string hiz_min =
        ExtractFunction(
            ssr_shader,
            "float HiZMinAt(float2 pixel, int level, out float2 block, out float block_size)");
    const std::string ssr_reproject =
        ExtractFunction(ssr_temporal_shader, "float2 ReprojectUv(float2 uv)");
    EXPECT_TRUE(!taa_reproject.empty());
    EXPECT_TRUE(!tonemap.empty());
    EXPECT_TRUE(!ssgi_reproject.empty());
    EXPECT_TRUE(!hiz_min.empty());
    EXPECT_TRUE(!ssr_reproject.empty());

    EXPECT_TRUE(taa_reproject.find("float2 reprojected_uv = uv;") !=
                std::string::npos);
    EXPECT_TRUE(taa_reproject.find("return reprojected_uv;") !=
                std::string::npos);
    EXPECT_EQ(CountOccurrences(taa_reproject, "return "), std::size_t{1});

    EXPECT_TRUE(tonemap.find("float3 result = float3(0.0, 0.0, 0.0);") !=
                std::string::npos);
    EXPECT_TRUE(tonemap.find("return result;") != std::string::npos);
    EXPECT_EQ(CountOccurrences(tonemap, "return "), std::size_t{1});

    EXPECT_TRUE(ssgi_reproject.find("float2 reprojected_uv = uv;") !=
                std::string::npos);
    EXPECT_TRUE(ssgi_reproject.find("return reprojected_uv;") !=
                std::string::npos);
    EXPECT_EQ(CountOccurrences(ssgi_reproject, "return "), std::size_t{1});
    EXPECT_TRUE(ssgi_shader.find("float2 huv = v.uv;") !=
                std::string::npos);

    EXPECT_TRUE(hiz_min.find("block = float2(0.0, 0.0);") !=
                std::string::npos);
    EXPECT_TRUE(hiz_min.find("block_size = 1.0;") != std::string::npos);
    EXPECT_TRUE(hiz_min.find("float min_depth = 0.0;") !=
                std::string::npos);
    EXPECT_TRUE(hiz_min.find("return min_depth;") != std::string::npos);
    EXPECT_EQ(CountOccurrences(hiz_min, "return "), std::size_t{1});
    EXPECT_TRUE(ssr_shader.find(
                    "float2 block = float2(0.0, 0.0);") !=
                std::string::npos);
    EXPECT_TRUE(ssr_shader.find("float block_size = 1.0;") !=
                std::string::npos);

    EXPECT_TRUE(ssr_reproject.find("float2 reprojected_uv = uv;") !=
                std::string::npos);
    EXPECT_TRUE(ssr_reproject.find("return reprojected_uv;") !=
                std::string::npos);
    EXPECT_EQ(CountOccurrences(ssr_reproject, "return "), std::size_t{1});
    EXPECT_TRUE(ssr_temporal_shader.find("float2 huv = v.uv;") !=
                std::string::npos);
}

ACS_TEST(PostEffects, InverseTransposePreservesNormalOrthogonality)
{
    const FMat4 model = FMat4::Scale(FVec3{2.0f, 0.5f, 3.0f});
    const FVec3 tangent = Normalize(FVec3{1.0f, 1.0f, 0.0f});
    const FVec3 normal = Normalize(FVec3{-1.0f, 1.0f, 0.0f});

    const FVec3 world_tangent = Normalize(TransformVector(tangent, model));
    const FVec3 world_normal =
        Normalize(TransformVector(normal, MakeSafeNormalMatrix(model)));
    const FVec3 incorrectly_scaled_normal = Normalize(TransformVector(normal, model));

    EXPECT_NEAR(Dot(world_tangent, world_normal), 0.0f, 1e-5f);
    EXPECT_TRUE(Dot(world_tangent, incorrectly_scaled_normal) < -0.5f);
}

ACS_TEST(PostEffects, SingularScaleNormalMatrixStaysFinite)
{
    const FMat4 singular =
        FMat4::Scale(FVec3{1.0f, 0.0f, 2.0f}) * FMat4::RotationY(0.7f);
    const FMat4 normal = MakeSafeNormalMatrix(singular);
    for (u32 row = 0; row < 4; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            EXPECT_TRUE(std::isfinite(normal.m[row][col]));
        }
    }
    const FVec3 transformed = TransformVector(FVec3::UnitY(), normal);
    EXPECT_TRUE(std::isfinite(transformed.x));
    EXPECT_TRUE(std::isfinite(transformed.y));
    EXPECT_TRUE(std::isfinite(transformed.z));
}

ACS_TEST(PostEffects, RefractionFrameTracksViewportWithoutBackDepth)
{
    FRefractionShader refraction;
    refraction.SetFrame(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f}, 1920, 1080);
    EXPECT_EQ(refraction.ScreenWidth(), 1920u);
    EXPECT_EQ(refraction.ScreenHeight(), 1080u);

    // Zero means "preserve" so callers may update one dimension independently.
    refraction.SetFrame(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f}, 0, 720);
    EXPECT_EQ(refraction.ScreenWidth(), 1920u);
    EXPECT_EQ(refraction.ScreenHeight(), 720u);
}

ACS_TEST(PostEffects, HizSkipStopsBeforeTheNextBlockBoundary)
{
    const auto max_skip = [](FVec2 pixel, FVec2 step, f32 block_size) noexcept {
        const FVec2 block_min{
            std::floor(pixel.x / block_size) * block_size,
            std::floor(pixel.y / block_size) * block_size};
        f32 tx = 1.0e20f;
        f32 ty = 1.0e20f;
        if (step.x > 1.0e-6f)
            tx = (block_min.x + block_size - pixel.x) / step.x;
        else if (step.x < -1.0e-6f)
            tx = (block_min.x - pixel.x) / step.x;
        if (step.y > 1.0e-6f)
            ty = (block_min.y + block_size - pixel.y) / step.y;
        else if (step.y < -1.0e-6f)
            ty = (block_min.y - pixel.y) / step.y;
        const f32 result = std::ceil(tx < ty ? tx : ty) - 1.0f;
        return result > 0.0f ? static_cast<u32>(result) : 0u;
    };

    EXPECT_EQ(max_skip(FVec2{1.0f, 2.0f}, FVec2{1.0f, 0.0f}, 8.0f), 6u);
    EXPECT_EQ(max_skip(FVec2{7.2f, 2.0f}, FVec2{1.0f, 0.0f}, 8.0f), 0u);
    EXPECT_EQ(max_skip(FVec2{2.0f, 7.5f}, FVec2{0.5f, 1.0f}, 8.0f), 0u);
    EXPECT_EQ(max_skip(FVec2{9.0f, 9.0f}, FVec2{-1.0f, -0.25f}, 8.0f), 0u);

    // Coarser pyramid levels obey the same strict-before-boundary rule.
    EXPECT_EQ(max_skip(FVec2{17.0f, 5.0f}, FVec2{1.0f, 0.0f}, 64.0f), 46u);
    EXPECT_EQ(max_skip(FVec2{63.25f, 8.0f}, FVec2{1.0f, 0.0f}, 64.0f), 0u);
    EXPECT_EQ(max_skip(FVec2{100.0f, 127.5f}, FVec2{0.25f, 1.0f}, 128.0f), 0u);
}

ACS_TEST(PostEffects, PipelinesCompileOnActiveBackend)
{
    // The Diligent allocator adapter intentionally refuses to create a device
    // without the engine memory system. Own a test-local lifetime when the
    // runner has not initialized one, so this test actually compiles the
    // pipelines instead of silently taking the headless-device skip.
    struct FMemorySystemScope {
        bool owned = false;
        ~FMemorySystemScope() noexcept {
            if (owned) FMemorySystem::Shutdown();
        }
    } memory_scope;
    if (FMemorySystem::Get(ESegment::Resource) == nullptr) {
        const auto memory_result = FMemorySystem::Init(FMemorySystem::DefaultConfig());
        EXPECT_TRUE(memory_result.IsOk());
        if (memory_result.IsErr()) return;
        memory_scope.owned = true;
    }

    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return; // Headless CI may not expose a graphics adapter.
    IRhiDevice& device = *device_result.Value();

    FPostProcess post;
    EXPECT_TRUE(post.Init(device, 64, 64, EFormat::B8G8R8A8_UNorm).IsOk());

    FSsao ssao;
    EXPECT_TRUE(ssao.Init(device, 64, 64).IsOk());

    FSsgi ssgi;
    EXPECT_TRUE(ssgi.Init(device, 64, 64).IsOk());

    FSsr ssr;
    EXPECT_TRUE(ssr.Init(device, EFormat::R16G16B16A16_Float, 64, 64).IsOk());

    FHiZ hiz;
    EXPECT_TRUE(hiz.Init(device, 65, 33).IsOk());
    EXPECT_EQ(hiz.Width(), 9u);
    EXPECT_EQ(hiz.Height(), 5u);
    EXPECT_EQ(hiz.PhysicalWidth(), 16u);
    EXPECT_EQ(hiz.PhysicalHeight(), 8u);
    EXPECT_EQ(hiz.MipCount(), 5u); // 16x8 -> 8x4 -> 4x2 -> 2x1 -> 1x1
    EXPECT_TRUE(hiz.Texture() == hiz.EvenTexture());
    EXPECT_TRUE(hiz.EvenTexture() != nullptr);
    EXPECT_TRUE(hiz.OddTexture() != nullptr);
    EXPECT_TRUE(hiz.EvenTexture() != hiz.OddTexture());
    EXPECT_EQ(hiz.EvenTexture()->Width(), hiz.PhysicalWidth());
    EXPECT_EQ(hiz.EvenTexture()->Height(), hiz.PhysicalHeight());
    EXPECT_EQ(hiz.EvenTexture()->PixelFormat(), EFormat::R32G32_Float);
    EXPECT_EQ(hiz.OddTexture()->PixelFormat(), EFormat::R32G32_Float);
    EXPECT_EQ(hiz.EvenTexture()->MipLevels(), hiz.MipCount());
    EXPECT_EQ(hiz.OddTexture()->MipLevels(), hiz.MipCount());

    FMotionVector motion;
    EXPECT_TRUE(motion.Init(device, 64, 64).IsOk());

    FRefractionShader refraction;
    EXPECT_TRUE(refraction.Init(device, EFormat::R16G16B16A16_Float,
                                EFormat::D32_Float).IsOk());
    refraction.SetFrame(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f}, 64, 64);
    refraction.SetObject(FMat4::Identity(), 1.5f, 0.5f, FVec3{1.0f, 1.0f, 1.0f},
                         0.25f, 0.0f, 6);
    IRhiBuffer* refr_first = refraction.PerObjectCB();
    refraction.SetObject(FMat4::Identity(), 1.33f, 0.2f, FVec3{0.8f, 0.9f, 1.0f},
                         0.5f, 0.0f, 1);
    EXPECT_TRUE(refr_first != nullptr);
    EXPECT_TRUE(refraction.PerObjectCB() != nullptr);
    EXPECT_TRUE(refraction.PerObjectCB() != refr_first);

    FVolumetricClouds clouds;
    EXPECT_TRUE(clouds.Init(device, EFormat::R16G16B16A16_Float).IsOk());

    // Keep the dedicated 3D-water VS displacement / normal-map / refraction
    // pipeline compiled on every active RHI backend. CPU ripple lifetime tests
    // alone cannot catch regressions in the embedded HLSL.
    FWaterSurface3D water;
    EXPECT_TRUE(water.Init(device, EFormat::R16G16B16A16_Float,
                           EFormat::D32_Float).IsOk());

    FStandardShader standard;
    const auto standard_result =
        standard.Init(device, EFormat::R16G16B16A16_Float, EFormat::D32_Float);
    EXPECT_TRUE(standard_result.IsOk());
    if (standard_result.IsOk()) {
        standard.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                           nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        EXPECT_TRUE(standard.SetObject(FMat4::Identity()));
        IRhiBuffer* first_object = standard.PerObjectCB();
        EXPECT_TRUE(standard.SetObject(FMat4::Identity()));
        EXPECT_TRUE(standard.PerObjectCB() != first_object);
        for (u32 i = 2; i < FStandardShader::kMaxObjectDrawsPerFrame; ++i)
            EXPECT_TRUE(standard.SetObject(FMat4::Identity()));
        EXPECT_TRUE(!standard.SetObject(FMat4::Identity()));
        EXPECT_TRUE(standard.PerObjectCB() == nullptr);

        standard.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                           nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        EXPECT_TRUE(standard.SetObject(FMat4::Identity()));
        EXPECT_TRUE(standard.PerObjectCB() != nullptr);
    }

    FSkinnedShader skinned;
    const auto skinned_result =
        skinned.Init(device, EFormat::R16G16B16A16_Float, EFormat::D32_Float);
    EXPECT_TRUE(skinned_result.IsOk());
    if (skinned_result.IsOk()) {
        skinned.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                          nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        EXPECT_TRUE(skinned.SetObject(FMat4::Identity()));
        EXPECT_TRUE(skinned.SetBonePalette(nullptr, 0));
        IRhiBuffer* first_object = skinned.PerObjectCB();
        IRhiBuffer* first_bones = skinned.BonesCB();
        EXPECT_TRUE(skinned.SetObject(FMat4::Identity()));
        EXPECT_TRUE(skinned.SetBonePalette(nullptr, 0));
        EXPECT_TRUE(skinned.PerObjectCB() != first_object);
        EXPECT_TRUE(skinned.BonesCB() != first_bones);
        for (u32 i = 2; i < FSkinnedShader::kMaxObjectDrawsPerFrame; ++i)
            EXPECT_TRUE(skinned.SetObject(FMat4::Identity()));
        EXPECT_TRUE(!skinned.SetObject(FMat4::Identity()));
        EXPECT_TRUE(skinned.PerObjectCB() == nullptr);
        EXPECT_TRUE(skinned.BonesCB() == nullptr);
        EXPECT_TRUE(!skinned.SetBonePalette(nullptr, 0));

        skinned.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                          nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        EXPECT_TRUE(skinned.SetObject(FMat4::Identity()));
        EXPECT_TRUE(skinned.SetBonePalette(nullptr, 0));
    }

    FPbrShader pbr;
    const auto pbr_result = pbr.Init(device, EFormat::R16G16B16A16_Float,
                                     EFormat::D32_Float);
    EXPECT_TRUE(pbr_result.IsOk());
    if (pbr_result.IsOk()) {
        pbr.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                      nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        // Manual render paths bind slot 0 before the first SetObject, so reset
        // keeps a valid compatibility pointer without consuming that slot.
        EXPECT_TRUE(pbr.PerObjectCB() != nullptr);
        for (u32 i = 0; i < 256; ++i) {
            pbr.SetObject(FMat4::Identity());
            EXPECT_TRUE(pbr.PerObjectCB() != nullptr);
        }

        // Never wrap to slot zero: that would overwrite a recorded Raw DX12 draw.
        pbr.SetObject(FMat4::Identity());
        EXPECT_TRUE(pbr.PerObjectCB() == nullptr);

        // SetLights is the documented frame boundary and restores the capacity.
        pbr.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                      nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        pbr.SetObject(FMat4::Identity());
        EXPECT_TRUE(pbr.PerObjectCB() != nullptr);
    }
}
