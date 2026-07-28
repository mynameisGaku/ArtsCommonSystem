// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#include "editor_abi/EditorFrustumCulling.h"
#include "editor_abi/EditorSubsurfaceVisibility.h"
#include "render/PostProcess.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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

std::string ExtractRawShader(
    const std::string& source,
    const char* declaration)
{
    const std::size_t declaration_begin = source.find(declaration);
    if (declaration_begin == std::string::npos) return {};
    const std::size_t raw_begin =
        source.find("R\"(", declaration_begin);
    if (raw_begin == std::string::npos) return {};
    const std::size_t content_begin = raw_begin + 3u;
    const std::size_t content_end =
        source.find(")\";", content_begin);
    if (content_end == std::string::npos) return {};
    return source.substr(
        content_begin, content_end - content_begin);
}

std::string ExtractFunction(
    const std::string& source,
    const char* signature)
{
    const std::size_t signature_begin = source.find(signature);
    if (signature_begin == std::string::npos) return {};
    const std::size_t body_begin =
        source.find('{', signature_begin);
    if (body_begin == std::string::npos) return {};
    u32 depth = 0u;
    for (std::size_t index = body_begin;
         index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            if (depth == 0u) return {};
            --depth;
            if (depth == 0u) {
                return source.substr(
                    signature_begin,
                    index - signature_begin + 1u);
            }
        }
    }
    return {};
}

std::size_t CountOccurrences(
    const std::string& text,
    const char* needle)
{
    const std::string token{needle};
    if (token.empty()) return 0u;
    std::size_t count = 0u;
    std::size_t cursor = 0u;
    while ((cursor = text.find(token, cursor)) !=
           std::string::npos) {
        ++count;
        cursor += token.size();
    }
    return count;
}

} // namespace

ACS_TEST(PostEffectWorkload,
         BloomThresholdUsesTheSameManualExposureAsTonemapping)
{
    const std::string source =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    const std::string extract =
        ExtractRawShader(source, "const char* kExtractPS");
    const std::string exposure_apply =
        ExtractRawShader(source, "const char* kExposeApplyPS");
    const std::string tonemap =
        ExtractRawShader(source, "const char* kTonemapPS");
    const std::string tonemap_pass =
        ExtractFunction(source, "bool FPostProcess::Pass_Tonemap");
    const std::string scene_input =
        ExtractFunction(source, "IRhiTexture* FPostProcess::SceneInput");
    const std::string create_pipelines =
        ExtractFunction(source, "TResult<void> FPostProcess::CreatePipelines");
    EXPECT_TRUE(!extract.empty());
    EXPECT_TRUE(!exposure_apply.empty());
    EXPECT_TRUE(!tonemap.empty());
    EXPECT_TRUE(!tonemap_pass.empty());
    EXPECT_TRUE(!scene_input.empty());
    EXPECT_TRUE(!create_pipelines.empty());
    if (extract.empty() || exposure_apply.empty() ||
        tonemap.empty() || tonemap_pass.empty() ||
        scene_input.empty() || create_pipelines.empty()) {
        return;
    }

    // Auto exposure is baked into SceneInput by Exposure.Apply. Manual
    // exposure/EV compensation is then applied once to each extraction tap,
    // exactly as it is applied once to scene color in tonemapping.
    EXPECT_TRUE(exposure_apply.find(
        "return float4(c * e, 1.0);") != std::string::npos);
    EXPECT_TRUE(exposure_apply.find("params0.w") ==
                std::string::npos);
    EXPECT_TRUE(extract.find(
        "float manual_exposure = params0.w;") !=
        std::string::npos);
    EXPECT_EQ(
        CountOccurrences(extract, "* manual_exposure"),
        std::size_t{4u});
    EXPECT_EQ(
        CountOccurrences(tonemap, "hdr_col *= params0.w;"),
        std::size_t{1u});
    EXPECT_TRUE(tonemap.find(
        "bloom.Sample(bloom_sampler, v.uv).rgb) * params0.w") ==
        std::string::npos);

    // Standalone SSR is scene-linear too. It receives manual exposure once,
    // and only a completed auto-exposure result enables the 1x1 sample.
    EXPECT_TRUE(tonemap.find(
        "Texture2D    adapted_exposure : register(t3);") !=
        std::string::npos);
    EXPECT_TRUE(tonemap.find(
        "[branch]\n    if (params3.w >= 0.5)") !=
        std::string::npos);
    EXPECT_EQ(
        CountOccurrences(
            tonemap, "adapted_exposure.SampleLevel("),
        std::size_t{1u});
    EXPECT_TRUE(tonemap.find(
        "float ssr_exposure = params0.w;") !=
        std::string::npos);
    EXPECT_TRUE(tonemap.find(
        "(params3.y * ssr_exposure)") !=
        std::string::npos);
    EXPECT_TRUE(create_pipelines.find(
        "pd.texture_slots = 4;") != std::string::npos);
    EXPECT_TRUE(create_pipelines.find(
        "pd.texture_names[3] = \"adapted_exposure\";") !=
        std::string::npos);
    EXPECT_TRUE(create_pipelines.find(
        "pd.static_sampler_count = 4;") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        "const bool scene_auto_exposed =") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        "p.auto_exposure_enabled &&") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        "m_ExposureOutputValid &&") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find("m_ExposedRt;") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        "p.ssr_texture != nullptr &&") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        "tonemap_params.auto_exposure_enabled = expose_ssr;") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        "IRhiTexture* adapted_exposure_input =") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        "? m_Exposure[m_AutoFrame % 2].Get()") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        ": m_BlackFb.Get();") != std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        "if (!bloom_input || !ssr_input || !adapted_exposure_input)") !=
        std::string::npos);
    EXPECT_TRUE(tonemap_pass.find(
        "cmd.SetTexture(3, *adapted_exposure_input);") !=
        std::string::npos);
    EXPECT_EQ(
        CountOccurrences(tonemap_pass, "cmd.SetTexture("),
        std::size_t{4u});
    EXPECT_TRUE(scene_input.find(
        "p.auto_exposure_enabled && m_ExposureOutputValid && "
        "m_ExposedRt") != std::string::npos);

    FPostProcessParams neutral{};
    neutral.exposure = 1.0f;
    neutral.Sanitize();
    EXPECT_EQ(neutral.exposure, 1.0f);

    FPostProcessParams non_finite{};
    non_finite.exposure =
        std::numeric_limits<f32>::quiet_NaN();
    non_finite.Sanitize();
    EXPECT_EQ(non_finite.exposure, 1.0f);

    FPostProcessParams negative{};
    negative.exposure = -100.0f;
    negative.Sanitize();
    EXPECT_EQ(negative.exposure, 0.0f);

    FPostProcessParams excessive{};
    excessive.exposure = 1000.0f;
    excessive.Sanitize();
    EXPECT_EQ(excessive.exposure, 64.0f);
}

ACS_TEST(PostEffectWorkload,
         SubsurfacePresenceTracksMainViewWithoutLosingWarmup)
{
    using editor_frustum_culling::FSubmissionMaskView;
    using editor_subsurface_visibility::FPresence;

    u8 visibility[2] = {1u, 0u};
    const FSubmissionMaskView masked{
        true, visibility, 2u};

    // Offscreen SSSS still requests scene-wide warm-up, but records no
    // full-resolution frame workload.
    FPresence offscreen{};
    offscreen.Observe(1u, true, true, masked);
    EXPECT_TRUE(offscreen.scene_has_material);
    EXPECT_FALSE(offscreen.main_view_has_material);
    EXPECT_FALSE(offscreen.Complete());

    // Enter, leave and re-enter are stateless mask decisions. Existing
    // resources can remain warm while the recurring workload toggles.
    visibility[1] = 1u;
    FPresence visible{};
    visible.Observe(1u, true, true, masked);
    EXPECT_TRUE(visible.Complete());
    visibility[1] = 0u;
    FPresence hidden_again{};
    hidden_again.Observe(1u, true, true, masked);
    EXPECT_TRUE(hidden_again.scene_has_material);
    EXPECT_FALSE(hidden_again.main_view_has_material);
    visibility[1] = 1u;
    FPresence visible_again{};
    visible_again.Observe(1u, true, true, masked);
    EXPECT_TRUE(visible_again.Complete());

    // Missing/short diagnostics fail open, while a material without a
    // renderable opaque GPU mesh may warm shaders but cannot request passes.
    const FSubmissionMaskView short_mask{
        true, visibility, 1u};
    FPresence fail_open{};
    fail_open.Observe(1u, true, true, short_mask);
    EXPECT_TRUE(fail_open.Complete());
    FPresence not_renderable{};
    not_renderable.Observe(0u, true, false, masked);
    EXPECT_TRUE(not_renderable.scene_has_material);
    EXPECT_FALSE(not_renderable.main_view_has_material);
}

ACS_TEST(PostEffectWorkload,
         SubsurfaceGateMatchesDepthAndOpaquePassPolicy)
{
    using namespace editor_frustum_culling;
    const u8 visibility[1] = {0u};
    const FSubmissionMaskView main_view{
        true, visibility, 1u};

    EXPECT_FALSE(SubmissionMaskForPass(
        ESceneGeometryPass::NormalDepthPrepass,
        main_view).ShouldSubmit(0u));
    EXPECT_FALSE(SubmissionMaskForPass(
        ESceneGeometryPass::PbrOpaqueCount,
        main_view).ShouldSubmit(0u));
    EXPECT_FALSE(SubmissionMaskForPass(
        ESceneGeometryPass::PbrOpaqueDraw,
        main_view).ShouldSubmit(0u));

    // Camera-relative SSSS gating cannot leak into light/world-space work.
    EXPECT_TRUE(SubmissionMaskForPass(
        ESceneGeometryPass::ShadowCaster,
        main_view).ShouldSubmit(0u));
    EXPECT_TRUE(SubmissionMaskForPass(
        ESceneGeometryPass::VxgiVoxelization,
        main_view).ShouldSubmit(0u));
}

ACS_TEST(PostEffectWorkload,
         EditorRecordsAndProfilesSubsurfaceOnlyForVisibleWork)
{
    const std::string source =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string draw =
        ExtractFunction(source, "void DrawScene3D(");
    const std::string inspect =
        ExtractFunction(
            source,
            "InspectOpaqueSsssMaterials(");
    const std::string begin_profiler =
        ExtractFunction(source, "static void BeginProfilerFrame(");
    EXPECT_TRUE(!draw.empty());
    EXPECT_TRUE(!inspect.empty());
    EXPECT_TRUE(!begin_profiler.empty());
    if (draw.empty() || inspect.empty() ||
        begin_profiler.empty()) {
        return;
    }

    const std::size_t visibility_build =
        draw.find("BuildSceneMeshVisibility(");
    const std::size_t presence_inspection =
        draw.find("InspectOpaqueSsssMaterials(");
    const std::size_t warmup_request =
        draw.find("ssss_presence.scene_has_material");
    const std::size_t frame_gate =
        draw.find("ssss_presence.main_view_has_material");
    const std::size_t gbuffer_gate =
        draw.find("ssss_frame_resources) &&", frame_gate);
    EXPECT_TRUE(visibility_build != std::string::npos);
    EXPECT_TRUE(presence_inspection != std::string::npos);
    EXPECT_TRUE(warmup_request != std::string::npos);
    EXPECT_TRUE(frame_gate != std::string::npos);
    EXPECT_TRUE(gbuffer_gate != std::string::npos);
    EXPECT_TRUE(visibility_build < presence_inspection);
    EXPECT_TRUE(presence_inspection < warmup_request);
    EXPECT_TRUE(warmup_request < frame_gate);
    EXPECT_TRUE(frame_gate < gbuffer_gate);

    EXPECT_TRUE(inspect.find(
        "ESceneGeometryPass::PbrOpaqueCount") !=
        std::string::npos);
    EXPECT_TRUE(inspect.find(
        "eligible_for_main_view_draw") !=
        std::string::npos);
    EXPECT_TRUE(inspect.find("presence.Observe(") !=
                std::string::npos);

    const std::size_t workload =
        draw.find("if (ssss_mrt_bound)");
    const std::size_t cpu_profile =
        draw.find("ssssOpaqueScope", workload);
    const std::size_t gpu_profile =
        draw.find("ssssOpaqueGpuScope", workload);
    const std::size_t render =
        draw.find("h.ssss3d.Render(", workload);
    EXPECT_TRUE(workload != std::string::npos);
    EXPECT_TRUE(cpu_profile != std::string::npos);
    EXPECT_TRUE(gpu_profile != std::string::npos);
    EXPECT_TRUE(render != std::string::npos);
    EXPECT_TRUE(workload < cpu_profile);
    EXPECT_TRUE(cpu_profile < render);
    EXPECT_TRUE(gpu_profile < render);
    EXPECT_TRUE(begin_profiler.find(
        "host.profiler_work = {};") != std::string::npos);
}

ACS_TEST(PostEffectWorkload,
         TemporalScreenSpaceOutputsAreTransactionalAndStrictlyBound)
{
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string post =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    const std::string ssr =
        ReadWorkspaceSource("src/render/Ssr.cpp");
    const std::string ssr_header =
        ReadWorkspaceSource("src/render/Ssr.h");
    const std::string ssgi =
        ReadWorkspaceSource("src/render/Ssgi.cpp");
    const std::string ssgi_header =
        ReadWorkspaceSource("src/render/Ssgi.h");
    EXPECT_TRUE(!editor.empty());
    EXPECT_TRUE(!post.empty());
    EXPECT_TRUE(!ssr.empty());
    EXPECT_TRUE(!ssr_header.empty());
    EXPECT_TRUE(!ssgi.empty());
    EXPECT_TRUE(!ssgi_header.empty());
    if (editor.empty() || post.empty() || ssr.empty() ||
        ssr_header.empty() || ssgi.empty() ||
        ssgi_header.empty()) {
        return;
    }

    const std::string draw =
        ExtractFunction(editor, "void DrawScene3D(");
    const std::string taa =
        ExtractFunction(post, "bool FPostProcess::Pass_TaaResolve");
    const std::string tonemap =
        ExtractFunction(post, "bool FPostProcess::Pass_Tonemap");
    const std::string ssr_render =
        ExtractFunction(ssr, "void FSsr::Render");
    const std::string ssr_create_targets =
        ExtractFunction(ssr, "TResult<void> FSsr::CreateOutputRT");
    const std::string ssr_resize =
        ExtractFunction(ssr, "TResult<void> FSsr::Resize");
    const std::string ssr_init =
        ExtractFunction(ssr, "TResult<void> FSsr::Init");
    const std::string ssr_pipeline =
        ExtractFunction(ssr, "TResult<void> FSsr::CreatePipeline");
    const std::string ssgi_render =
        ExtractFunction(ssgi, "void FSsgi::Render");
    const std::string ssgi_create_targets =
        ExtractFunction(ssgi, "TResult<void> FSsgi::CreateOutputRT");
    const std::string ssgi_resize =
        ExtractFunction(ssgi, "TResult<void> FSsgi::Resize");
    const std::string ssgi_pipeline =
        ExtractFunction(ssgi, "TResult<void> FSsgi::CreatePipeline");
    EXPECT_TRUE(!draw.empty());
    EXPECT_TRUE(!taa.empty());
    EXPECT_TRUE(!tonemap.empty());
    EXPECT_TRUE(!ssr_render.empty());
    EXPECT_TRUE(!ssr_create_targets.empty());
    EXPECT_TRUE(!ssr_resize.empty());
    EXPECT_TRUE(!ssr_init.empty());
    EXPECT_TRUE(!ssr_pipeline.empty());
    EXPECT_TRUE(!ssgi_render.empty());
    EXPECT_TRUE(!ssgi_create_targets.empty());
    EXPECT_TRUE(!ssgi_resize.empty());
    EXPECT_TRUE(!ssgi_pipeline.empty());
    if (draw.empty() || taa.empty() || tonemap.empty() ||
        ssr_render.empty() || ssr_create_targets.empty() ||
        ssr_resize.empty() || ssr_init.empty() ||
        ssr_pipeline.empty() || ssgi_render.empty() ||
        ssgi_create_targets.empty() || ssgi_resize.empty() ||
        ssgi_pipeline.empty()) {
        return;
    }

    // Publication is fail-closed: a caller cannot advertise stale history
    // when an incomplete frame returned before recording every pass.
    EXPECT_TRUE(ssr_header.find(
        "bool HasValidOutput() const noexcept") !=
        std::string::npos);
    EXPECT_TRUE(ssgi_header.find(
        "bool HasValidOutput() const noexcept") !=
        std::string::npos);
    const std::size_t ssr_reset =
        ssr_render.find("m_OutputValid = false;");
    const std::size_t ssr_guard =
        ssr_render.find("if (!m_Output || !m_History[0]");
    const std::size_t ssr_complete =
        ssr_render.find("m_OutputValid = true;");
    EXPECT_TRUE(ssr_reset != std::string::npos);
    EXPECT_TRUE(ssr_guard != std::string::npos);
    EXPECT_TRUE(ssr_complete != std::string::npos);
    EXPECT_TRUE(ssr_reset < ssr_guard);
    EXPECT_TRUE(ssr_guard < ssr_complete);
    const std::size_t ssgi_reset =
        ssgi_render.find("m_OutputValid = false;");
    const std::size_t ssgi_guard =
        ssgi_render.find("if (!m_Output || !m_BlurOutput");
    const std::size_t ssgi_complete =
        ssgi_render.find("m_OutputValid = true;");
    EXPECT_TRUE(ssgi_reset != std::string::npos);
    EXPECT_TRUE(ssgi_guard != std::string::npos);
    EXPECT_TRUE(ssgi_complete != std::string::npos);
    EXPECT_TRUE(ssgi_reset < ssgi_guard);
    EXPECT_TRUE(ssgi_guard < ssgi_complete);
    EXPECT_TRUE(draw.find(
        "h.ssr_computed = h.ssr3d.HasValidOutput();") !=
        std::string::npos);
    EXPECT_TRUE(draw.find(
        "h.ssgi_computed = h.ssgi3d.HasValidOutput();") !=
        std::string::npos);

    // Target sets use a strong commit. Allocation failure leaves the entire
    // previous same-generation output/history set intact.
    const std::size_t ssr_local_output =
        ssr_create_targets.find("TUniquePtr<IRhiTexture> output");
    const std::size_t ssr_local_history =
        ssr_create_targets.find("TUniquePtr<IRhiTexture> history[2]");
    const std::size_t ssr_commit =
        ssr_create_targets.find("m_Output = Move(output);");
    EXPECT_TRUE(ssr_local_output != std::string::npos);
    EXPECT_TRUE(ssr_local_history != std::string::npos);
    EXPECT_TRUE(ssr_commit != std::string::npos);
    EXPECT_TRUE(ssr_local_output < ssr_local_history);
    EXPECT_TRUE(ssr_local_history < ssr_commit);
    EXPECT_TRUE(ssr_create_targets.find("m_Output.Reset()") ==
                std::string::npos);
    const std::size_t ssr_resize_targets =
        ssr_resize.find("CreateOutputRT(*m_Device, width, height)");
    const std::size_t ssr_resize_width =
        ssr_resize.find("m_Width = width;");
    EXPECT_TRUE(ssr_resize_targets != std::string::npos);
    EXPECT_TRUE(ssr_resize_width != std::string::npos);
    EXPECT_TRUE(ssr_resize_targets < ssr_resize_width);
    EXPECT_TRUE(ssr_init.find("FSsr candidate;") !=
                std::string::npos);
    EXPECT_TRUE(ssr_init.find("Shutdown();") !=
                std::string::npos);

    const std::size_t ssgi_local_output =
        ssgi_create_targets.find("TUniquePtr<IRhiTexture> output");
    const std::size_t ssgi_local_history =
        ssgi_create_targets.find("TUniquePtr<IRhiTexture> history[2]");
    const std::size_t ssgi_commit =
        ssgi_create_targets.find("m_Output = Move(output);");
    EXPECT_TRUE(ssgi_local_output != std::string::npos);
    EXPECT_TRUE(ssgi_local_history != std::string::npos);
    EXPECT_TRUE(ssgi_commit != std::string::npos);
    EXPECT_TRUE(ssgi_local_output < ssgi_local_history);
    EXPECT_TRUE(ssgi_local_history < ssgi_commit);
    const std::size_t ssgi_resize_targets =
        ssgi_resize.find("CreateOutputRT(*m_Device, width, height)");
    const std::size_t ssgi_resize_width =
        ssgi_resize.find("m_Width  = width;");
    EXPECT_TRUE(ssgi_resize_targets != std::string::npos);
    EXPECT_TRUE(ssgi_resize_width != std::string::npos);
    EXPECT_TRUE(ssgi_resize_targets < ssgi_resize_width);

    // Declared SRV counts match unconditional bindings in every fullscreen
    // pass. Disabled shader branches receive neutral fallback descriptors.
    EXPECT_TRUE(ssr_pipeline.find(
        "pd.texture_slots = 5;") != std::string::npos);
    EXPECT_TRUE(ssr_pipeline.find(
        "tpd.texture_slots = 3;") != std::string::npos);
    EXPECT_EQ(
        CountOccurrences(ssr_render, "cl.SetTexture("),
        std::size_t{8u});
    EXPECT_EQ(
        CountOccurrences(ssgi_pipeline, ".texture_slots = 3;"),
        std::size_t{3u});
    EXPECT_EQ(
        CountOccurrences(ssgi_render, "cl.SetTexture("),
        std::size_t{9u});
    EXPECT_EQ(
        CountOccurrences(taa, "cmd.SetTexture("),
        std::size_t{5u});
    EXPECT_TRUE(taa.find(
        "if (!slot2_tex || !reactive_tex || !reactive_depth)") !=
        std::string::npos);
    EXPECT_EQ(
        CountOccurrences(tonemap, "cmd.SetTexture("),
        std::size_t{4u});
    EXPECT_TRUE(tonemap.find(
        "cmd.SetTexture(3, *adapted_exposure_input);") !=
        std::string::npos);

    // SSR and SSGI time is counted only inside their enabled + ready + G-buffer
    // gates, and before the authoritative output flag is sampled.
    const std::size_t ssr_gate = draw.find(
        "if (h.q_ssr_on && h.ssr_ready && gbufReady)");
    const std::size_t ssr_cpu =
        draw.find("ssrPostScope", ssr_gate);
    const std::size_t ssr_gpu =
        draw.find("ssrPostGpuScope", ssr_gate);
    const std::size_t ssr_call =
        draw.find("h.ssr3d.Render(", ssr_gate);
    const std::size_t ssr_publish =
        draw.find("h.ssr_computed = h.ssr3d.HasValidOutput();",
                  ssr_gate);
    EXPECT_TRUE(ssr_gate != std::string::npos);
    EXPECT_TRUE(ssr_cpu != std::string::npos);
    EXPECT_TRUE(ssr_gpu != std::string::npos);
    EXPECT_TRUE(ssr_call != std::string::npos);
    EXPECT_TRUE(ssr_publish != std::string::npos);
    EXPECT_TRUE(ssr_gate < ssr_cpu);
    EXPECT_TRUE(ssr_cpu < ssr_call);
    EXPECT_TRUE(ssr_gpu < ssr_call);
    EXPECT_TRUE(ssr_call < ssr_publish);

    const std::size_t ssgi_gate = draw.find(
        "if (h.q_ssgi_on && h.ssgi_ready && gbufReady)");
    const std::size_t ssgi_cpu =
        draw.find("ssgiPostScope", ssgi_gate);
    const std::size_t ssgi_gpu =
        draw.find("ssgiPostGpuScope", ssgi_gate);
    const std::size_t ssgi_call =
        draw.find("h.ssgi3d.Render(", ssgi_gate);
    const std::size_t ssgi_publish =
        draw.find("h.ssgi_computed = h.ssgi3d.HasValidOutput();",
                  ssgi_gate);
    EXPECT_TRUE(ssgi_gate != std::string::npos);
    EXPECT_TRUE(ssgi_cpu != std::string::npos);
    EXPECT_TRUE(ssgi_gpu != std::string::npos);
    EXPECT_TRUE(ssgi_call != std::string::npos);
    EXPECT_TRUE(ssgi_publish != std::string::npos);
    EXPECT_TRUE(ssgi_gate < ssgi_cpu);
    EXPECT_TRUE(ssgi_cpu < ssgi_call);
    EXPECT_TRUE(ssgi_gpu < ssgi_call);
    EXPECT_TRUE(ssgi_call < ssgi_publish);
}

ACS_TEST(PostEffectWorkload,
         DisabledOptionalEffectsRecordNoRecurringFullscreenWork)
{
    const std::string post =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string render =
        ExtractFunction(post, "void FPostProcess::Render");
    const std::string draw =
        ExtractFunction(editor, "void DrawScene3D(");
    EXPECT_TRUE(!render.empty());
    EXPECT_TRUE(!draw.empty());
    if (render.empty() || draw.empty()) return;

    const std::size_t auto_gate = render.find("if (auto_exp)");
    const std::size_t luma_call =
        render.find("Pass_LumaReduce(cmd)", auto_gate);
    const std::size_t exposure_call =
        render.find("Pass_ExposureAdapt(cmd, safe_params)", auto_gate);
    const std::size_t taa_gate =
        render.find("if (safe_params.taa_enabled)");
    const std::size_t taa_call =
        render.find("Pass_TaaResolve(cmd, safe_params)", taa_gate);
    const std::size_t bloom_gate = render.find(
        "if (safe_params.bloom_enabled && "
        "safe_params.bloom_intensity > 0.0f)");
    const std::size_t bloom_call =
        render.find("Pass_Extract(cmd, safe_params)", bloom_gate);
    const std::size_t tonemap_call =
        render.find("Pass_Tonemap(cmd, swapchain");
    EXPECT_TRUE(auto_gate != std::string::npos);
    EXPECT_TRUE(luma_call != std::string::npos);
    EXPECT_TRUE(exposure_call != std::string::npos);
    EXPECT_TRUE(taa_gate != std::string::npos);
    EXPECT_TRUE(taa_call != std::string::npos);
    EXPECT_TRUE(bloom_gate != std::string::npos);
    EXPECT_TRUE(bloom_call != std::string::npos);
    EXPECT_TRUE(tonemap_call != std::string::npos);
    EXPECT_TRUE(auto_gate < luma_call);
    EXPECT_TRUE(luma_call < exposure_call);
    EXPECT_TRUE(taa_gate < taa_call);
    EXPECT_TRUE(bloom_gate < bloom_call);
    EXPECT_TRUE(bloom_call < tonemap_call);
    EXPECT_EQ(
        CountOccurrences(render, "Pass_LumaReduce(cmd)"),
        std::size_t{1u});
    EXPECT_EQ(
        CountOccurrences(render, "Pass_TaaResolve(cmd, safe_params)"),
        std::size_t{1u});
    EXPECT_EQ(
        CountOccurrences(render, "Pass_Extract(cmd, safe_params)"),
        std::size_t{1u});

    const std::size_t ssr_gate = draw.find(
        "if (h.q_ssr_on && h.ssr_ready && gbufReady)");
    const std::size_t ssr_call =
        draw.find("h.ssr3d.Render(", ssr_gate);
    const std::size_t ssr_else =
        draw.find("} else {", ssr_call);
    const std::size_t ssgi_gate = draw.find(
        "if (h.q_ssgi_on && h.ssgi_ready && gbufReady)");
    const std::size_t ssgi_call =
        draw.find("h.ssgi3d.Render(", ssgi_gate);
    const std::size_t ssgi_else =
        draw.find("} else {", ssgi_call);
    EXPECT_TRUE(ssr_gate != std::string::npos);
    EXPECT_TRUE(ssr_call != std::string::npos);
    EXPECT_TRUE(ssr_else != std::string::npos);
    EXPECT_TRUE(ssgi_gate != std::string::npos);
    EXPECT_TRUE(ssgi_call != std::string::npos);
    EXPECT_TRUE(ssgi_else != std::string::npos);
    EXPECT_TRUE(ssr_gate < ssr_call);
    EXPECT_TRUE(ssr_call < ssr_else);
    EXPECT_TRUE(ssgi_gate < ssgi_call);
    EXPECT_TRUE(ssgi_call < ssgi_else);
    EXPECT_EQ(
        CountOccurrences(draw, "h.ssr3d.Render("),
        std::size_t{1u});
    EXPECT_EQ(
        CountOccurrences(draw, "h.ssgi3d.Render("),
        std::size_t{1u});
}

ACS_TEST(PostEffectWorkload,
         SubsurfacePresenceIsNotCachedWithoutMaterialRevisionCoverage)
{
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string mesh_component =
        ReadWorkspaceSource("src/gameframework/MeshComponent3D.h");
    const std::string draw =
        ExtractFunction(editor, "void DrawScene3D(");
    const std::string inspect =
        ExtractFunction(editor, "InspectOpaqueSsssMaterials(");
    const std::size_t cache_begin =
        editor.find("struct FSceneMeshCacheKey");
    const std::size_t cache_end =
        editor.find("struct FEditorHost", cache_begin);
    EXPECT_TRUE(!draw.empty());
    EXPECT_TRUE(!inspect.empty());
    EXPECT_TRUE(!mesh_component.empty());
    EXPECT_TRUE(cache_begin != std::string::npos);
    EXPECT_TRUE(cache_end != std::string::npos);
    if (draw.empty() || inspect.empty() ||
        mesh_component.empty() ||
        cache_begin == std::string::npos ||
        cache_end == std::string::npos) {
        return;
    }
    const std::string cache_key =
        editor.substr(cache_begin, cache_end - cache_begin);

    // MaterialMut exposes mutable authoring without a material-generation
    // counter. The mesh cache key intentionally omits transmission, SSSS,
    // slab MFP/thickness and expression roots, so reusing its revision for
    // presence would be stale. Keep the correctness-first frame inspection.
    EXPECT_TRUE(mesh_component.find(
        "FMaterial2D& MaterialMut() noexcept") !=
        std::string::npos);
    EXPECT_TRUE(cache_key.find("subsurface") == std::string::npos);
    EXPECT_TRUE(cache_key.find("transmission") == std::string::npos);
    EXPECT_TRUE(cache_key.find("mean_free_path") == std::string::npos);
    EXPECT_TRUE(cache_key.find("expressions") == std::string::npos);
    EXPECT_TRUE(inspect.find("material.pbr.transmission") !=
        std::string::npos);
    EXPECT_TRUE(inspect.find("material.pbr.subsurface") !=
        std::string::npos);
    EXPECT_TRUE(inspect.find("slab.mean_free_path_cm") !=
        std::string::npos);
    EXPECT_TRUE(inspect.find(
        "substrate_node.expressions.roots[26]") !=
        std::string::npos);
    EXPECT_EQ(
        CountOccurrences(draw, "InspectOpaqueSsssMaterials(h, all3d)"),
        std::size_t{1u});
}
