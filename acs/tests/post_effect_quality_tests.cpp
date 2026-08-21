// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#include "asset/MeshAsset.h"
#include "foundation/Move.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "memory/MemorySystem.h"
#include "render/Blit.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"
#include "render/HiZ.h"
#include "render/MotionVector.h"
#include "render/NormalMatrix.h"
#include "render/PbrShader.h"
#include "render/PostProcess.h"
#include "render/RefractionShader.h"
#include "render/RenderAssets.h"
#include "render/Ssao.h"
#include "render/Ssgi.h"
#include "render/Ssr.h"
#include "render/Sky.h"
#include "render/SkinnedShader.h"
#include "render/StandardShader.h"
#include "render/SubsurfaceScattering.h"
#include "render/TemporalHistory.h"
#include "render/WaterSurface3D.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

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

ACS_TEST(PostEffects,
         EditorMotionVectorsRunWithoutNormalGbufferAndResetDiscontinuities)
{
    const std::string draw = ReadDrawScene3DSource();
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    EXPECT_TRUE(!draw.empty());
    EXPECT_TRUE(!editor.empty());
    if (draw.empty() || editor.empty()) return;

    // TAA-only and motion-blur-only configurations do not request the normal
    // G-buffer. Motion still has its own MRT/depth pass and counts its exact
    // eligible set before reserving persistent per-object buffers.
    EXPECT_TRUE(draw.find(
        "const bool wantsMotion = taaOn || h.q_ssr_on ||") !=
                std::string::npos);
    EXPECT_TRUE(draw.find(
        "wantsMotion && h.mv_ready && h.pbr3d_ready") !=
                std::string::npos);
    EXPECT_TRUE(draw.find(
        "canDrawMotion && motionEligibleCount > 0u") !=
                std::string::npos);
    EXPECT_TRUE(draw.find(
        "h.mv3d.BeginFrame(motionEligibleCount)") !=
                std::string::npos);
    EXPECT_TRUE(draw.find("gbufReady && h.mv_ready") ==
                std::string::npos);

    // A node advances its transform history only after its draw was actually
    // recorded. The texture is authoritative only when every pre-counted
    // target succeeded, so partial passes cold-start instead of poisoning
    // TAA/SSR/SSGI history.
    EXPECT_TRUE(draw.find(
        "const bool motionHistoryReady = h.mv_computed;") !=
                std::string::npos);
    EXPECT_TRUE(draw.find(
        "motionHistoryReady ? h.prev_vp_nojit : vp_nojit") !=
                std::string::npos);
    EXPECT_TRUE(draw.find(
        "motionHistoryReady && er != nullptr &&") !=
                std::string::npos);
    EXPECT_TRUE(draw.find(
        "const bool recorded =") != std::string::npos);
    EXPECT_TRUE(draw.find(
        "if (recorded && er != nullptr)") != std::string::npos);
    EXPECT_TRUE(draw.find(
        "h.mv3d.ObjectDrawCount() == motionEligibleCount") !=
                std::string::npos);
    EXPECT_TRUE(CountOccurrences(
        draw, "invalidateMotionHistory();") >= 3u);
    EXPECT_TRUE(CountOccurrences(
        draw, "prev_world_valid = false;") >= 2u);
    EXPECT_TRUE(draw.find(
        "h.mv3d.Begin(*cl, vp_nojit, previousVp)") !=
                std::string::npos);

    const std::string clear_scene = ExtractFunction(
        editor,
        "void ClearScene3DResourcesRetired(FEditorHost& h) noexcept");
    const std::string set_view = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_set_view3d(void* handle, int on)");
    const std::string set_projection = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_set_ortho3d(void* handle, int on)");
    const std::string set_game_view = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_set_game_view(void* handle, int on)");
    const std::string restore_play = ExtractFunction(
        editor,
        "static void RestorePlayEditorCamera(FEditorHost& h) noexcept");
    const std::string apply_settings = ExtractFunction(
        editor,
        "static void ApplySettings(FEditorHost& h) noexcept");
    const std::string invalidate_temporal = ExtractFunction(
        editor,
        "void InvalidateTemporalRenderHistories(FEditorHost& h) noexcept");
    EXPECT_TRUE(!invalidate_temporal.empty());
    EXPECT_TRUE(invalidate_temporal.find(
        "h.mv_computed = false;") != std::string::npos);
    EXPECT_TRUE(invalidate_temporal.find(
        "h.taa_frame = 0u;") != std::string::npos);
    EXPECT_TRUE(invalidate_temporal.find(
        "h.post3d.InvalidateTaaHistory();") != std::string::npos);
    EXPECT_TRUE(invalidate_temporal.find(
        "h.post3d.InvalidateExposureHistory();") !=
        std::string::npos);
    EXPECT_TRUE(invalidate_temporal.find(
        "h.ssr3d.InvalidateHistory();") != std::string::npos);
    EXPECT_TRUE(invalidate_temporal.find(
        "h.ssgi3d.InvalidateHistory();") != std::string::npos);
    EXPECT_TRUE(invalidate_temporal.find(
        "h.vclouds3d.InvalidateHistory();") != std::string::npos);
    EXPECT_TRUE(invalidate_temporal.find(
        "h.ssr_computed = false;") != std::string::npos);
    EXPECT_TRUE(invalidate_temporal.find(
        "h.ssgi_computed = false;") != std::string::npos);
    EXPECT_TRUE(invalidate_temporal.find(
        "h.temporal_camera_pose_valid = false;") !=
        std::string::npos);

    EXPECT_TRUE(clear_scene.find(
        "InvalidateTemporalRenderHistories(h);") != std::string::npos);
    EXPECT_TRUE(set_view.find(
        "host->view3d != view3d") != std::string::npos);
    EXPECT_TRUE(set_view.find(
        "InvalidateTemporalRenderHistories(*host);") != std::string::npos);
    EXPECT_TRUE(set_projection.find(
        "host->ortho3d != ortho3d") != std::string::npos);
    EXPECT_TRUE(set_projection.find(
        "InvalidateTemporalRenderHistories(*host);") != std::string::npos);
    EXPECT_TRUE(set_game_view.find(
        "host->game_view != gameView") != std::string::npos);
    EXPECT_TRUE(set_game_view.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(restore_play.find(
        "InvalidateTemporalRenderHistories(h);") != std::string::npos);
    EXPECT_TRUE(apply_settings.find(
        "taa_was_requested != h.q_taa_on") != std::string::npos);
    EXPECT_TRUE(apply_settings.find(
        "ssr_was_requested != h.q_ssr_on") != std::string::npos);
    EXPECT_TRUE(apply_settings.find(
        "ssgi_was_requested != h.q_ssgi_on") != std::string::npos);
    EXPECT_TRUE(apply_settings.find(
        "auto_exposure_was_requested != h.q_auto_exposure") !=
        std::string::npos);
    EXPECT_TRUE(apply_settings.find(
        "InvalidateTemporalRenderHistories(h);") != std::string::npos);

    const std::size_t settings_capture =
        apply_settings.find("const bool taa_was_requested");
    const std::size_t settings_ssr_capture =
        apply_settings.find("const bool ssr_was_requested");
    const std::size_t settings_ssgi_capture =
        apply_settings.find("const bool ssgi_was_requested");
    const std::size_t settings_exposure_capture =
        apply_settings.find("const bool auto_exposure_was_requested");
    const std::size_t settings_preset =
        apply_settings.find("ApplyQualityPreset(");
    const std::size_t settings_taa_override =
        apply_settings.find("h.q_taa_on = (taa > 0.0f)");
    const std::size_t settings_ssr_override =
        apply_settings.find("h.q_ssr_on = (ri > 0.0f)");
    const std::size_t settings_ssgi_override =
        apply_settings.find("h.q_ssgi_on = (gi > 0.0f)");
    const std::size_t settings_exposure_override =
        apply_settings.find("h.q_auto_exposure =");
    const std::size_t settings_transition =
        apply_settings.find("taa_was_requested != h.q_taa_on");
    const std::size_t settings_invalidation =
        apply_settings.find(
            "InvalidateTemporalRenderHistories(h);",
            settings_transition);
    EXPECT_TRUE(settings_capture != std::string::npos);
    EXPECT_TRUE(settings_ssr_capture != std::string::npos);
    EXPECT_TRUE(settings_ssgi_capture != std::string::npos);
    EXPECT_TRUE(settings_exposure_capture != std::string::npos);
    EXPECT_TRUE(settings_preset != std::string::npos);
    EXPECT_TRUE(settings_taa_override != std::string::npos);
    EXPECT_TRUE(settings_ssr_override != std::string::npos);
    EXPECT_TRUE(settings_ssgi_override != std::string::npos);
    EXPECT_TRUE(settings_exposure_override != std::string::npos);
    EXPECT_TRUE(settings_transition != std::string::npos);
    EXPECT_TRUE(settings_invalidation != std::string::npos);
    EXPECT_TRUE(settings_capture < settings_preset);
    EXPECT_TRUE(settings_ssr_capture < settings_preset);
    EXPECT_TRUE(settings_ssgi_capture < settings_preset);
    EXPECT_TRUE(settings_exposure_capture < settings_preset);
    EXPECT_TRUE(settings_taa_override < settings_transition);
    EXPECT_TRUE(settings_ssr_override < settings_transition);
    EXPECT_TRUE(settings_ssgi_override < settings_transition);
    EXPECT_TRUE(settings_exposure_override < settings_transition);
    EXPECT_TRUE(settings_transition < settings_invalidation);

    // Switching the physical presenter between logical cameras invalidates
    // both the effect-internal histories and the host's published-output
    // flags. The opaque pass runs before the current frame's SSR/SSGI
    // dispatches, so leaving either flag set would expose one frame from the
    // previous camera.
    const std::size_t projection_change_detection = draw.find(
        "const bool render_projection_changed =");
    const std::size_t camera_history_reset = draw.find(
        "if (render_camera_changed || camera_view_history_reset ||");
    EXPECT_TRUE(projection_change_detection != std::string::npos);
    EXPECT_TRUE(draw.find(
        "h.last_render_camera_orthographic != renderOrtho") !=
        std::string::npos);
    EXPECT_TRUE(draw.find(
        "const bool render_camera_cut =") != std::string::npos);
    EXPECT_TRUE(draw.find(
        "VolumetricCloudViewCutDetected(") != std::string::npos);
    EXPECT_TRUE(draw.find(
        "render_projection_changed || render_camera_cut") !=
        std::string::npos);
    EXPECT_TRUE(camera_history_reset != std::string::npos);
    EXPECT_TRUE(projection_change_detection < camera_history_reset);
    if (camera_history_reset != std::string::npos) {
        const std::string reset_block =
            draw.substr(camera_history_reset, 900u);
        EXPECT_TRUE(reset_block.find(
            "render_projection_changed") != std::string::npos);
        EXPECT_TRUE(reset_block.find(
            "InvalidateTemporalRenderHistories(h);") !=
                std::string::npos);
    }
    EXPECT_TRUE(CountOccurrences(
        draw, "h.ssr3d.InvalidateHistory();") >= 1u);
    EXPECT_TRUE(CountOccurrences(
        draw, "h.ssgi3d.InvalidateHistory();") >= 1u);
    const std::size_t ssr_render_gate = draw.find(
        "if (h.q_ssr_on && h.ssr_ready && gbufReady)");
    const std::size_t ssr_skipped_invalidate = draw.find(
        "h.ssr3d.InvalidateHistory();", ssr_render_gate);
    const std::size_t ssgi_render_gate = draw.find(
        "if (h.q_ssgi_on && h.ssgi_ready && gbufReady)");
    const std::size_t ssgi_skipped_invalidate = draw.find(
        "h.ssgi3d.InvalidateHistory();", ssgi_render_gate);
    EXPECT_TRUE(ssr_render_gate != std::string::npos);
    EXPECT_TRUE(ssr_skipped_invalidate != std::string::npos);
    EXPECT_TRUE(ssgi_render_gate != std::string::npos);
    EXPECT_TRUE(ssgi_skipped_invalidate != std::string::npos);
    EXPECT_TRUE(ssr_render_gate < ssr_skipped_invalidate);
    EXPECT_TRUE(ssr_skipped_invalidate < ssgi_render_gate);
    EXPECT_TRUE(ssgi_render_gate < ssgi_skipped_invalidate);
    const std::size_t camera_reset_call = draw.find(
        "InvalidateTemporalRenderHistories(h);", camera_history_reset);
    const std::size_t publish_ssr = draw.find("h.pbr3d.SetSsr(");
    const std::size_t publish_ssgi = draw.find("h.pbr3d.SetSsgi(");
    EXPECT_TRUE(camera_reset_call != std::string::npos);
    EXPECT_TRUE(publish_ssr != std::string::npos);
    EXPECT_TRUE(publish_ssgi != std::string::npos);
    EXPECT_TRUE(camera_reset_call < publish_ssr);
    EXPECT_TRUE(camera_reset_call < publish_ssgi);
    EXPECT_TRUE(draw.find(
        "h.prev_temporal_camera_eye = eye;") != std::string::npos);
    EXPECT_TRUE(draw.find(
        "h.temporal_camera_pose_valid = true;") != std::string::npos);
    EXPECT_TRUE(draw.find(
        "h.ssr_computed = h.ssr3d.HasValidOutput();") !=
        std::string::npos);
    EXPECT_TRUE(draw.find(
        "h.ssgi_computed = h.ssgi3d.HasValidOutput();") !=
        std::string::npos);

    const std::string camera_reset = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_camera_reset(void* handle)");
    const std::string camera3d_set = ExtractFunction(
        editor,
        "ACS_EDITOR_API int acs_editor_camera3d_set(");
    const std::string camera3d_reset = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_cam3d_reset(void* handle)");
    const std::string camera_focus = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_camera_focus(void* handle)");
    const std::string camera_frame_all = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_camera_frame_all(void* handle)");
    const std::string camera_pan = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_camera_pan(void* handle");
    const std::string camera_move = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_camera_move(void* handle");
    const std::string camera_zoom = ExtractFunction(
        editor,
        "ACS_EDITOR_API void acs_editor_camera_zoom(void* handle");
    const std::string current_render_camera =
        ExtractFunction(
            editor,
            "bool IsCurrentTemporalRenderCamera3D(");
    const std::string transform_affects_camera =
        ExtractFunction(
            editor,
            "bool TransformAffectsCurrentTemporalRenderCamera3D(");
    const std::string authored_camera_set =
        ExtractFunction(
            editor,
            "ACS_EDITOR_API int acs_editor_node3d_camera_set(");
    const std::string authored_camera_clear =
        ExtractFunction(
            editor,
            "ACS_EDITOR_API int acs_editor_node3d_camera_clear(");
    const std::string node_transform_set =
        ExtractFunction(
            editor,
            "ACS_EDITOR_API int acs_editor_node3d_set_transform(");
    const std::string node_transform_set_masked =
        ExtractFunction(
            editor,
            "ACS_EDITOR_API int acs_editor_node3d_set_transform_masked(");
    const std::string node_transform_set_masked_impl =
        ExtractFunction(
            editor,
            "[[nodiscard]] int SetNode3DTransformMasked(");
    const std::string align_camera_to_view =
        ExtractFunction(
            editor,
            "bool AlignSceneCameraNodeToView(");
    const std::string gizmo3d_drag =
        ExtractFunction(
            editor,
            "ACS_EDITOR_API void acs_editor_gizmo3d_drag(");
    const std::string reparent3d =
        ExtractFunction(
            editor,
            "ACS_EDITOR_API int acs_editor_reparent3d(");
    EXPECT_TRUE(camera_reset.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(camera3d_set.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(camera3d_reset.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(camera_focus.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(camera_frame_all.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(camera_pan.find(
        "InvalidateTemporalRenderHistories") == std::string::npos);
    EXPECT_TRUE(camera_move.find(
        "InvalidateTemporalRenderHistories") == std::string::npos);
    EXPECT_TRUE(camera_zoom.find(
        "InvalidateTemporalRenderHistories") == std::string::npos);

    // Authored projection/clip/pose changes are discontinuities even when the
    // same camera id and perspective/ortho mode remain selected. The physical
    // owner's last rendered id covers active, legacy preview, and request
    // presenter resolution without resetting histories for unrelated cameras
    // or normal mesh edits. Parent transforms are included because they alter
    // the authored camera's world pose.
    EXPECT_TRUE(current_render_camera.find(
        "host.game_view") != std::string::npos);
    EXPECT_TRUE(current_render_camera.find(
        "host.last_render_camera_node_id") !=
        std::string::npos);
    EXPECT_TRUE(transform_affects_camera.find(
        "FindNode3DNode(host, host.last_render_camera_node_id)") !=
        std::string::npos);
    EXPECT_TRUE(transform_affects_camera.find(
        "cursor = cursor->Parent()") != std::string::npos);
    EXPECT_TRUE(authored_camera_set.find(
        "const bool was_temporal_owner") != std::string::npos);
    EXPECT_TRUE(authored_camera_set.find(
        "const bool is_temporal_owner") != std::string::npos);
    EXPECT_TRUE(authored_camera_set.find(
        "was_temporal_owner || is_temporal_owner") !=
        std::string::npos);
    EXPECT_TRUE(authored_camera_set.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(authored_camera_clear.find(
        "const bool was_temporal_owner") != std::string::npos);
    EXPECT_TRUE(authored_camera_clear.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(node_transform_set.find(
        "SetNode3DTransformMasked(") !=
        std::string::npos);
    EXPECT_TRUE(node_transform_set_masked.find(
        "SetNode3DTransformMasked(") !=
        std::string::npos);
    EXPECT_TRUE(node_transform_set_masked_impl.find(
        "TransformAffectsCurrentTemporalRenderCamera3D(") !=
        std::string::npos);
    EXPECT_TRUE(node_transform_set_masked_impl.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(align_camera_to_view.find(
        "TransformAffectsCurrentTemporalRenderCamera3D(") !=
        std::string::npos);
    EXPECT_TRUE(align_camera_to_view.find(
        "InvalidateTemporalRenderHistories(host);") !=
        std::string::npos);
    EXPECT_TRUE(gizmo3d_drag.find(
        "TransformAffectsCurrentTemporalRenderCamera3D(") !=
        std::string::npos);
    EXPECT_TRUE(gizmo3d_drag.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
    EXPECT_TRUE(reparent3d.find(
        "TransformAffectsCurrentTemporalRenderCamera3D(") !=
        std::string::npos);
    EXPECT_TRUE(reparent3d.find(
        "InvalidateTemporalRenderHistories(*host);") !=
        std::string::npos);
}

ACS_TEST(PostEffects, MotionVectorContractsPublishOnlyCompleteAuthoritativeOutputs)
{
    const std::string motion_header =
        ReadWorkspaceSource("src/render/MotionVector.h");
    const std::string motion_source =
        ReadWorkspaceSource("src/render/MotionVector.cpp");
    const std::string reference =
        ReadWorkspaceSource("docs/reference/data/render_core.js");

    EXPECT_TRUE(!motion_header.empty());
    EXPECT_TRUE(!motion_source.empty());
    EXPECT_TRUE(!reference.empty());
    if (motion_header.empty() || motion_source.empty() || reference.empty()) {
        return;
    }

    // UINT32_MAX is a reserved cursor value, never an allocation request, and
    // DrawMesh checks it before evaluating cursor + 1.
    EXPECT_TRUE(motion_header.find(
        "kInvalidObjectBuffer = ~u32{0}") != std::string::npos);
    EXPECT_TRUE(motion_source.find(
        "required_draws == kInvalidObjectBuffer") != std::string::npos);
    EXPECT_TRUE(motion_source.find(
        "m_DrawCursor == kInvalidObjectBuffer ||") != std::string::npos);

    // Public examples must teach the same fail-closed lifecycle rather than
    // the former void Begin/Draw API.
    EXPECT_TRUE(reference.find(
        "bool BeginFrame(u32 required_draws = 0)") != std::string::npos);
    EXPECT_TRUE(reference.find(
        "bool Begin(IRhiCommandList& cl") != std::string::npos);
    EXPECT_TRUE(reference.find(
        "bool DrawMesh(IRhiCommandList& cl") != std::string::npos);
    EXPECT_TRUE(reference.find(
        "sig: \"void DrawMesh(IRhiCommandList& cl") ==
                std::string::npos);
}

ACS_TEST(PostEffects,
         LegacyScene3DTransparentHookUsesLoadAndRestoresExternalState)
{
    const std::string adapter =
        ReadWorkspaceSource("src/gameframework/LegacyScene3DAdapter.cpp");
    const std::string header =
        ReadWorkspaceSource("src/gameframework/LegacyScene3DAdapter.h");
    const std::string command_header =
        ReadWorkspaceSource("src/render/IRhiCommandList.h");
    const std::string frame = ExtractFunction(
        adapter, "void ALegacyScene3DAdapter::OnRender(");
    EXPECT_TRUE(!frame.empty());
    EXPECT_TRUE(!header.empty());
    EXPECT_TRUE(!command_header.empty());
    if (frame.empty() || header.empty() || command_header.empty()) return;

    const std::size_t cloud = frame.find("CompositeClouds(");
    const std::size_t load = frame.find(
        "BeginRenderToTextureLoad(*hdr, depth)", cloud);
    const std::size_t hook = frame.find(
        "OnRenderTransparent3D(transparent_context)", load);
    const std::size_t restore = frame.find(
        "RestoreStateAfterExternalCommands()", hook);
    const std::size_t end = frame.find(
        "EndRenderToTexture(*hdr)", restore);
    const std::size_t reflection = frame.find(
        "RenderReflectionPass(", end);
    const std::size_t post = frame.find("m_Post.Render(", reflection);
    EXPECT_TRUE(cloud != std::string::npos);
    EXPECT_TRUE(load != std::string::npos);
    EXPECT_TRUE(hook != std::string::npos);
    EXPECT_TRUE(restore != std::string::npos);
    EXPECT_TRUE(end != std::string::npos);
    EXPECT_TRUE(reflection != std::string::npos);
    EXPECT_TRUE(post != std::string::npos);
    EXPECT_TRUE(frame.find(
        "if (OnRenderTransparent3D(transparent_context))") !=
                std::string::npos);

    EXPECT_TRUE(header.find("struct FScene3DTransparentRenderContext") !=
                std::string::npos);
    EXPECT_TRUE(header.find("IRhiDevice& Device") != std::string::npos);
    EXPECT_TRUE(header.find("IRhiCommandList& Commands") != std::string::npos);
    EXPECT_TRUE(header.find("const CCamera& Camera") != std::string::npos);
    EXPECT_TRUE(header.find("IRhiTexture& ColorTarget") != std::string::npos);
    EXPECT_TRUE(header.find("IRhiTexture* DepthTarget") != std::string::npos);
    EXPECT_TRUE(header.find("u32 Width") != std::string::npos);
    EXPECT_TRUE(header.find("u32 Height") != std::string::npos);
    EXPECT_TRUE(header.find(
        "virtual bool OnRenderTransparent3D(") != std::string::npos);
    EXPECT_TRUE(header.find("(void)context;") != std::string::npos);
    EXPECT_TRUE(header.find("return false;") != std::string::npos);

    const std::size_t statistics_virtual = command_header.find(
        "virtual FRhiCommandStatistics& StatisticsStorage() noexcept = 0;");
    const std::size_t public_after_statistics = command_header.find(
        "public:", statistics_virtual);
    const std::size_t interop_virtual = command_header.find(
        "virtual void* D3D12GraphicsCommandList() noexcept", statistics_virtual);
    EXPECT_TRUE(statistics_virtual != std::string::npos);
    EXPECT_TRUE(public_after_statistics != std::string::npos);
    EXPECT_TRUE(interop_virtual != std::string::npos);
    EXPECT_TRUE(statistics_virtual < public_after_statistics);
    EXPECT_TRUE(public_after_statistics < interop_virtual);

    const std::string diligent_command_list =
        ReadWorkspaceSource("src/render/Diligent/DiligentCommandList.cpp");
    const std::size_t restore_function = diligent_command_list.find(
        "void CDiligentCommandList::RestoreStateAfterExternalCommands() noexcept");
    const std::size_t flush = diligent_command_list.find(
        "context->Flush();", restore_function);
    const std::size_t invalidate = diligent_command_list.find(
        "context->InvalidateState();", restore_function);
    EXPECT_TRUE(!diligent_command_list.empty());
    EXPECT_TRUE(restore_function != std::string::npos);
    EXPECT_TRUE(flush != std::string::npos);
    EXPECT_TRUE(invalidate != std::string::npos);
    EXPECT_TRUE(flush < invalidate);
}

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

ACS_TEST(PostEffects,
         EditorFallbackShadersUseFxcSafeControlFlowAndExplicitLod)
{
    const std::string source =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string mesh =
        ExtractRawShader(source, "kMesh3DHLSL");
    const std::string dof =
        ExtractRawShader(source, "kDof3DHLSL");
    const std::string motion =
        ExtractRawShader(source, "kMotionBlur3DHLSL");

    EXPECT_TRUE(!mesh.empty());
    EXPECT_TRUE(!dof.empty());
    EXPECT_TRUE(!motion.empty());
    if (mesh.empty() || dof.empty() || motion.empty()) return;

    const std::string sky =
        ExtractFunction(mesh, "float3 SkyCol(float3 d)");
    const std::string shadow =
        ExtractFunction(
            mesh, "float ShadowFactor(float3 wpos, float ndl)");
    EXPECT_TRUE(!sky.empty());
    EXPECT_TRUE(!shadow.empty());
    EXPECT_EQ(
        CountOccurrences(sky, "return "),
        static_cast<std::size_t>(1u));
    EXPECT_EQ(
        CountOccurrences(shadow, "return "),
        static_cast<std::size_t>(1u));
    EXPECT_TRUE(
        sky.find("float3 sky_color = lerp(") != std::string::npos);
    EXPECT_TRUE(
        shadow.find("float shadow_factor = 1.0;") !=
        std::string::npos);
    EXPECT_TRUE(
        shadow.find("bool projection_valid = abs(lp.w) > 1.0e-5;") !=
        std::string::npos);

    // Diligent combined samplers pair by <texture>_sampler.  Using the
    // matching name avoids an unbound sampler without changing the PCF taps.
    EXPECT_TRUE(
        mesh.find("SamplerState shadow_map_sampler") !=
        std::string::npos);
    EXPECT_TRUE(mesh.find("shadow_samp") == std::string::npos);

    // Screen-space gathers intentionally read the full-resolution level.
    // Explicit LOD removes undefined implicit derivatives inside rolled loops
    // while preserving every tap, coordinate and accumulation order.
    EXPECT_EQ(
        CountOccurrences(dof, ".Sample("),
        static_cast<std::size_t>(0u));
    EXPECT_EQ(
        CountOccurrences(motion, ".Sample("),
        static_cast<std::size_t>(0u));
    EXPECT_TRUE(
        dof.find("depthTex.SampleLevel(depthTex_sampler, uv, 0.0)") !=
        std::string::npos);
    EXPECT_TRUE(
        dof.find("sceneTex.SampleLevel(\n"
                 "                sceneTex_sampler, sampleUv, 0.0)") !=
        std::string::npos);
    EXPECT_TRUE(
        motion.find(
            "motionTex.SampleLevel(\n"
            "            motionTex_sampler, sampleUv, 0.0)") !=
        std::string::npos);
    EXPECT_TRUE(
        motion.find("sceneTex.SampleLevel(\n"
                    "            sceneTex_sampler, sampleUv, 0.0)") !=
        std::string::npos);
}

ACS_TEST(PostEffects, AuthoringParamsRejectNonFiniteAndInvalidRanges)
{
    FPostProcessParams params{};
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    params.bloom_threshold = nan;
    params.bloom_intensity = -3.0f;
    params.bloom_radius = infinity;
    params.bloom_scatter = 4.0f;
    params.exposure = infinity;
    params.gamma = nan;
    params.tonemap_kind = 99;
    params.chromatic_aberration = -1.0f;
    params.cg_lift = FVec3{nan, -5.0f, 5.0f};
    params.cg_gain = FVec3{-1.0f, infinity, 20.0f};
    params.taa_blend_factor = 3.0f;
    params.auto_exposure_min = 10.0f;
    params.auto_exposure_max = -2.0f;
    params.delta_time = nan;
    params.taa_view_proj_no_jitter.m[0][0] = nan;
    params.taa_prev_view_proj_no_jitter = FMat4{
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0};
    params.taa_camera_position = FVec3{nan, infinity, -infinity};
    params.Sanitize();

    EXPECT_NEAR(params.bloom_threshold, 1.0f, 1e-6f);
    EXPECT_NEAR(params.bloom_intensity, 0.0f, 1e-6f);
    EXPECT_NEAR(params.bloom_radius, 1.0f, 1e-6f);
    EXPECT_NEAR(params.bloom_scatter, 1.0f, 1e-6f);
    EXPECT_NEAR(params.exposure, 1.0f, 1e-6f);
    EXPECT_NEAR(params.gamma, 2.2f, 1e-6f);
    EXPECT_EQ(params.tonemap_kind, 0);
    EXPECT_NEAR(params.chromatic_aberration, 0.0f, 1e-6f);
    EXPECT_TRUE(std::isfinite(params.cg_lift.x));
    EXPECT_NEAR(params.cg_lift.y, -2.0f, 1e-6f);
    EXPECT_NEAR(params.cg_lift.z, 2.0f, 1e-6f);
    EXPECT_NEAR(params.cg_gain.x, 0.0f, 1e-6f);
    EXPECT_TRUE(std::isfinite(params.cg_gain.y));
    EXPECT_NEAR(params.cg_gain.z, 8.0f, 1e-6f);
    EXPECT_NEAR(params.taa_blend_factor, 1.0f, 1e-6f);
    EXPECT_NEAR(params.auto_exposure_min, 10.0f, 1e-6f);
    EXPECT_NEAR(params.auto_exposure_max, 10.0f, 1e-6f);
    EXPECT_TRUE(std::isfinite(params.delta_time));
    EXPECT_NEAR(params.taa_view_proj_no_jitter.m[0][0], 1.0f, 1e-6f);
    EXPECT_NEAR(params.taa_prev_view_proj_no_jitter.m[0][0], 1.0f, 1e-6f);
    EXPECT_NEAR(params.taa_prev_view_proj_no_jitter.m[1][1], 1.0f, 1e-6f);
    EXPECT_NEAR(params.taa_prev_view_proj_no_jitter.m[2][2], 1.0f, 1e-6f);
    EXPECT_NEAR(params.taa_prev_view_proj_no_jitter.m[3][3], 1.0f, 1e-6f);
    EXPECT_NEAR(params.taa_camera_position.x, 0.0f, 1e-6f);
    EXPECT_NEAR(params.taa_camera_position.y, 0.0f, 1e-6f);
    EXPECT_NEAR(params.taa_camera_position.z, 0.0f, 1e-6f);
}

ACS_TEST(PostEffects, WaterAndBloomShadersKeepPhysicalSafetyContracts)
{
    const std::string water_source =
        ReadWorkspaceSource("src/render/WaterSurface3D.cpp");
    const std::string post_source =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    const std::string water =
        ExtractRawShader(water_source, "const char* kWaterSurface3DHlsl");
    const std::string bloom =
        ExtractRawShader(post_source, "const char* kExtractPS");
    EXPECT_TRUE(!water.empty());
    EXPECT_TRUE(!bloom.empty());
    if (water.empty() || bloom.empty()) return;

    EXPECT_TRUE(water.find("abs(radial) > sigma * 3.75") !=
                std::string::npos);
    EXPECT_TRUE(water.find(
        "input.normal.x * normal_row0.x") != std::string::npos);
    EXPECT_TRUE(water.find(
        "world.xyz += base_normal * (ambient_height + ripple_height);") !=
                std::string::npos);
    EXPECT_TRUE(water.find(
        "EvaluateNormalMap(input.surface_position") != std::string::npos);
    EXPECT_TRUE(water.find(
        "input.world_normal") != std::string::npos);
    EXPECT_TRUE(water.find(
        "tangent * (micro_slope.x * normal_strength)") !=
                std::string::npos);
    EXPECT_TRUE(water.find(
        "bitangent * (micro_slope.y * normal_strength)") !=
                std::string::npos);
    EXPECT_TRUE(water.find("float3 PerturbWaterNormal(") !=
                std::string::npos);
    EXPECT_TRUE(water_source.find(
        "BuildWaterSurfaceFrame(model)") != std::string::npos);
    EXPECT_TRUE(water_source.find(
        "MakeSafeNormalMatrix(model)") != std::string::npos);
    EXPECT_TRUE(water.find("float3 extinction = absorption + scattering;") !=
                std::string::npos);
    EXPECT_TRUE(water.find("float phase = (1.0 - phase_g * phase_g)") !=
                std::string::npos);
    EXPECT_TRUE(water.find("float3 direct_inscatter = sun_color.rgb") !=
                std::string::npos);

    EXPECT_TRUE(bloom.find("all(abs(color) < 1.0e30)") !=
                std::string::npos);
    EXPECT_TRUE(bloom.find("min(src.SampleLevel") == std::string::npos);
    EXPECT_TRUE(bloom.find("Karis weighting") != std::string::npos);

    const std::string tonemap =
        ExtractRawShader(post_source, "const char* kTonemapPS");
    EXPECT_TRUE(!tonemap.empty());
    EXPECT_TRUE(tonemap.find(
        "min(SafeHdr(hdr_col + bloom_col + ssr_col), 65504.0)") !=
        std::string::npos);
    EXPECT_TRUE(tonemap.find(
        "return all(abs(color) < 1.0e30) ? max(color, 0.0) : 0.0;") !=
                std::string::npos);
    EXPECT_TRUE(tonemap.find(
        "mapped = pow(max(mapped, 0.0), 2.2 / max(params1.x, 1.0));") !=
                std::string::npos);
    EXPECT_TRUE(water.find("float safe_w = abs(world.w) > 1e-6") !=
                std::string::npos);
    EXPECT_TRUE(water_source.find("if (IsFinite(view_projection))") !=
                std::string::npos);
    EXPECT_TRUE(water_source.find(
        "m_SunColor = ClampFinite(sun_color, m_SunColor, 0.0f, 65504.0f);") !=
                std::string::npos);
    EXPECT_TRUE(water_source.find(
        "m_ShadowPcfRadius = ClampFinite(pcf_radius, 0.0f, 0.0f, 8.0f);") !=
                std::string::npos);
}

ACS_TEST(PostEffects,
         WaterAndSubsurfaceShadersUseFxcSafeSingleExitHelpers)
{
    const std::string water_source =
        ReadWorkspaceSource("src/render/WaterSurface3D.cpp");
    const std::string subsurface_source =
        ReadWorkspaceSource("src/render/SubsurfaceScattering.cpp");
    const std::string water =
        ExtractRawShader(water_source, "const char* kWaterSurface3DHlsl");
    const std::string subsurface = ExtractRawShader(
        subsurface_source, "const char* kSubsurfaceScatteringHlsl");
    EXPECT_TRUE(!water.empty());
    EXPECT_TRUE(!subsurface.empty());
    if (water.empty() || subsurface.empty()) return;

    const std::string authored_normal =
        ExtractFunction(water, "float3 EvaluateAuthoredNormal(");
    const std::string perturbed_normal =
        ExtractFunction(water, "float3 PerturbWaterNormal(");
    const std::string projected_direction =
        ExtractFunction(
            water, "float2 ProjectWorldDirectionToScreenPixels(");
    const std::string profile_radii =
        ExtractFunction(subsurface, "float3 ResolveProfileRadii(");
    const std::string profile_similarity =
        ExtractFunction(subsurface, "float ProfileSimilarity(");
    const std::string blur_diffuse =
        ExtractFunction(subsurface, "float4 BlurDiffuse(");

    for (const std::string* helper :
         {&authored_normal, &perturbed_normal, &projected_direction,
          &profile_radii, &profile_similarity, &blur_diffuse}) {
        EXPECT_TRUE(!helper->empty());
        EXPECT_EQ(
            CountOccurrences(*helper, "return "),
            static_cast<std::size_t>(1u));
    }

    EXPECT_TRUE(authored_normal.find(
        "float3 authored_result = float3(0.0, 0.0, 1.0);") !=
                std::string::npos);
    EXPECT_TRUE(perturbed_normal.find(
        "float3 perturbed_result = world_normal;") !=
                std::string::npos);
    EXPECT_TRUE(projected_direction.find(
        "float2 projected_result = float2(0.0, 0.0);") !=
                std::string::npos);
    EXPECT_TRUE(profile_radii.find(
        "float3 resolved_radii = authored;") != std::string::npos);
    EXPECT_TRUE(profile_similarity.find(
        "float similarity = 0.0;") != std::string::npos);
    EXPECT_TRUE(blur_diffuse.find(
        "float4 blur_result = center_diffuse;") != std::string::npos);

    // Warning cleanup must not trade away water texture filtering or the
    // separable SSS profile quality. Keep every authored-normal sample and
    // every bilateral tap with explicit full-resolution LOD in the gather.
    EXPECT_EQ(
        CountOccurrences(authored_normal, "authored_normal.Sample("),
        static_cast<std::size_t>(1u));
    EXPECT_TRUE(blur_diffuse.find("static const float kOffsets[6]") !=
                std::string::npos);
    EXPECT_TRUE(blur_diffuse.find(
        "for (int tap = 0; tap < 6; ++tap)") != std::string::npos);
    EXPECT_TRUE(blur_diffuse.find(
        "for (int side = 0; side < 2; ++side)") !=
                std::string::npos);
    EXPECT_EQ(
        CountOccurrences(blur_diffuse, "[unroll]"),
        static_cast<std::size_t>(2u));
    EXPECT_TRUE(blur_diffuse.find("ProfileWeight(") !=
                std::string::npos);
    EXPECT_TRUE(blur_diffuse.find(
        "accumulated / max(normalization, 1e-5)") !=
                std::string::npos);
    EXPECT_TRUE(blur_diffuse.find(".Sample(") == std::string::npos);
    EXPECT_TRUE(blur_diffuse.find(".SampleLevel(") !=
                std::string::npos);
}

ACS_TEST(PostEffects, RawDx12PostShadersAvoidFxcIsFiniteMiscompile)
{
    const std::string post_source =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    const std::string bloom =
        ExtractRawShader(post_source, "const char* kExtractPS");
    const std::string taa =
        ExtractRawShader(post_source, "const char* kTaaResolvePS");
    const std::string tonemap =
        ExtractRawShader(post_source, "const char* kTonemapPS");
    EXPECT_TRUE(!bloom.empty());
    EXPECT_TRUE(!taa.empty());
    EXPECT_TRUE(!tonemap.empty());
    if (bloom.empty() || taa.empty() || tonemap.empty()) return;

    // FXC/SM5 can optimize isfinite(float3) to an all-false predicate in a
    // pixel shader on the raw DX12 path, blacking every otherwise valid HDR
    // sample. Ordered comparisons reject NaN/Inf as well and compile reliably.
    for (const std::string* shader : {&bloom, &taa, &tonemap}) {
        EXPECT_TRUE(shader->find("isfinite(") == std::string::npos);
        EXPECT_TRUE(shader->find("all(abs(color) < 1.0e30)") !=
                    std::string::npos);
    }
}

ACS_TEST(PostEffects, CameraFrustumOverlayIsSceneOnlyAndAfterHdrPost)
{
    const std::string source =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string draw = ReadDrawScene3DSource();
    const std::string overlay = ExtractFunction(
        source, "void DrawSelectedCameraFrustumOverlay");
    EXPECT_TRUE(!draw.empty());
    EXPECT_TRUE(!overlay.empty());
    if (draw.empty() || overlay.empty()) return;

    EXPECT_TRUE(overlay.find(
        "if (!host.show_camera_frustum || host.game_view)") !=
        std::string::npos);
    EXPECT_TRUE(draw.find(
        "if (hdrRt == nullptr && !h.game_view)") != std::string::npos);

    const std::size_t post =
        draw.find("h.post3d.Render");
    const std::size_t game_view_gate =
        draw.find(
            "if (!h.game_view && h.show_camera_frustum",
            post);
    const std::size_t swapchain_load =
        draw.find("cl->BeginRenderToSwapchainLoad", game_view_gate);
    const std::size_t frustum_draw =
        draw.find(
            "DrawSelectedCameraFrustumOverlay",
            swapchain_load);
    EXPECT_TRUE(post != std::string::npos);
    EXPECT_TRUE(game_view_gate != std::string::npos);
    EXPECT_TRUE(swapchain_load != std::string::npos);
    EXPECT_TRUE(frustum_draw != std::string::npos);
    EXPECT_TRUE(post < game_view_gate);
    EXPECT_TRUE(game_view_gate < swapchain_load);
    EXPECT_TRUE(swapchain_load < frustum_draw);
    EXPECT_TRUE(draw.find(
        "h, *cl, vp_nojit, aspect", frustum_draw) !=
        std::string::npos);
}

ACS_TEST(PostEffects, EditorFrustumCullingMasksEveryPerNodeGeometryPass)
{
    const std::string source =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string draw = ReadDrawScene3DSource();
    const std::string build_verts = ExtractFunction(
        source, "void BuildSceneMeshVerts");
    const std::string build_visibility = ExtractFunction(
        source, "void BuildSceneMeshVisibility");
    const std::string count_pbr = ExtractFunction(
        source, "FPbrFrameDrawCounts CountPbrFrameDraws");
    const std::string draw_water = ExtractFunction(
        source, "void DrawInteractiveWater3DPass");
    EXPECT_TRUE(!draw.empty());
    EXPECT_TRUE(!build_verts.empty());
    EXPECT_TRUE(!build_visibility.empty());
    EXPECT_TRUE(!count_pbr.empty());
    EXPECT_TRUE(!draw_water.empty());
    if (draw.empty() || build_verts.empty() ||
        build_visibility.empty() || count_pbr.empty() ||
        draw_water.empty())
        return;

    const std::size_t visibility =
        draw.find("BuildSceneMeshVisibility(h, all3d, vp_nojit);");
    const std::size_t pbr_reserve =
        draw.find("CountPbrFrameDraws(h, all3d);");
    EXPECT_TRUE(visibility != std::string::npos);
    EXPECT_TRUE(pbr_reserve != std::string::npos);
    EXPECT_TRUE(visibility < pbr_reserve);

    // Normal/depth records coalesced non-indexed ranges. Motion history,
    // opaque PBR, interactive water and refraction record indexed nodes. All
    // of them resolve the same main-view mask through an explicit production
    // pass identity.
    EXPECT_TRUE(CountOccurrences(
        draw,
        "editor_frustum_culling::ForEachSubmittedNode(") >=
        std::size_t{3});
    EXPECT_TRUE(draw.find(
        "editor_frustum_culling::ForEachSubmittedVertexRange(") !=
        std::string::npos);
    EXPECT_TRUE(count_pbr.find(
        "editor_frustum_culling::ForEachSubmittedNode(") !=
        std::string::npos);
    EXPECT_TRUE(draw.find(
        "editor_frustum_culling::AnySubmittedNode(") !=
        std::string::npos);
    EXPECT_TRUE(CountOccurrences(
        draw, "SceneMeshSubmissionMask(") >= std::size_t{6});
    EXPECT_TRUE(draw.find("NormalDepthPrepass") != std::string::npos);
    EXPECT_TRUE(CountOccurrences(
        draw, "MotionVectors") >= std::size_t{2});
    EXPECT_TRUE(draw.find("PbrOpaqueDraw") != std::string::npos);
    EXPECT_TRUE(draw.find("InteractiveWaterDraw") != std::string::npos);
    EXPECT_TRUE(draw.find("RefractionPreflight") != std::string::npos);
    EXPECT_TRUE(draw.find("RefractionDraw") != std::string::npos);
    EXPECT_TRUE(count_pbr.find("PbrOpaqueCount") != std::string::npos);
    EXPECT_TRUE(CountOccurrences(
        draw_water, "submission_mask.ShouldSubmit(i)") >=
        std::size_t{3});

    // Specialized water is not part of the aggregate opaque VB, but its
    // actual indexed base mesh must still build a sphere and overwrite the
    // default-visible slot. Otherwise every water node remains 1 forever and
    // all water preflight/fallback/specialized loops silently bypass culling.
    const std::size_t water_bounds_source =
        build_verts.find("WaterCpuMeshForNode3D(h, nn)");
    const std::size_t water_radius =
        build_verts.find(
            "h.scene_mesh_local_radius[i] =",
            water_bounds_source);
    const std::size_t water_aggregate_skip =
        build_verts.find("if (interactive_water) continue;");
    EXPECT_TRUE(water_bounds_source != std::string::npos);
    EXPECT_TRUE(water_radius != std::string::npos);
    EXPECT_TRUE(water_aggregate_skip != std::string::npos);
    EXPECT_TRUE(water_bounds_source < water_radius);
    EXPECT_TRUE(water_radius < water_aggregate_skip);

    const std::size_t water_classification =
        build_visibility.find("const bool interactive_water");
    const std::size_t water_gpu_mesh =
        build_visibility.find("? WaterGpuMeshForNode3D(host, node)");
    const std::size_t water_displacement_bound =
        build_visibility.find(
            "ConservativeDisplacementBoundForSurface(");
    const std::size_t sphere_evaluation = build_visibility.find("editor_frustum_culling::EvaluateSpheresBatch(");
    EXPECT_TRUE(build_visibility.find(
        "if (interactive_water) continue;") == std::string::npos);
    EXPECT_TRUE(build_visibility.find(
        "mesh->RenderHandle() != nullptr ||\n"
        "            IsRenderedByWater3D(host, node)") ==
        std::string::npos);
    EXPECT_TRUE(water_classification != std::string::npos);
    EXPECT_TRUE(water_gpu_mesh != std::string::npos);
    EXPECT_TRUE(water_displacement_bound != std::string::npos);
    EXPECT_TRUE(sphere_evaluation != std::string::npos);
    EXPECT_TRUE(water_classification < water_gpu_mesh);
    EXPECT_TRUE(water_gpu_mesh < water_displacement_bound);
    EXPECT_TRUE(water_displacement_bound < sphere_evaluation);

    // A non-indexable aggregate fallback must disable diagnostics instead of
    // claiming that nodes were culled while still issuing one combined draw.
    const std::size_t aggregate =
        draw.find("auto draw_aggregate_mesh_fallback");
    const std::size_t aggregate_draw =
        draw.find("cl->Draw(dvCount, 0);", aggregate);
    EXPECT_TRUE(aggregate != std::string::npos);
    EXPECT_TRUE(aggregate_draw != std::string::npos);
    EXPECT_TRUE(draw.find(
        "h.profiler_work.frustum_culling_enabled = false;",
        aggregate) < aggregate_draw);

    const std::size_t rejected_pool =
        draw.find("if (h.pbr3d_ready && !pbr_object_pool_ready)");
    const std::size_t rejected_return =
        draw.find("return;", rejected_pool);
    EXPECT_TRUE(rejected_pool != std::string::npos);
    EXPECT_TRUE(rejected_return != std::string::npos);
    EXPECT_TRUE(draw.find(
        "h.profiler_work.frustum_culling_enabled = false;",
        rejected_pool) < rejected_return);
    EXPECT_TRUE(draw.find(
        "h.profiler_work.frustum_tested = 0u;",
        rejected_pool) < rejected_return);

    // Any invalid plane/sphere decision is fail-open and clears the partial
    // mask, keeping submitted geometry and published counters truthful.
    EXPECT_TRUE(build_visibility.find(
        "host.scene_mesh_visible[reset] = 1u;") !=
        std::string::npos);
    EXPECT_TRUE(build_visibility.find(
        "host.profiler_work.frustum_culling_enabled =\n"
        "        frame.enabled;") !=
        std::string::npos);
}

ACS_TEST(PostEffects, EditorCompositeOrderKeepsCloudsInRefractionBackground)
{
    const std::string draw = ReadDrawScene3DSource();
    EXPECT_TRUE(!draw.empty());
    if (draw.empty()) return;

    const std::size_t opaque_done =
        draw.find("// HDR RT → CPostProcess");
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

ACS_TEST(PostEffects, AnimatedCloudsUseReactiveMaskWhileGeometryKeepsGlobalTaa)
{
    const std::string draw = ReadDrawScene3DSource();
    const std::string post =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    const std::string cloud =
        ReadWorkspaceSource("src/render/Sky.cpp");
    const std::string cloud_header =
        ReadWorkspaceSource("src/render/Sky.h");
    EXPECT_TRUE(!draw.empty());
    EXPECT_TRUE(!post.empty());
    EXPECT_TRUE(!cloud.empty());
    EXPECT_TRUE(!cloud_header.empty());
    if (draw.empty() || post.empty() || cloud.empty() || cloud_header.empty())
        return;

    EXPECT_TRUE(draw.find("animatedCloudsRequested") == std::string::npos);
    EXPECT_TRUE(draw.find("h.q_taa_on && !renderOrtho") != std::string::npos);
    EXPECT_TRUE(draw.find("Inverse(vp_nojit), eye") != std::string::npos);
    EXPECT_TRUE(draw.find(
        "pp.taa_reactive_texture = h.vclouds3d.ResolvedDepth();") !=
        std::string::npos);
    EXPECT_TRUE(draw.find("pp.taa_camera_position          = eye;") !=
                std::string::npos);

    const std::string taa_shader =
        ExtractRawShader(post, "const char* kTaaResolvePS");
    EXPECT_TRUE(!taa_shader.empty());
    EXPECT_TRUE(taa_shader.find("reactive_mask : register(t3)") !=
                std::string::npos);
    EXPECT_TRUE(taa_shader.find("reactive_scene_depth : register(t4)") !=
                std::string::npos);
    EXPECT_TRUE(taa_shader.find("if (taa_params.w >= 0.5)") !=
                std::string::npos);
    EXPECT_TRUE(taa_shader.find("float SceneDistanceAt(float2 uv)") !=
                std::string::npos);
    const std::string scene_distance =
        ExtractFunction(taa_shader, "float SceneDistanceAt(float2 uv)");
    EXPECT_TRUE(scene_distance.find("float sceneDistance = 1e30;") !=
                std::string::npos);
    EXPECT_TRUE(scene_distance.find("return sceneDistance;") !=
                std::string::npos);
    EXPECT_EQ(CountOccurrences(scene_distance, "return "), std::size_t{1});
    EXPECT_TRUE(taa_shader.find(
        "reactiveHit.x < sceneDistance - tolerance") != std::string::npos);
    EXPECT_TRUE(taa_shader.find(
        "if (sceneDistance > 250000.0)") != std::string::npos);
    EXPECT_TRUE(taa_shader.find("for (int ry = -1; ry <= 1; ++ry)") !=
                std::string::npos);
    EXPECT_TRUE(taa_shader.find("if (rx == 0 && ry == 0)") !=
                std::string::npos);
    EXPECT_TRUE(taa_shader.find(
        "smoothstep(0.001, 0.02, reactive)") != std::string::npos);
    EXPECT_TRUE(post.find("pd.texture_slots = 5;") != std::string::npos);
    EXPECT_TRUE(post.find("cmd.SetTexture(3, *reactive_tex);") !=
                std::string::npos);
    EXPECT_TRUE(post.find("cmd.SetTexture(4, *reactive_depth);") !=
                std::string::npos);

    // ResolvedDepth is a full-resolution, same-frame RG32F pair where
    // R=ray distance and G=resolved alpha. Keep this producer contract tied to
    // the TAA visibility gate so a format/lifetime change cannot silently turn
    // foreground terrain current-only again.
    EXPECT_TRUE(cloud.find(
        "make_texture(fw, fh, EFormat::R32G32_Float, true, false, historyDepth[0])") !=
        std::string::npos);
    EXPECT_TRUE(cloud.find("resolvedDepth.y=outA;") != std::string::npos);
    const std::size_t publish_index = cloud.find("m_ResolvedIndex = cur;");
    const std::size_t publish_valid = cloud.find(
        "m_HistoryValid = true;", publish_index);
    EXPECT_TRUE(publish_index != std::string::npos);
    EXPECT_TRUE(publish_valid != std::string::npos);
    EXPECT_TRUE(publish_index < publish_valid);
    EXPECT_TRUE(cloud_header.find(
        "return m_HistoryValid ? m_HistoryDepth[m_ResolvedIndex].Get() : nullptr;") !=
        std::string::npos);

    const std::size_t taa_gate =
        post.find("if (safe_params.taa_enabled) {");
    const std::size_t disabled_history_reset =
        post.find("m_TaaFrame = 0;", taa_gate);
    const std::size_t bloom_stage =
        post.find("if (safe_params.bloom_enabled", disabled_history_reset);
    EXPECT_TRUE(taa_gate != std::string::npos);
    EXPECT_TRUE(disabled_history_reset != std::string::npos);
    EXPECT_TRUE(bloom_stage != std::string::npos);
    EXPECT_TRUE(taa_gate < disabled_history_reset);
    EXPECT_TRUE(disabled_history_reset < bloom_stage);
}

ACS_TEST(PostEffects, EditorGridFadesSubpixelFrequenciesWithoutHardHorizonCutoff)
{
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    EXPECT_TRUE(!editor.empty());
    if (editor.empty()) return;

    const std::string grid =
        ExtractRawShader(editor, "const char* kGrid3DHLSL");
    EXPECT_TRUE(!grid.empty());
    EXPECT_TRUE(grid.find(
        "float2 frequencyFade = 1.0 - smoothstep(0.25, 0.50, footprint);") !=
                std::string::npos);
    EXPECT_TRUE(grid.find("lineCoverage.x * frequencyFade.x") !=
                std::string::npos);
    EXPECT_TRUE(grid.find("lineCoverage.y * frequencyFade.y") !=
                std::string::npos);
    EXPECT_TRUE(grid.find("saturate(a * fade)") != std::string::npos);
    EXPECT_TRUE(grid.find("if (a < 0.004) discard;") == std::string::npos);
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
    CRefractionShader refraction;
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

ACS_TEST(PostEffects, RuntimeStartupPacesGpuCommitsAndResizeKeepsStrongGuarantee)
{
    const std::string legacy =
        ReadWorkspaceSource("src/gameframework/LegacyScene3DAdapter.cpp");
    const std::string legacy_header =
        ReadWorkspaceSource("src/gameframework/LegacyScene3DAdapter.h");
    const std::string post =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    EXPECT_TRUE(!legacy.empty());
    EXPECT_TRUE(!legacy_header.empty());
    EXPECT_TRUE(!post.empty());
    if (legacy.empty() || legacy_header.empty() || post.empty()) return;

    EXPECT_TRUE(legacy_header.find("PendingCommit") != std::string::npos);
    EXPECT_TRUE(legacy_header.find("enum class EGpuCommitSubsystem") !=
                std::string::npos);
    EXPECT_TRUE(legacy.find(
        "EGpuCommitSubsystem frame_commit = EGpuCommitSubsystem::None;") !=
        std::string::npos);

    const std::size_t claim_begin =
        legacy.find("bool ALegacyScene3DAdapter::TryClaimGpuCommit(");
    const std::size_t claim_end =
        legacy.find("void ALegacyScene3DAdapter::AdvanceHdrPbrInitialization(",
                    claim_begin);
    EXPECT_TRUE(claim_begin != std::string::npos);
    EXPECT_TRUE(claim_end != std::string::npos);
    if (claim_begin != std::string::npos &&
        claim_end != std::string::npos) {
        const std::string claim =
            legacy.substr(claim_begin, claim_end - claim_begin);
        EXPECT_TRUE(claim.find(
            "if (frame_commit != EGpuCommitSubsystem::None) return false;") !=
            std::string::npos);
    }

    const auto commit_is_claimed = [&legacy](
        const char* function_name,
        const char* next_function_name,
        const char* commit_call) {
        const std::size_t begin = legacy.find(function_name);
        const std::size_t end = legacy.find(next_function_name, begin);
        if (begin == std::string::npos || end == std::string::npos)
            return false;
        const std::string function = legacy.substr(begin, end - begin);
        const std::size_t claim = function.find("TryClaimGpuCommit(");
        const std::size_t commit = function.find(commit_call);
        return claim != std::string::npos &&
               commit != std::string::npos &&
               claim < commit;
    };
    EXPECT_TRUE(commit_is_claimed(
        "void ALegacyScene3DAdapter::AdvanceHdrPbrInitialization(",
        "void ALegacyScene3DAdapter::AdvanceHdrSsssInitialization(",
        "candidate.InitWithCompiledShaders("));
    EXPECT_TRUE(commit_is_claimed(
        "void ALegacyScene3DAdapter::AdvanceHdrSsssInitialization(",
        "void ALegacyScene3DAdapter::AdvanceSubsurfaceInitialization(",
        "candidate.InitWithCompiledShaders("));
    EXPECT_TRUE(commit_is_claimed(
        "void ALegacyScene3DAdapter::AdvanceSubsurfaceInitialization(",
        "void ALegacyScene3DAdapter::EnsureSubsurfaceAuxTargets(",
        "m_Ssss.InitPipelineResourcesWithCompiledShaders("));
    EXPECT_TRUE(commit_is_claimed(
        "void ALegacyScene3DAdapter::AdvancePostInitialization(",
        "void ALegacyScene3DAdapter::AdvanceBlitInitialization(",
        "m_Post.InitWithCompiledShaders("));
    EXPECT_TRUE(commit_is_claimed(
        "void ALegacyScene3DAdapter::AdvanceBlitInitialization(",
        "void ALegacyScene3DAdapter::AdvanceSkyInitialization(",
        "m_Blit.InitWithCompiledShaders("));
    EXPECT_TRUE(commit_is_claimed(
        "void ALegacyScene3DAdapter::AdvanceSkyInitialization(",
        "void ALegacyScene3DAdapter::AdvanceWaterInitialization(",
        "m_Sky.InitWithCompiledShaders("));
    EXPECT_TRUE(commit_is_claimed(
        "void ALegacyScene3DAdapter::AdvanceWaterInitialization(",
        "u32 ALegacyScene3DAdapter::CollectWaterDraws(",
        "m_Water.BeginInitWithCompiledShaders("));

    const std::size_t ssss_targets_begin = legacy.find(
        "void ALegacyScene3DAdapter::EnsureSubsurfaceAuxTargets(");
    const std::size_t ssss_targets_end = legacy.find(
        "void ALegacyScene3DAdapter::AdvancePostInitialization(",
        ssss_targets_begin);
    EXPECT_TRUE(ssss_targets_begin != std::string::npos);
    EXPECT_TRUE(ssss_targets_end != std::string::npos);
    if (ssss_targets_begin != std::string::npos &&
        ssss_targets_end != std::string::npos) {
        const std::string staged = legacy.substr(
            ssss_targets_begin, ssss_targets_end - ssss_targets_begin);
        const std::size_t initial_claim =
            staged.find("TryClaimGpuCommit(");
        const std::size_t internal_pair =
            staged.find("m_Ssss.Resize(width, height)", initial_claim);
        const std::size_t return_after_pair =
            staged.find("return;", internal_pair);
        const std::size_t aux_claim =
            staged.find("TryClaimGpuCommit(", return_after_pair);
        const std::size_t diffuse =
            staged.find("auto diffuse = CreateRhiTexture", aux_claim);
        const std::size_t material =
            staged.find("auto material = CreateRhiTexture", diffuse);
        const std::size_t normal =
            staged.find("auto normal = CreateRhiTexture", material);
        EXPECT_TRUE(initial_claim != std::string::npos);
        EXPECT_TRUE(internal_pair != std::string::npos);
        EXPECT_TRUE(return_after_pair != std::string::npos);
        EXPECT_TRUE(aux_claim != std::string::npos);
        EXPECT_TRUE(diffuse != std::string::npos);
        EXPECT_TRUE(material != std::string::npos);
        EXPECT_TRUE(normal != std::string::npos);
        EXPECT_TRUE(initial_claim < internal_pair);
        EXPECT_TRUE(internal_pair < return_after_pair);
        EXPECT_TRUE(return_after_pair < aux_claim);
        EXPECT_TRUE(aux_claim < diffuse);
        EXPECT_TRUE(diffuse < material);
        EXPECT_TRUE(material < normal);
    }
    const std::size_t water_begin = legacy.find(
        "void ALegacyScene3DAdapter::AdvanceWaterInitialization(");
    const std::size_t water_end = legacy.find(
        "u32 ALegacyScene3DAdapter::CollectWaterDraws(", water_begin);
    if (water_begin != std::string::npos &&
        water_end != std::string::npos) {
        const std::string water =
            legacy.substr(water_begin, water_end - water_begin);
        const std::size_t begin_init =
            water.find("m_Water.BeginInitWithCompiledShaders(");
        const std::size_t buffering =
            water.find("m_WaterGpuState = EWaterGpuState::Buffering;",
                       begin_init);
        const std::size_t return_after_begin =
            water.find("return;", buffering);
        const std::size_t buffered_claim =
            water.find("TryClaimGpuCommit(", return_after_begin);
        const std::size_t advance_buffers =
            water.find("m_Water.AdvanceInitialization(", buffered_claim);
        EXPECT_TRUE(begin_init != std::string::npos);
        EXPECT_TRUE(buffering != std::string::npos);
        EXPECT_TRUE(return_after_begin != std::string::npos);
        EXPECT_TRUE(buffered_claim != std::string::npos);
        EXPECT_TRUE(advance_buffers != std::string::npos);
        EXPECT_TRUE(begin_init < buffering);
        EXPECT_TRUE(buffering < return_after_begin);
        EXPECT_TRUE(return_after_begin < buffered_claim);
        EXPECT_TRUE(buffered_claim < advance_buffers);
    }

    const std::size_t resize_begin =
        post.find("TResult<void> CPostProcess::Resize(");
    const std::size_t resize_end =
        post.find("TResult<void> CPostProcess::CreateRenderTargets(",
                  resize_begin);
    EXPECT_TRUE(resize_begin != std::string::npos);
    EXPECT_TRUE(resize_end != std::string::npos);
    if (resize_begin != std::string::npos &&
        resize_end != std::string::npos) {
        const std::string resize =
            post.substr(resize_begin, resize_end - resize_begin);
        const std::size_t candidate =
            resize.find("CPostProcess candidate;");
        const std::size_t create =
            resize.find("candidate.CreateRenderTargets(");
        const std::size_t publish =
            resize.find("m_HdrRt = Move(candidate.m_HdrRt);");
        const std::size_t reset_taa =
            resize.find("m_TaaFrame  = 0");
        const std::size_t reset_exposure =
            resize.find("m_AutoFrame = 0");
        EXPECT_TRUE(candidate != std::string::npos);
        EXPECT_TRUE(create != std::string::npos);
        EXPECT_TRUE(publish != std::string::npos);
        EXPECT_TRUE(reset_taa != std::string::npos);
        EXPECT_TRUE(reset_exposure != std::string::npos);
        EXPECT_TRUE(candidate < create);
        EXPECT_TRUE(create < publish);
        EXPECT_TRUE(resize.find("m_HdrRt.Reset()") == std::string::npos);
        EXPECT_TRUE(reset_taa > publish);
        EXPECT_TRUE(reset_exposure > publish);
    }

    const std::size_t retry_begin =
        legacy.find("const auto resize = m_Post.Resize(width, height);");
    const std::size_t retry_end =
        legacy.find("m_FrameWidth = width;", retry_begin);
    EXPECT_TRUE(retry_begin != std::string::npos);
    EXPECT_TRUE(retry_end != std::string::npos);
    if (retry_begin != std::string::npos &&
        retry_end != std::string::npos) {
        const std::string failure_path =
            legacy.substr(retry_begin, retry_end - retry_begin);
        EXPECT_TRUE(failure_path.find("m_Post.Shutdown()") ==
                    std::string::npos);
        EXPECT_TRUE(failure_path.find(
            "m_PostGpuState = EShaderGpuState::Failed") ==
            std::string::npos);
    }
}

ACS_TEST(PostEffects, PbrBaseStartupCandidateIsThreadedAndInstrumented)
{
    const std::string pbr_header =
        ReadWorkspaceSource("src/render/PbrShader.h");
    const std::string pbr =
        ReadWorkspaceSource("src/render/PbrShader.cpp");
    const std::string legacy =
        ReadWorkspaceSource("src/gameframework/LegacyScene3DAdapter.cpp");
    const std::string legacy_header =
        ReadWorkspaceSource("src/gameframework/LegacyScene3DAdapter.h");
    const std::string dx12 =
        ReadWorkspaceSource("src/render/Dx12/Dx12Device.cpp");

    EXPECT_TRUE(pbr_header.find(
        "bool include_subsurface_mrt = true") != std::string::npos);
    EXPECT_TRUE(pbr_header.find(
        "BuildInitializedCandidateForRawDx12(") != std::string::npos);
    EXPECT_TRUE(pbr.find(
        "CPbrShader::CompileShadersCpu(bool include_subsurface_mrt)") !=
        std::string::npos);
    EXPECT_TRUE(pbr.find(
        "if (include_subsurface_mrt)") != std::string::npos);
    EXPECT_TRUE(pbr.find(
        "constant_buffers=%.3f ms") != std::string::npos);
    EXPECT_TRUE(pbr.find(
        "fallback_textures=%.3f ms") != std::string::npos);
    EXPECT_TRUE(pbr.find(
        "fallback_resources=%.3f ms") != std::string::npos);
    EXPECT_TRUE(pbr.find("base_pso=%.3f ms") != std::string::npos);
    EXPECT_TRUE(pbr.find("mrt_pso=%.3f ms") != std::string::npos);
    EXPECT_TRUE(pbr.find(
        "CPbrShader::BuildInitializedCandidateForRawDx12(") !=
        std::string::npos);
    EXPECT_TRUE(pbr.find(
        "return InitWithCompiledShadersInternal(") != std::string::npos);

    // Legacy always publishes the base renderer first. SSS material scenes
    // are detected by the unified per-frame feature scan and then upgrade the
    // inactive PBR slot without making the base renderer non-ready.
    EXPECT_TRUE(legacy.find(
        "CPbrShader::CompileShadersCpu(false)") != std::string::npos);
    EXPECT_TRUE(legacy.find(
        "device, false);") != std::string::npos);
    EXPECT_TRUE(legacy.find(
        "ScanSceneRenderFeatures(") != std::string::npos);
    EXPECT_TRUE(legacy.find(
        "scene_features.needs_subsurface_mrt") != std::string::npos);
    EXPECT_TRUE(legacy.find(
        "CPbrShader::CompileShadersCpu(true)") != std::string::npos);
    EXPECT_TRUE(legacy.find(
        "m_HdrSsssGpuState") != std::string::npos);
    EXPECT_TRUE(legacy_header.find(
        "CPbrShader m_HdrShaders[2]") != std::string::npos);

    const std::size_t worker_begin = legacy.find(
        "void ALegacyScene3DAdapter::HdrPbrCpuCompileWorkerEntry(");
    const std::size_t worker_end = legacy.find(
        "void ALegacyScene3DAdapter::PostCpuCompileWorkerEntry(",
        worker_begin);
    EXPECT_TRUE(worker_begin != std::string::npos);
    EXPECT_TRUE(worker_end != std::string::npos);
    if (worker_begin != std::string::npos &&
        worker_end != std::string::npos) {
        const std::string worker =
            legacy.substr(worker_begin, worker_end - worker_begin);
        const std::size_t compile =
            worker.find("CPbrShader::CompileShadersCpu(false)");
        const std::size_t build =
            worker.find(".BuildInitializedCandidateForRawDx12(");
        const std::size_t payload_publish =
            worker.find("m_HdrPendingIsInitialized = succeeded;");
        const std::size_t release_publish =
            worker.find("m_HdrCompileWorkerState.store(");
        EXPECT_TRUE(compile != std::string::npos);
        EXPECT_TRUE(build != std::string::npos);
        EXPECT_TRUE(payload_publish != std::string::npos);
        EXPECT_TRUE(release_publish != std::string::npos);
        EXPECT_TRUE(compile < build);
        EXPECT_TRUE(build < payload_publish);
        EXPECT_TRUE(payload_publish < release_publish);
    }

    const std::size_t advance_begin = legacy.find(
        "void ALegacyScene3DAdapter::AdvanceHdrPbrInitialization(");
    const std::size_t advance_end = legacy.find(
        "void ALegacyScene3DAdapter::AdvancePostInitialization(",
        advance_begin);
    EXPECT_TRUE(advance_begin != std::string::npos);
    EXPECT_TRUE(advance_end != std::string::npos);
    if (advance_begin != std::string::npos &&
        advance_end != std::string::npos) {
        const std::string advance =
            legacy.substr(advance_begin, advance_end - advance_begin);
        const std::size_t acquire =
            advance.find("std::memory_order_acquire");
        const std::size_t join =
            advance.find("m_HdrCompileWorker.Join();", acquire);
        const std::size_t claim =
            advance.find("TryClaimGpuCommit(", join);
        const std::size_t raw_flip =
            advance.find("m_HdrActiveSlot = m_HdrPendingSlot;", claim);
        const std::size_t diligent_candidate =
            advance.find("CPbrShader& candidate", raw_flip);
        const std::size_t diligent_init =
            advance.find("candidate.InitWithCompiledShaders(",
                         diligent_candidate);
        const std::size_t diligent_flip =
            advance.find("m_HdrActiveSlot = m_HdrPendingSlot;",
                         diligent_init);
        EXPECT_TRUE(acquire != std::string::npos);
        EXPECT_TRUE(join != std::string::npos);
        EXPECT_TRUE(claim != std::string::npos);
        EXPECT_TRUE(raw_flip != std::string::npos);
        EXPECT_TRUE(diligent_candidate != std::string::npos);
        EXPECT_TRUE(diligent_init != std::string::npos);
        EXPECT_TRUE(diligent_flip != std::string::npos);
        EXPECT_TRUE(acquire < join);
        EXPECT_TRUE(join < claim);
        EXPECT_TRUE(claim < raw_flip);
        EXPECT_TRUE(raw_flip < diligent_candidate);
        EXPECT_TRUE(diligent_candidate < diligent_init);
        EXPECT_TRUE(diligent_init < diligent_flip);
    }

    const std::size_t release_begin = legacy.find(
        "void ALegacyScene3DAdapter::ReleaseGpu()");
    const std::size_t release_end = legacy.find(
        "void ALegacyScene3DAdapter::JoinCpuCompileWorkers()",
        release_begin);
    EXPECT_TRUE(release_begin != std::string::npos);
    EXPECT_TRUE(release_end != std::string::npos);
    if (release_begin != std::string::npos &&
        release_end != std::string::npos) {
        const std::string release =
            legacy.substr(release_begin, release_end - release_begin);
        EXPECT_TRUE(release.find("m_HdrShaders[0].Shutdown();") !=
                    std::string::npos);
        EXPECT_TRUE(release.find("m_HdrShaders[1].Shutdown();") !=
                    std::string::npos);
    }

    const std::size_t exit_begin = legacy.find(
        "void ALegacyScene3DAdapter::OnExit()");
    const std::size_t exit_end = legacy.find(
        "void ALegacyScene3DAdapter::OnUpdate(", exit_begin);
    EXPECT_TRUE(exit_begin != std::string::npos);
    EXPECT_TRUE(exit_end != std::string::npos);
    if (exit_begin != std::string::npos &&
        exit_end != std::string::npos) {
        const std::string exit =
            legacy.substr(exit_begin, exit_end - exit_begin);
        EXPECT_TRUE(exit.find("DrainAndReleaseGpu();") !=
                    std::string::npos);
    }

    const std::size_t drain_begin = legacy.find(
        "void ALegacyScene3DAdapter::DrainAndReleaseGpu()");
    const std::size_t drain_end = legacy.find(
        "void ALegacyScene3DAdapter::ReleaseGpu()", drain_begin);
    EXPECT_TRUE(drain_begin != std::string::npos);
    EXPECT_TRUE(drain_end != std::string::npos);
    if (drain_begin != std::string::npos &&
        drain_end != std::string::npos) {
        const std::string drain =
            legacy.substr(drain_begin, drain_end - drain_begin);
        const std::size_t join =
            drain.find("JoinCpuCompileWorkers();");
        const std::size_t wait = drain.find("device->WaitIdle();", join);
        const std::size_t release = drain.find("ReleaseGpu();", wait);
        EXPECT_TRUE(join != std::string::npos);
        EXPECT_TRUE(wait != std::string::npos);
        EXPECT_TRUE(release != std::string::npos);
        EXPECT_TRUE(join < wait);
        EXPECT_TRUE(wait < release);
    }

    const auto reload_drains_before_parse = [&legacy](
        const char* begin_name,
        const char* end_name,
        const char* parse_call) {
        const std::size_t begin = legacy.find(begin_name);
        const std::size_t end = legacy.find(end_name, begin);
        if (begin == std::string::npos || end == std::string::npos)
            return false;
        const std::string reload = legacy.substr(begin, end - begin);
        const std::size_t drain =
            reload.find("DrainAndReleaseGpu();");
        const std::size_t parse = reload.find(parse_call, drain);
        return drain != std::string::npos &&
               parse != std::string::npos &&
               drain < parse;
    };
    EXPECT_TRUE(reload_drains_before_parse(
        "FScene3DLoadResult ALegacyScene3DAdapter::LoadFile(",
        "FScene3DLoadResult ALegacyScene3DAdapter::LoadAssetPack(",
        "TryLoadScene3DFile("));
    EXPECT_TRUE(reload_drains_before_parse(
        "FScene3DLoadResult ALegacyScene3DAdapter::LoadAssetPack(",
        "void ALegacyScene3DAdapter::FrameScene(",
        "TryLoadScene3DAssetPack("));

    const std::size_t feature_scan_begin =
        legacy.find("FSceneRenderFeatures ScanSceneRenderFeatures(");
    const std::size_t feature_scan_end =
        legacy.find("} // namespace", feature_scan_begin);
    EXPECT_TRUE(feature_scan_begin != std::string::npos);
    EXPECT_TRUE(feature_scan_end != std::string::npos);
    if (feature_scan_begin != std::string::npos &&
        feature_scan_end != std::string::npos) {
        const std::string feature_scan =
            legacy.substr(feature_scan_begin,
                          feature_scan_end - feature_scan_begin);
        const std::size_t active =
            feature_scan.find("if (!IsEffectivelyActive(*node)) continue;");
        const std::size_t children =
            feature_scan.find("node->ChildCount()");
        EXPECT_TRUE(active != std::string::npos);
        EXPECT_TRUE(children != std::string::npos);
        EXPECT_TRUE(active < children);
        EXPECT_TRUE(feature_scan.find("FindWater(*node)") !=
                    std::string::npos);
        EXPECT_TRUE(feature_scan.find(
            "SubstrateNeedsSubsurfaceMrt(material)") !=
                    std::string::npos);
    }

    // Raw-DX12 descriptor handle math may execute concurrently with slot
    // allocation. It must validate against immutable heap capacity, never an
    // unlocked mutable high-water counter.
    const auto handle_uses_capacity = [&dx12](
        const char* begin_name,
        const char* end_name,
        const char* capacity_name) {
        const std::size_t begin = dx12.find(begin_name);
        const std::size_t end = dx12.find(end_name, begin);
        if (begin == std::string::npos || end == std::string::npos)
            return false;
        const std::string accessor = dx12.substr(begin, end - begin);
        return accessor.find(capacity_name) != std::string::npos &&
               accessor.find("HighWater") == std::string::npos;
    };
    EXPECT_TRUE(legacy.find(
        ".BuildInitializedCandidateForRawDx12(") != std::string::npos);
    EXPECT_TRUE(handle_uses_capacity(
        "CDx12Device::SrvCpuHandle(",
        "CDx12Device::SrvGpuHandle(", "kSrvCapacity"));
    EXPECT_TRUE(handle_uses_capacity(
        "CDx12Device::SrvGpuHandle(",
        "CDx12Device::AllocateDsvSlot(", "kSrvCapacity"));
    EXPECT_TRUE(handle_uses_capacity(
        "CDx12Device::DsvCpuHandle(",
        "CDx12Device::AllocateRtvSlot(", "kDsvCapacity"));
    EXPECT_TRUE(handle_uses_capacity(
        "CDx12Device::RtvCpuHandle(",
        "CDx12Device::Init(", "kRtvCapacity"));
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
            if (owned) CMemorySystem::Shutdown();
        }
    } memory_scope;
    if (CMemorySystem::Get(ESegment::Resource) == nullptr) {
        const auto memory_result = CMemorySystem::Init(CMemorySystem::DefaultConfig());
        EXPECT_TRUE(memory_result.IsOk());
        if (memory_result.IsErr()) return;
        memory_scope.owned = true;
    }

    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return; // Headless CI may not expose a graphics adapter.
    IRhiDevice& device = *device_result.Value();

    CPostProcess post;
    if (device.SupportsAsyncShaderCompilation()) {
        auto compiled =
            CPostProcess::BeginCompileShadersAsync(device);
        EXPECT_TRUE(compiled.IsOk());
        if (compiled.IsOk()) {
            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(30);
            EShaderStatus status = compiled.Value().Status();
            while (status == EShaderStatus::Compiling &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
                status = compiled.Value().Status();
            }
            EXPECT_EQ(status, EShaderStatus::Ready);
            if (status == EShaderStatus::Ready) {
                EXPECT_TRUE(
                    post.InitWithCompiledShaders(
                            device, Move(compiled.Value()), 64, 64,
                            EFormat::B8G8R8A8_UNorm)
                        .IsOk());
            }
        }
    } else {
#if !WITH_RENDER_DILIGENT
        auto compiled = CPostProcess::CompileShadersCpu();
        EXPECT_TRUE(compiled.IsOk());
        if (compiled.IsOk()) {
            EXPECT_EQ(compiled.Value().Status(), EShaderStatus::Ready);
            EXPECT_TRUE(
                post.InitWithCompiledShaders(
                        device, Move(compiled.Value()), 64, 64,
                        EFormat::B8G8R8A8_UNorm)
                    .IsOk());
        }
#else
        EXPECT_TRUE(
            post.Init(device, 64, 64, EFormat::B8G8R8A8_UNorm).IsOk());
#endif
    }
    EXPECT_TRUE(post.HdrRenderTarget() != nullptr);
    IRhiTexture* const first_hdr = post.HdrRenderTarget();
    CPostProcess::FCompiledShaders incomplete_post{};
    EXPECT_EQ(incomplete_post.Status(), EShaderStatus::Failed);
    EXPECT_TRUE(
        post.InitWithCompiledShaders(
                device, Move(incomplete_post), 32, 32,
                EFormat::B8G8R8A8_UNorm)
            .IsErr());
    EXPECT_TRUE(post.HdrRenderTarget() == first_hdr);

    // The original synchronous API shares the same owner-thread commit route.
    EXPECT_TRUE(post.Init(device, 0, 0, EFormat::B8G8R8A8_UNorm).IsOk());
    EXPECT_TRUE(post.HdrRenderTarget() != nullptr);
    if (post.HdrRenderTarget()) {
        EXPECT_EQ(post.HdrRenderTarget()->Width(), 1u);
        EXPECT_EQ(post.HdrRenderTarget()->Height(), 1u);
    }
    EXPECT_TRUE(post.Resize(0, 0).IsOk());
    EXPECT_TRUE(post.HdrRenderTarget() != nullptr);
    if (post.HdrRenderTarget()) {
        EXPECT_EQ(post.HdrRenderTarget()->Width(), 1u);
        EXPECT_EQ(post.HdrRenderTarget()->Height(), 1u);
    }
    IRhiTexture* const stable_hdr = post.HdrRenderTarget();
    const auto rejected_resize = post.Resize(65535u, 65535u);
    EXPECT_TRUE(rejected_resize.IsErr());
    EXPECT_TRUE(post.HdrRenderTarget() == stable_hdr);
    if (post.HdrRenderTarget()) {
        EXPECT_EQ(post.HdrRenderTarget()->Width(), 1u);
        EXPECT_EQ(post.HdrRenderTarget()->Height(), 1u);
    }
    // A failed candidate allocation leaves the live stack retryable.
    EXPECT_TRUE(post.Resize(96u, 48u).IsOk());
    EXPECT_TRUE(post.HdrRenderTarget() != nullptr);
    if (post.HdrRenderTarget()) {
        EXPECT_EQ(post.HdrRenderTarget()->Width(), 96u);
        EXPECT_EQ(post.HdrRenderTarget()->Height(), 48u);
    }

    CBlit blit;
    if (device.SupportsAsyncShaderCompilation()) {
        auto compiled = CBlit::BeginCompileShadersAsync(device);
        EXPECT_TRUE(compiled.IsOk());
        if (compiled.IsOk()) {
            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(15);
            EShaderStatus status = compiled.Value().Status();
            while (status == EShaderStatus::Compiling &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
                status = compiled.Value().Status();
            }
            EXPECT_EQ(status, EShaderStatus::Ready);
            if (status == EShaderStatus::Ready) {
                EXPECT_TRUE(
                    blit.InitWithCompiledShaders(
                            device, Move(compiled.Value()),
                            EFormat::R16G16B16A16_Float)
                        .IsOk());
            }
        }
    } else {
#if !WITH_RENDER_DILIGENT
        auto compiled = CBlit::CompileShadersCpu();
        EXPECT_TRUE(compiled.IsOk());
        if (compiled.IsOk()) {
            EXPECT_EQ(compiled.Value().Status(), EShaderStatus::Ready);
            EXPECT_TRUE(
                blit.InitWithCompiledShaders(
                        device, Move(compiled.Value()),
                        EFormat::R16G16B16A16_Float)
                    .IsOk());
        }
#else
        EXPECT_TRUE(
            blit.Init(device, EFormat::R16G16B16A16_Float).IsOk());
#endif
    }
    EXPECT_TRUE(blit.Pipeline() != nullptr);
    IRhiPipeline* const valid_blit = blit.Pipeline();
    CBlit::FCompiledShaders incomplete_blit{};
    EXPECT_EQ(incomplete_blit.Status(), EShaderStatus::Failed);
    EXPECT_TRUE(
        blit.InitWithCompiledShaders(
                device, Move(incomplete_blit),
                EFormat::R16G16B16A16_Float)
            .IsErr());
    EXPECT_TRUE(blit.Pipeline() == valid_blit);

    CSsao ssao;
    if (device.SupportsAsyncShaderCompilation()) {
        auto compiled_result =
            CSsao::BeginCompileShadersAsync(device);
        EXPECT_TRUE(compiled_result.IsOk());
        if (compiled_result.IsOk()) {
            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(15);
            EShaderStatus status =
                compiled_result.Value().Status();
            while (status == EShaderStatus::Compiling &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
                status = compiled_result.Value().Status();
            }
            EXPECT_EQ(status, EShaderStatus::Ready);
            if (status == EShaderStatus::Ready) {
                EXPECT_TRUE(
                    ssao.InitWithCompiledShaders(
                        device,
                        Move(compiled_result.Value()),
                        64,
                        64)
                        .IsOk());
            }
        }
    } else {
        EXPECT_TRUE(ssao.Init(device, 64, 64).IsOk());
    }
    EXPECT_TRUE(ssao.OutputTexture() != nullptr);
    if (ssao.OutputTexture() != nullptr) {
        EXPECT_EQ(ssao.OutputTexture()->Width(), 64u);
        EXPECT_EQ(ssao.OutputTexture()->Height(), 64u);
        EXPECT_TRUE(ssao.Resize(96, 48).IsOk());
        EXPECT_EQ(ssao.OutputTexture()->Width(), 96u);
        EXPECT_EQ(ssao.OutputTexture()->Height(), 48u);

        IRhiTexture* const valid_output = ssao.OutputTexture();
        CSsao::FCompiledShaders incomplete{};
        EXPECT_EQ(incomplete.Status(), EShaderStatus::Failed);
        EXPECT_TRUE(
            ssao.InitWithCompiledShaders(
                    device, Move(incomplete), 32, 32)
                .IsErr());
        EXPECT_TRUE(ssao.OutputTexture() == valid_output);
    }
    ssao.Shutdown();
    EXPECT_TRUE(ssao.OutputTexture() == nullptr);
    EXPECT_TRUE(ssao.Resize(64, 64).IsErr());
    // Non-async and unsupported backends retain the original synchronous path;
    // exercise it even when this device also supported the startup fast path.
    EXPECT_TRUE(ssao.Init(device, 64, 64).IsOk());

    CSsgi ssgi;
    EXPECT_TRUE(ssgi.Init(device, 64, 64).IsOk());
    EXPECT_TRUE(ssgi.OutputTexture() != nullptr);
    EXPECT_FALSE(ssgi.HasValidOutput());
    EXPECT_TRUE(ssgi.Resize(96, 48).IsOk());
    EXPECT_TRUE(ssgi.OutputTexture() != nullptr);
    if (ssgi.OutputTexture() != nullptr) {
        EXPECT_EQ(ssgi.OutputTexture()->Width(), 96u);
        EXPECT_EQ(ssgi.OutputTexture()->Height(), 48u);
    }
    EXPECT_FALSE(ssgi.HasValidOutput());

    CSsr ssr;
    EXPECT_TRUE(ssr.Init(device, EFormat::R16G16B16A16_Float, 64, 64).IsOk());
    EXPECT_TRUE(ssr.OutputTexture() != nullptr);
    EXPECT_FALSE(ssr.HasValidOutput());
    EXPECT_TRUE(ssr.Resize(96, 48).IsOk());
    EXPECT_TRUE(ssr.OutputTexture() != nullptr);
    if (ssr.OutputTexture() != nullptr) {
        EXPECT_EQ(ssr.OutputTexture()->Width(), 96u);
        EXPECT_EQ(ssr.OutputTexture()->Height(), 48u);
    }
    EXPECT_FALSE(ssr.HasValidOutput());

    CHiZ hiz;
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

    CMotionVector motion;
    EXPECT_TRUE(motion.Init(device, 64, 64).IsOk());

    CRefractionShader refraction;
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

    CVolumetricClouds clouds;
    EXPECT_TRUE(clouds.Init(device, EFormat::R16G16B16A16_Float).IsOk());

    // Keep the dedicated 3D-water VS displacement / normal-map / refraction
    // pipeline compiled on every active RHI backend. CPU ripple lifetime tests
    // alone cannot catch regressions in the embedded HLSL.
    CWaterSurface3D water;
    EXPECT_TRUE(water.Init(device, EFormat::R16G16B16A16_Float,
                           EFormat::D32_Float).IsOk());

    // Compile and execute the real two-pass SSSS path on the active backend.
    // This prevents the module from degrading into an unreferenced API whose
    // embedded HLSL is never validated.
    CSubsurfaceScattering subsurface;
    const auto subsurface_result = subsurface.Init(device, 64, 64);
    EXPECT_TRUE(subsurface_result.IsOk());
    if (subsurface_result.IsOk()) {
        EXPECT_TRUE(subsurface.IsReady());
        EXPECT_TRUE(subsurface.OutputTexture() != nullptr);
        EXPECT_TRUE(subsurface.HorizontalTexture() != nullptr);
        EXPECT_TRUE(subsurface.Resize(96, 48).IsOk());
        EXPECT_EQ(subsurface.Width(), 96u);
        EXPECT_EQ(subsurface.Height(), 48u);
        EXPECT_TRUE(subsurface.Resize(64, 64).IsOk());

        FTextureDesc hdr_desc{};
        hdr_desc.width = 64;
        hdr_desc.height = 64;
        hdr_desc.format = EFormat::R16G16B16A16_Float;
        hdr_desc.is_render_target = true;
        auto scene_result = CreateRhiTexture(device, hdr_desc);
        auto diffuse_result = CreateRhiTexture(device, hdr_desc);
        auto normal_result = CreateRhiTexture(device, hdr_desc);

        FTextureDesc material_desc = hdr_desc;
        material_desc.format = EFormat::R16G16B16A16_Float;
        auto material_result = CreateRhiTexture(device, material_desc);

        FTextureDesc depth_desc{};
        depth_desc.width = 64;
        depth_desc.height = 64;
        depth_desc.format = EFormat::D32_Float;
        depth_desc.is_depth_target = true;
        depth_desc.shader_visible_depth = true;
        auto depth_result = CreateRhiTexture(device, depth_desc);
        auto command_result = CreateRhiCommandList(device);

        EXPECT_TRUE(scene_result.IsOk());
        EXPECT_TRUE(diffuse_result.IsOk());
        EXPECT_TRUE(normal_result.IsOk());
        EXPECT_TRUE(material_result.IsOk());
        EXPECT_TRUE(depth_result.IsOk());
        EXPECT_TRUE(command_result.IsOk());
        if (scene_result.IsOk() && diffuse_result.IsOk() &&
            normal_result.IsOk() && material_result.IsOk() &&
            depth_result.IsOk() && command_result.IsOk()) {
            auto scene = Move(scene_result.Value());
            auto diffuse = Move(diffuse_result.Value());
            auto normal = Move(normal_result.Value());
            auto material = Move(material_result.Value());
            auto depth = Move(depth_result.Value());
            auto command = Move(command_result.Value());

            command->Begin();
            command->BeginRenderToTexture(
                *scene, FClearColor{0.35f, 0.20f, 0.12f, 1.0f});
            command->EndRenderToTexture(*scene);
            command->BeginRenderToTexture(
                *diffuse, FClearColor{0.25f, 0.12f, 0.07f, 1.0f});
            command->EndRenderToTexture(*diffuse);
            command->BeginRenderToTexture(
                *normal, FClearColor{0.0f, 0.0f, 1.0f, 1.0f});
            command->EndRenderToTexture(*normal);
            command->BeginRenderToTexture(
                *material, FClearColor{0.02f, 0.01f, 0.005f, 1.0f});
            command->EndRenderToTexture(*material);
            command->BeginShadowPass(*depth, 0.5f);
            command->EndShadowPass(*depth);

            FSubsurfaceScatteringParams subsurface_params{};
            subsurface_params.radius_world = 0.02f;
            EXPECT_TRUE(subsurface.Render(
                *command, *scene, *diffuse, *depth, *normal, *material,
                FMat4::Identity(), subsurface_params));
            command->End();
            command->Submit();
            device.WaitIdle();

            u16 output_pixels[64 * 64 * 4]{};
            const bool output_read = device.ReadTexture(
                *subsurface.OutputTexture(), output_pixels,
                static_cast<u32>(sizeof(output_pixels)));
            EXPECT_TRUE(output_read);
            if (output_read) {
                constexpr usize center =
                    (32u * 64u + 32u) * 4u;
                EXPECT_TRUE(output_pixels[center + 0u] != 0u);
                EXPECT_TRUE(output_pixels[center + 1u] != 0u);
                EXPECT_TRUE(output_pixels[center + 2u] != 0u);
                EXPECT_EQ(output_pixels[center + 3u],
                          static_cast<u16>(0x3C00u));
            }
        }
    }

    CStandardShader standard;
    EXPECT_FALSE(standard.BeginFrame(0u));
    const auto standard_result =
        standard.Init(device, EFormat::R16G16B16A16_Float, EFormat::D32_Float);
    EXPECT_TRUE(standard_result.IsOk());
    if (standard_result.IsOk()) {
        EXPECT_EQ(standard.ObjectBufferPageCount(), 1u);
        EXPECT_FALSE(standard.BeginFrame(
            std::numeric_limits<u32>::max()));
        EXPECT_EQ(standard.ObjectBufferPageCount(), 1u);
        EXPECT_FALSE(standard.SetObject(FMat4::Identity()));
        EXPECT_EQ(standard.ObjectDrawCount(), 0u);
        EXPECT_TRUE(standard.PerObjectCB() == nullptr);

        constexpr u32 kLargeStandardDrawCount = 512u;
        EXPECT_TRUE(standard.BeginFrame(kLargeStandardDrawCount));
        EXPECT_TRUE(
            standard.ObjectBufferCapacity() >= kLargeStandardDrawCount);
        EXPECT_EQ(standard.ObjectBufferPageCount(), 2u);
        standard.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                           nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        IRhiBuffer* first_object = nullptr;
        for (u32 i = 0u; i < kLargeStandardDrawCount; ++i) {
            EXPECT_TRUE(standard.SetObject(FMat4::Identity()));
            if (i == 0u) first_object = standard.PerObjectCB();
        }
        EXPECT_TRUE(first_object != nullptr);
        EXPECT_TRUE(standard.PerObjectCB() != first_object);
        EXPECT_EQ(
            standard.ObjectDrawCount(), kLargeStandardDrawCount);

        const u32 retained_standard_capacity =
            standard.ObjectBufferCapacity();
        const u32 retained_standard_pages =
            standard.ObjectBufferPageCount();
        EXPECT_TRUE(standard.BeginFrame(1u));
        EXPECT_EQ(
            standard.ObjectBufferCapacity(), retained_standard_capacity);
        EXPECT_EQ(standard.ObjectBufferPageCount(), retained_standard_pages);
        EXPECT_EQ(standard.ObjectDrawCount(), 0u);
        standard.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                           nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        EXPECT_TRUE(standard.SetObject(FMat4::Identity()));
        EXPECT_TRUE(standard.PerObjectCB() != nullptr);
        standard.SetLights(FMat4::Identity(), FVec3{1.0f, 0.0f, 0.0f},
                           nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        EXPECT_EQ(standard.ObjectDrawCount(), 1u);
        standard.Shutdown();
        EXPECT_FALSE(standard.BeginFrame(0u));
    }

    CSkinnedShader skinned;
    EXPECT_FALSE(skinned.BeginFrame(0u));
    const auto skinned_result =
        skinned.Init(device, EFormat::R16G16B16A16_Float, EFormat::D32_Float);
    EXPECT_TRUE(skinned_result.IsOk());
    if (skinned_result.IsOk()) {
        EXPECT_FALSE(skinned.BeginFrame(
            std::numeric_limits<u32>::max()));
        EXPECT_FALSE(skinned.SetObject(FMat4::Identity()));
        EXPECT_FALSE(skinned.SetBonePalette(nullptr, 0));
        EXPECT_EQ(skinned.ObjectDrawCount(), 0u);
        EXPECT_TRUE(skinned.PerObjectCB() == nullptr);
        EXPECT_TRUE(skinned.BonesCB() == nullptr);

        constexpr u32 kLargeSkinnedDrawCount = 512u;
        EXPECT_TRUE(skinned.BeginFrame(kLargeSkinnedDrawCount));
        EXPECT_TRUE(
            skinned.ObjectBufferCapacity() >= kLargeSkinnedDrawCount);
        skinned.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                          nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        IRhiBuffer* first_skinned_object = nullptr;
        IRhiBuffer* first_bones = nullptr;
        for (u32 i = 0u; i < kLargeSkinnedDrawCount; ++i) {
            EXPECT_TRUE(skinned.SetObject(FMat4::Identity()));
            EXPECT_TRUE(skinned.SetBonePalette(nullptr, 0));
            if (i == 0u) {
                first_skinned_object = skinned.PerObjectCB();
                first_bones = skinned.BonesCB();
            }
        }
        EXPECT_TRUE(first_skinned_object != nullptr);
        EXPECT_TRUE(first_bones != nullptr);
        EXPECT_TRUE(skinned.PerObjectCB() != first_skinned_object);
        EXPECT_TRUE(skinned.BonesCB() != first_bones);
        EXPECT_EQ(skinned.ObjectDrawCount(), kLargeSkinnedDrawCount);

        const u32 retained_skinned_capacity =
            skinned.ObjectBufferCapacity();
        EXPECT_TRUE(skinned.BeginFrame(1u));
        EXPECT_EQ(
            skinned.ObjectBufferCapacity(), retained_skinned_capacity);
        EXPECT_EQ(skinned.ObjectDrawCount(), 0u);
        skinned.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 0.0f},
                          nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        EXPECT_TRUE(skinned.SetObject(FMat4::Identity()));
        EXPECT_TRUE(skinned.SetBonePalette(nullptr, 0));
        skinned.SetLights(FMat4::Identity(), FVec3{1.0f, 0.0f, 0.0f},
                          nullptr, 0, FVec3{0.0f, 0.0f, 0.0f});
        EXPECT_EQ(skinned.ObjectDrawCount(), 1u);
        skinned.Shutdown();
        EXPECT_FALSE(skinned.BeginFrame(0u));
    }

    // Runtime startup remains base-only even though Legacy can now lazily
    // upgrade an SSS material scene. Raw DX12 constructs the complete
    // unpublished base candidate on a worker; joining before inspection
    // mirrors the runtime publication edge without opening a window.
    CPbrShader base_only_pbr;
    bool base_only_ready = false;
#if !WITH_RENDER_DILIGENT
    std::atomic<bool> background_ready{false};
    std::thread pbr_candidate_worker([&]() noexcept {
        auto shaders = CPbrShader::CompileShadersCpu(false);
        if (shaders.IsErr()) return;
        const auto initialized =
            base_only_pbr.BuildInitializedCandidateForRawDx12(
                device, Move(shaders.Value()),
                EFormat::R16G16B16A16_Float,
                EFormat::D32_Float, ECullMode::None);
        background_ready.store(
            initialized.IsOk(), std::memory_order_release);
    });
    pbr_candidate_worker.join();
    base_only_ready =
        background_ready.load(std::memory_order_acquire);
#else
    base_only_ready = base_only_pbr.Init(
        device, EFormat::R16G16B16A16_Float,
        EFormat::D32_Float, ECullMode::None, false).IsOk();
#endif
    EXPECT_TRUE(base_only_ready);
    if (base_only_ready)
        EXPECT_TRUE(!base_only_pbr.HasSubsurfaceMrtPipeline());
    base_only_pbr.Shutdown();

    CPbrShader pbr;
    const auto pbr_result = pbr.Init(device, EFormat::R16G16B16A16_Float,
                                     EFormat::D32_Float,
                                     ECullMode::None);
    EXPECT_TRUE(pbr_result.IsOk());
    if (pbr_result.IsOk()) {
        EXPECT_EQ(pbr.ObjectBufferPageCount(), 1u);
        // Compile, bind and execute the optional four-target shader/PSO, not
        // merely its single-target sibling. RT2 proves the material profile;
        // RT3 proves the final normal after PBR normal mapping is exported.
        EXPECT_TRUE(pbr.HasSubsurfaceMrtPipeline());
        AMeshAsset triangle;
        triangle.Vertices().Add(FMeshVertex{
            FVec3{-0.8f, -0.8f, 0.5f}, FVec3{0, 0, 1}, 0, 1});
        triangle.Vertices().Add(FMeshVertex{
            FVec3{0.0f, 0.8f, 0.5f}, FVec3{0, 0, 1}, 0.5f, 0});
        triangle.Vertices().Add(FMeshVertex{
            FVec3{0.8f, -0.8f, 0.5f}, FVec3{0, 0, 1}, 1, 1});
        triangle.Indices().Add(0);
        triangle.Indices().Add(1);
        triangle.Indices().Add(2);
        FGpuMesh gpu_triangle{};
        const auto mesh_result =
            UploadMesh(device, triangle, gpu_triangle);
        EXPECT_TRUE(mesh_result.IsOk());

        FTextureDesc hdr_description{};
        hdr_description.width = 64;
        hdr_description.height = 64;
        hdr_description.format = EFormat::R16G16B16A16_Float;
        hdr_description.is_render_target = true;
        auto mrt_scene = CreateRhiTexture(device, hdr_description);
        auto mrt_diffuse = CreateRhiTexture(device, hdr_description);
        FTextureDesc mask_description = hdr_description;
        mask_description.format = EFormat::R16G16B16A16_Float;
        auto mrt_mask = CreateRhiTexture(device, mask_description);
        auto mrt_normal = CreateRhiTexture(device, hdr_description);
        FTextureDesc mrt_depth_description{};
        mrt_depth_description.width = 64;
        mrt_depth_description.height = 64;
        mrt_depth_description.format = EFormat::D32_Float;
        mrt_depth_description.is_depth_target = true;
        auto mrt_depth =
            CreateRhiTexture(device, mrt_depth_description);
        auto mrt_command = CreateRhiCommandList(device);
        EXPECT_TRUE(mrt_scene.IsOk());
        EXPECT_TRUE(mrt_diffuse.IsOk());
        EXPECT_TRUE(mrt_mask.IsOk());
        EXPECT_TRUE(mrt_normal.IsOk());
        EXPECT_TRUE(mrt_depth.IsOk());
        EXPECT_TRUE(mrt_command.IsOk());
        if (pbr.HasSubsurfaceMrtPipeline() &&
            mesh_result.IsOk() && mrt_scene.IsOk() &&
            mrt_diffuse.IsOk() && mrt_mask.IsOk() &&
            mrt_normal.IsOk() &&
            mrt_depth.IsOk() && mrt_command.IsOk()) {
            FDirLight light{};
            light.direction = FVec3{0, 0, 1};
            light.color = FVec3{1.5f, 1.5f, 1.5f};
            pbr.SetIbl(nullptr, nullptr, nullptr, 0);
            pbr.SetShadowMap(nullptr, FMat4::Identity());
            pbr.SetSsao(nullptr, 0.0f, 64, 64);

            auto command = Move(mrt_command.Value());
            IRhiTexture* targets[4] = {
                mrt_scene.Value().Get(),
                mrt_diffuse.Value().Get(),
                mrt_mask.Value().Get(),
                mrt_normal.Value().Get()
            };
            auto render_material_profile =
                [&](u16 (&center_profile)[4]) noexcept {
                    if (!pbr.BeginFrame(1u)) return false;
                    pbr.SetLights(
                        FMat4::Identity(), FVec3{0, 0, 2},
                        &light, 1, FVec3{0.2f, 0.2f, 0.2f});
                    command->Begin();
                    const bool mrt_bound =
                        command->BeginRenderToTextureMrt(
                            targets, 4u, FClearColor{0, 0, 0, 0},
                            mrt_depth.Value().Get(), 1.0f);
                    EXPECT_TRUE(mrt_bound);
                    if (!mrt_bound) {
                        command->End();
                        return false;
                    }
                    const bool drew = pbr.DrawMeshSubsurfaceMrt(
                        *command, gpu_triangle, FMat4::Identity(),
                        FVec3{0.85f, 0.20f, 0.10f},
                        0.0f, 0.5f, 1.0f);
                    command->EndRenderToTextureMrt(targets, 4u);
                    command->End();
                    command->Submit();
                    device.WaitIdle();

                    u16 material_pixels[64 * 64 * 4]{};
                    const bool read = device.ReadTexture(
                        *mrt_mask.Value(), material_pixels,
                        static_cast<u32>(sizeof(material_pixels)));
                    u16 normal_pixels[64 * 64 * 4]{};
                    const bool normal_read = device.ReadTexture(
                        *mrt_normal.Value(), normal_pixels,
                        static_cast<u32>(sizeof(normal_pixels)));
                    if (read) {
                        constexpr usize center =
                            (32u * 64u + 32u) * 4u;
                        for (u32 channel = 0u; channel < 4u; ++channel) {
                            center_profile[channel] =
                                material_pixels[center + channel];
                        }
                    }
                    if (normal_read) {
                        constexpr usize center =
                            (32u * 64u + 32u) * 4u;
                        EXPECT_EQ(normal_pixels[center + 0u],
                                  static_cast<u16>(0u));
                        EXPECT_EQ(normal_pixels[center + 1u],
                                  static_cast<u16>(0u));
                        EXPECT_EQ(normal_pixels[center + 2u],
                                  static_cast<u16>(0x3C00u));
                        EXPECT_EQ(normal_pixels[center + 3u],
                                  static_cast<u16>(0x3C00u));
                    }
                    EXPECT_TRUE(drew);
                    EXPECT_TRUE(read);
                    EXPECT_TRUE(normal_read);
                    return drew && read && normal_read;
                };

            // Legacy authoring: RGB radii are color * scalar centimetres
            // converted to scene metres; A is the independent coverage.
            u16 legacy_profile_a[4]{};
            pbr.ClearSubstrateSurface();
            pbr.SetSubsurface(
                FVec3{1.0f, 0.45f, 0.20f}, 1.0f);
            const bool legacy_a_ok =
                render_material_profile(legacy_profile_a);
            if (legacy_a_ok) {
                EXPECT_TRUE(legacy_profile_a[0] >
                            legacy_profile_a[1]);
                EXPECT_TRUE(legacy_profile_a[1] >
                            legacy_profile_a[2]);
                EXPECT_EQ(legacy_profile_a[3],
                          static_cast<u16>(0x3C00u));
            }

            u16 legacy_profile_b[4]{};
            pbr.ClearSubstrateSurface();
            pbr.SetSubsurface(
                FVec3{0.10f, 1.0f, 0.80f}, 0.60f);
            const bool legacy_b_ok =
                render_material_profile(legacy_profile_b);
            if (legacy_a_ok && legacy_b_ok) {
                EXPECT_TRUE(legacy_profile_b[0] <
                            legacy_profile_a[0]);
                EXPECT_TRUE(legacy_profile_b[1] >
                            legacy_profile_a[1]);
                EXPECT_TRUE(legacy_profile_b[2] >
                            legacy_profile_a[2]);
                EXPECT_TRUE(legacy_profile_b[3] <
                            legacy_profile_a[3]);
            }

            // Substrate authoring preserves each MFP channel and thickness
            // response rather than collapsing RGB to max(MFP).
            FSubstrateResolvedSurface substrate_a{};
            substrate_a.diffuse_albedo =
                FVec3{0.75f, 0.30f, 0.18f};
            substrate_a.mean_free_path_cm =
                FVec3{2.0f, 0.60f, 0.15f};
            substrate_a.thickness_cm = 0.03f;
            pbr.SetSubstrateSurface(substrate_a);
            u16 substrate_profile_a[4]{};
            const bool substrate_a_ok =
                render_material_profile(substrate_profile_a);
            if (substrate_a_ok) {
                EXPECT_TRUE(substrate_profile_a[0] >
                            substrate_profile_a[1]);
                EXPECT_TRUE(substrate_profile_a[1] >
                            substrate_profile_a[2]);
                EXPECT_EQ(substrate_profile_a[3],
                          static_cast<u16>(0x3A00u));
            }

            FSubstrateResolvedSurface substrate_b = substrate_a;
            substrate_b.mean_free_path_cm =
                FVec3{0.25f, 1.40f, 0.05f};
            substrate_b.thickness_cm = 0.005f;
            pbr.SetSubstrateSurface(substrate_b);
            u16 substrate_profile_b[4]{};
            const bool substrate_b_ok =
                render_material_profile(substrate_profile_b);
            if (substrate_a_ok && substrate_b_ok) {
                EXPECT_TRUE(substrate_profile_b[0] <
                            substrate_profile_a[0]);
                EXPECT_TRUE(substrate_profile_b[1] >
                            substrate_profile_a[1]);
                EXPECT_TRUE(substrate_profile_b[2] <
                            substrate_profile_a[2]);
                EXPECT_TRUE(substrate_profile_b[3] <
                            substrate_profile_a[3]);
            }
        }

        // Regression: the former fixed 256-entry ring made every visible PBR
        // object after #256 disappear. Exercise 512 ordinary draws and 300 MRT
        // draws through real command lists on both Raw DX12 and Diligent.
        auto large_draw_command_result = CreateRhiCommandList(device);
        EXPECT_TRUE(large_draw_command_result.IsOk());
        if (mesh_result.IsOk() && mrt_scene.IsOk() &&
            mrt_diffuse.IsOk() && mrt_mask.IsOk() &&
            mrt_normal.IsOk() && mrt_depth.IsOk() &&
            large_draw_command_result.IsOk()) {
            auto large_draw_command =
                Move(large_draw_command_result.Value());
            pbr.ClearSubstrateSurface();
            pbr.SetSubsurface(FVec3{0.0f, 0.0f, 0.0f}, 0.0f);
            EXPECT_TRUE(pbr.BeginFrame(512u));
            EXPECT_TRUE(pbr.ObjectBufferCapacity() >= 512u);
            EXPECT_EQ(pbr.ObjectBufferPageCount(), 2u);
            pbr.SetLights(FMat4::Identity(), FVec3{0.0f, 0.0f, 2.0f}, nullptr, 0u, FVec3{1.0f, 1.0f, 1.0f});
            large_draw_command->Begin();
            large_draw_command->BeginRenderToTexture(
                *mrt_scene.Value(), FClearColor{0, 0, 0, 0},
                mrt_depth.Value().Get(), 1.0f);
            bool normal_draws_valid = true;
            for (u32 draw = 0u; draw < 512u; ++draw) {
                /** 中間slotを画面外へ置き、先頭と末尾のpage跨ぎ結果だけを観測するmodel。 */
                FMat4 draw_model = FMat4::Translation(FVec3{4.0f, 0.0f, 0.0f});
                /** page跨ぎで異なる定数範囲を識別する色。 */
                FVec3 draw_color = FVec3{0.0f, 0.0f, 1.0f};
                if (draw == 0u) {
                    draw_model = FMat4::Translation(FVec3{-0.5f, 0.0f, 0.0f});
                    draw_color = FVec3{1.0f, 0.0f, 0.0f};
                } else if (draw == 511u) {
                    draw_model = FMat4::Translation(FVec3{0.5f, 0.0f, 0.0f});
                    draw_color = FVec3{0.0f, 1.0f, 0.0f};
                }
                normal_draws_valid = pbr.DrawMesh(*large_draw_command, gpu_triangle, draw_model, draw_color, 0.0f, 0.5f, 1.0f) && normal_draws_valid;
            }
            large_draw_command->EndRenderToTexture(
                *mrt_scene.Value());
            large_draw_command->End();
            large_draw_command->Submit();
            device.WaitIdle();
            EXPECT_TRUE(normal_draws_valid);
            EXPECT_EQ(pbr.ObjectDrawCount(), static_cast<u32>(512u));
            /** slot 0と511の異なる親page・offsetを描画結果で検証するHDR pixel列。 */
            u16 arena_pixels[64u * 64u * 4u]{};
            /** 共有arena描画結果のreadback成否。 */
            const bool arena_output_read = device.ReadTexture(*mrt_scene.Value(), arena_pixels, static_cast<u32>(sizeof(arena_pixels)));
            EXPECT_TRUE(arena_output_read);
            if (arena_output_read) {
                /** 左側slot 0の三角形中央pixel。 */
                constexpr usize kLeftCenter = (32u * 64u + 16u) * 4u;
                /** 右側slot 511の三角形中央pixel。 */
                constexpr usize kRightCenter = (32u * 64u + 48u) * 4u;
                EXPECT_EQ(arena_pixels[kLeftCenter + 0u], static_cast<u16>(0x3C00u));
                EXPECT_EQ(arena_pixels[kLeftCenter + 1u], static_cast<u16>(0u));
                EXPECT_EQ(arena_pixels[kLeftCenter + 2u], static_cast<u16>(0u));
                EXPECT_EQ(arena_pixels[kLeftCenter + 3u], static_cast<u16>(0x3C00u));
                EXPECT_EQ(arena_pixels[kRightCenter + 0u], static_cast<u16>(0u));
                EXPECT_EQ(arena_pixels[kRightCenter + 1u], static_cast<u16>(0x3C00u));
                EXPECT_EQ(arena_pixels[kRightCenter + 2u], static_cast<u16>(0u));
                EXPECT_EQ(arena_pixels[kRightCenter + 3u], static_cast<u16>(0x3C00u));
            }

            const u32 retained_capacity =
                pbr.ObjectBufferCapacity();
            const u32 retained_pages =
                pbr.ObjectBufferPageCount();
            EXPECT_TRUE(pbr.BeginFrame(300u));
            EXPECT_EQ(
                pbr.ObjectBufferCapacity(), retained_capacity);
            EXPECT_EQ(pbr.ObjectBufferPageCount(), retained_pages);
            EXPECT_EQ(pbr.ObjectDrawCount(), static_cast<u32>(0u));
            pbr.SetLights(
                FMat4::Identity(), FVec3{0.0f, 0.0f, 2.0f},
                nullptr, 0u, FVec3{0.1f, 0.1f, 0.1f});
            IRhiTexture* large_mrt_targets[4] = {
                mrt_scene.Value().Get(),
                mrt_diffuse.Value().Get(),
                mrt_mask.Value().Get(),
                mrt_normal.Value().Get(),
            };
            large_draw_command->Begin();
            const bool large_mrt_bound =
                large_draw_command->BeginRenderToTextureMrt(
                    large_mrt_targets, 4u,
                    FClearColor{0, 0, 0, 0},
                    mrt_depth.Value().Get(), 1.0f);
            EXPECT_TRUE(large_mrt_bound);
            bool mrt_draws_valid = large_mrt_bound;
            if (large_mrt_bound) {
                for (u32 draw = 0u; draw < 300u; ++draw) {
                    mrt_draws_valid =
                        pbr.DrawMeshSubsurfaceMrt(
                            *large_draw_command, gpu_triangle,
                            FMat4::Identity(),
                            FVec3{0.4f, 0.5f, 0.6f},
                            0.0f, 0.5f, 1.0f) &&
                        mrt_draws_valid;
                }
                large_draw_command->EndRenderToTextureMrt(
                    large_mrt_targets, 4u);
            }
            large_draw_command->End();
            large_draw_command->Submit();
            device.WaitIdle();
            EXPECT_TRUE(mrt_draws_valid);
            EXPECT_EQ(
                pbr.ObjectDrawCount(), static_cast<u32>(300u));
            // Lighting changes inside one command-list frame must not rewind
            // object storage referenced by the first pass.
            pbr.SetLights(
                FMat4::Identity(), FVec3{0.0f, 0.0f, 2.0f},
                nullptr, 0u, FVec3{0.2f, 0.2f, 0.2f});
            EXPECT_EQ(
                pbr.ObjectDrawCount(), static_cast<u32>(300u));
        }
    }
}

ACS_TEST(PostEffects,
         PbrObjectBufferFrameBoundariesCoverProductionCallsites)
{
    const std::string pbr =
        ReadWorkspaceSource("src/render/PbrShader.cpp");
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string legacy =
        ReadWorkspaceSource(
            "src/gameframework/LegacyScene3DAdapter.cpp");
    EXPECT_TRUE(!pbr.empty());
    EXPECT_TRUE(!editor.empty());
    EXPECT_TRUE(!legacy.empty());
    if (pbr.empty() || editor.empty() || legacy.empty()) {
        return;
    }

    const std::string set_lights =
        ExtractFunction(pbr, "void CPbrShader::SetLights(");
    EXPECT_TRUE(set_lights.find("m_ObjectCbCursor") ==
                std::string::npos);
    EXPECT_TRUE(pbr.find(
        "bool CPbrShader::BeginFrame(u32 required_object_draws)") !=
        std::string::npos);
    EXPECT_TRUE(editor.find("h.pbr3d.BeginFrame(") !=
                std::string::npos);
    EXPECT_TRUE(editor.find("shader.BeginFrame(1u)") !=
                std::string::npos);
    EXPECT_TRUE(editor.find("CountPbrFrameDraws(") !=
                std::string::npos);
    EXPECT_TRUE(legacy.find(
        "ActiveHdrShader().BeginFrame(pbr_full_required)") !=
        std::string::npos);
    EXPECT_TRUE(legacy.find("draw_count >= 256u") ==
                std::string::npos);
    EXPECT_TRUE(legacy.find(
        "draws_valid =\n                shader.DrawMesh(") !=
        std::string::npos);
}

ACS_TEST(PostEffects, GrowableLegacyShaderPoolsCoverProductionAndPublicContracts)
{
    const std::string standard =
        ReadWorkspaceSource("src/render/StandardShader.cpp");
    const std::string standard_header =
        ReadWorkspaceSource("src/render/StandardShader.h");
    const std::string skinned =
        ReadWorkspaceSource("src/render/SkinnedShader.cpp");
    const std::string skinned_header =
        ReadWorkspaceSource("src/render/SkinnedShader.h");
    const std::string shadow =
        ReadWorkspaceSource("src/render/ShadowMap.cpp");
    const std::string shadow_header =
        ReadWorkspaceSource("src/render/ShadowMap.h");
    const std::string render_reference =
        ReadWorkspaceSource("docs/reference/data/render_core.js");

    EXPECT_TRUE(!standard.empty());
    EXPECT_TRUE(!standard_header.empty());
    EXPECT_TRUE(!skinned.empty());
    EXPECT_TRUE(!skinned_header.empty());
    EXPECT_TRUE(!shadow.empty());
    EXPECT_TRUE(!shadow_header.empty());
    EXPECT_TRUE(!render_reference.empty());
    if (standard.empty() || standard_header.empty() ||
        skinned.empty() || skinned_header.empty() ||
        shadow.empty() || shadow_header.empty() ||
        render_reference.empty()) return;

    const std::string standard_set_lights =
        ExtractFunction(
            standard, "void CStandardShader::SetLights(");
    const std::string skinned_set_lights =
        ExtractFunction(
            skinned, "void CSkinnedShader::SetLights(");
    EXPECT_TRUE(standard_set_lights.find("m_ObjectCbCursor") ==
                std::string::npos);
    EXPECT_TRUE(skinned_set_lights.find("m_ObjectCbCursor") ==
                std::string::npos);
    EXPECT_TRUE(standard.find(
        "bool CStandardShader::BeginFrame(u32 required_object_draws)") !=
        std::string::npos);
    EXPECT_TRUE(skinned.find(
        "bool CSkinnedShader::BeginFrame(u32 required_object_draws)") !=
        std::string::npos);
    EXPECT_TRUE(shadow.find(
        "bool CShadowMap::BeginFrame(") != std::string::npos);
    const std::string shadow_begin_frame =
        ExtractFunction(shadow, "bool CShadowMap::BeginFrame(");
    const std::string shadow_ensure_capacity =
        ExtractFunction(shadow, "bool CShadowMap::EnsureCasterCapacity(");
    EXPECT_TRUE(shadow_begin_frame.find("cascade < m_CascadeCapacity") !=
                std::string::npos);
    EXPECT_TRUE(shadow_begin_frame.find("cascade < m_CascadeCount") ==
                std::string::npos);
    EXPECT_TRUE(shadow_ensure_capacity.find(
                    "cascade >= m_CascadeCapacity") != std::string::npos);
    EXPECT_TRUE(shadow_ensure_capacity.find(
                    "cascade >= m_CascadeCount") == std::string::npos);
    EXPECT_TRUE(standard_header.find(
                    "[[deprecated(\"growable pool; not a hard limit\")]]") !=
                std::string::npos);
    EXPECT_TRUE(standard_header.find("kMaxObjectDrawsPerFrame = 256u") !=
                std::string::npos);
    EXPECT_TRUE(skinned_header.find("kMaxObjectDrawsPerFrame = 256u") !=
                std::string::npos);
    EXPECT_TRUE(shadow_header.find(
                    "kMaxCasterDrawsPerCascade = 256u") !=
                std::string::npos);
    EXPECT_TRUE(shadow_header.find("kMaxCasterDrawsPerFrame") !=
                std::string::npos);
    EXPECT_TRUE(standard.find("kMaxObjectDrawsPerFrame") ==
                std::string::npos);
    EXPECT_TRUE(skinned.find("kMaxObjectDrawsPerFrame") ==
                std::string::npos);
    EXPECT_TRUE(shadow.find("kMaxCasterDrawsPerCascade") ==
                std::string::npos);
    EXPECT_TRUE(shadow.find("kMaxCasterDrawsPerFrame") ==
                std::string::npos);
    EXPECT_TRUE(skinned_header.find("struct FDrawBufferPair") !=
                std::string::npos);
    EXPECT_TRUE(skinned_header.find("TArray<FDrawBufferPair>") !=
                std::string::npos);

    EXPECT_TRUE(render_reference.find(
        "if (!shd.BeginFrame(/* exact standard draws this frame */ 1u)) "
        "return;") != std::string::npos);
    EXPECT_TRUE(render_reference.find(
        "if (!shd.BeginFrame(/* exact skinned draws this frame */ 1u)) "
        "return;") != std::string::npos);
    EXPECT_TRUE(render_reference.find(
        "{ sig: \"bool SetObject(const FMat4& model") !=
        std::string::npos);
    EXPECT_TRUE(render_reference.find(
        "{ sig: \"bool SetObject(...)\"") != std::string::npos);
    EXPECT_TRUE(render_reference.find(
        "if (!shd.SetBonePalette(palette, nb)) return;") !=
        std::string::npos);
    EXPECT_TRUE(render_reference.find(
        "{ sig: \"bool SetBonePalette(") != std::string::npos);
    EXPECT_TRUE(render_reference.find(
        "if (!sm.BeginFrame(static_cast&lt;u32&gt;(casters.Size()))) "
        "return;") != std::string::npos);
    EXPECT_TRUE(render_reference.find(
        "bool TrySetCaster(const FMat4& model)") != std::string::npos);
    EXPECT_TRUE(render_reference.find("sm.SetCaster(") ==
                std::string::npos);
}

ACS_TEST(PostEffects, TemporalHistoryRemainsSceneLinearAcrossEyeAdaptation)
{
    const std::string post =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    EXPECT_TRUE(!post.empty());
    if (post.empty()) return;

    const std::string render =
        ExtractFunction(post, "void CPostProcess::Render");
    const std::string taa =
        ExtractFunction(post, "bool CPostProcess::Pass_TaaResolve");
    const std::string exposure =
        ExtractFunction(post, "bool CPostProcess::Pass_ExposureApply");
    const std::string luma =
        ExtractFunction(post, "bool CPostProcess::Pass_LumaReduce");
    EXPECT_TRUE(!render.empty());
    EXPECT_TRUE(!taa.empty());
    EXPECT_TRUE(!exposure.empty());
    EXPECT_TRUE(!luma.empty());
    if (render.empty() || taa.empty() || exposure.empty() || luma.empty())
        return;

    const std::size_t meter = render.find("Pass_LumaReduce(cmd) &&");
    const std::size_t adapt =
        render.find("Pass_ExposureAdapt(cmd, safe_params);");
    const std::size_t resolve =
        render.find("Pass_TaaResolve(cmd, safe_params);");
    const std::size_t expose =
        render.find("Pass_ExposureApply(cmd, *exposure_source);");
    const std::size_t bloom =
        render.find("if (safe_params.bloom_enabled");
    EXPECT_TRUE(meter != std::string::npos);
    EXPECT_TRUE(adapt != std::string::npos);
    EXPECT_TRUE(resolve != std::string::npos);
    EXPECT_TRUE(expose != std::string::npos);
    EXPECT_TRUE(bloom != std::string::npos);
    EXPECT_TRUE(meter < adapt);
    EXPECT_TRUE(adapt < resolve);
    EXPECT_TRUE(resolve < expose);
    EXPECT_TRUE(expose < bloom);

    // Metering remains based on unexposed scene radiance.
    EXPECT_TRUE(luma.find("cmd.SetTexture(0, *m_HdrRt);") !=
                std::string::npos);
    // Current and history samples share one scene-linear exposure domain.
    EXPECT_TRUE(taa.find("IRhiTexture* scene = m_HdrRt.Get();") !=
                std::string::npos);
    EXPECT_TRUE(taa.find("SceneInput(p)") == std::string::npos);
    // Eye adaptation is applied to the resolved image, never baked into
    // history.
    EXPECT_TRUE(exposure.find("cmd.SetTexture(0, source);") !=
                std::string::npos);

    const std::string scene_input =
        ExtractFunction(
            post,
            "IRhiTexture* CPostProcess::SceneInput");
    EXPECT_TRUE(!scene_input.empty());
    EXPECT_TRUE(scene_input.find(
        "p.auto_exposure_enabled && m_ExposureOutputValid") !=
        std::string::npos);
    EXPECT_TRUE(scene_input.find(
        "p.taa_enabled && m_TaaOutputValid") !=
        std::string::npos);
}

ACS_TEST(PostEffects, TemporalHistoryPolicyColdStartsAndPreservesWarmInputs)
{
    const FMat4 current = FMat4::Translation(FVec3{1.0f, 2.0f, 3.0f});
    const FMat4 previous = FMat4::Translation(FVec3{-4.0f, 5.0f, 6.0f});

    const auto cold = ResolveTemporalHistoryFrame(
        0u, current, previous, 0.27f, true);
    EXPECT_EQ(cold.current_frame_weight, 1.0f);
    EXPECT_FALSE(cold.motion_vectors_enabled);
    for (u32 row = 0; row < 4u; ++row) {
        for (u32 column = 0; column < 4u; ++column) {
            EXPECT_EQ(
                cold.previous_view_projection.m[row][column],
                current.m[row][column]);
        }
    }

    const auto warm = ResolveTemporalHistoryFrame(
        7u, current, previous, 0.1f, true);
    EXPECT_EQ(warm.current_frame_weight, 0.1f);
    EXPECT_TRUE(warm.motion_vectors_enabled);
    for (u32 row = 0; row < 4u; ++row) {
        for (u32 column = 0; column < 4u; ++column) {
            EXPECT_EQ(
                warm.previous_view_projection.m[row][column],
                previous.m[row][column]);
        }
    }

    const auto warm_without_motion = ResolveTemporalHistoryFrame(
        1u, current, previous, 0.35f, false);
    EXPECT_EQ(warm_without_motion.current_frame_weight, 0.35f);
    EXPECT_FALSE(warm_without_motion.motion_vectors_enabled);
}

ACS_TEST(PostEffects, TemporalPassesShareColdStartPolicy)
{
    const std::string post =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    const std::string ssr =
        ReadWorkspaceSource("src/render/Ssr.cpp");
    const std::string ssgi =
        ReadWorkspaceSource("src/render/Ssgi.cpp");
    EXPECT_TRUE(!post.empty());
    EXPECT_TRUE(!ssr.empty());
    EXPECT_TRUE(!ssgi.empty());
    if (post.empty() || ssr.empty() || ssgi.empty()) return;

    const std::string taa_resolve =
        ExtractFunction(post, "bool CPostProcess::Pass_TaaResolve");
    const std::string ssr_render =
        ExtractFunction(ssr, "void CSsr::Render");
    const std::string ssgi_render =
        ExtractFunction(ssgi, "void CSsgi::Render");
    const std::string ssgi_temporal =
        ExtractRawShader(ssgi, "const char* kSsgiTemporalHLSL");
    EXPECT_TRUE(!taa_resolve.empty());
    EXPECT_TRUE(!ssr_render.empty());
    EXPECT_TRUE(!ssgi_render.empty());
    EXPECT_TRUE(!ssgi_temporal.empty());
    if (taa_resolve.empty() || ssr_render.empty() ||
        ssgi_render.empty() || ssgi_temporal.empty()) {
        return;
    }

    EXPECT_TRUE(taa_resolve.find(
        "ResolveTemporalHistoryFrame(") != std::string::npos);
    EXPECT_TRUE(taa_resolve.find(
        "temporal_params.taa_blend_factor =") != std::string::npos);
    EXPECT_TRUE(taa_resolve.find(
        "temporal.current_frame_weight") != std::string::npos);
    EXPECT_TRUE(taa_resolve.find(
        "temporal_params.taa_motion_texture =") != std::string::npos);
    EXPECT_TRUE(taa_resolve.find(
        "r.prev_view_proj = temporal.previous_view_projection;") !=
                std::string::npos);
    EXPECT_TRUE(taa_resolve.find(
        "IRhiTexture* slot2_tex = temporal.motion_vectors_enabled") !=
                std::string::npos);

    EXPECT_TRUE(ssr_render.find(
        "ResolveTemporalHistoryFrame(") != std::string::npos);
    EXPECT_TRUE(ssr_render.find(
        "data.prev_view_proj = temporal.previous_view_projection;") !=
                std::string::npos);
    EXPECT_TRUE(ssr_render.find(
        "temporal.current_frame_weight") != std::string::npos);
    EXPECT_TRUE(ssr_render.find(
        "temporal.motion_vectors_enabled ? 1.0f : 0.0f") !=
                std::string::npos);

    EXPECT_TRUE(ssgi_render.find(
        "ResolveTemporalHistoryFrame(") != std::string::npos);
    EXPECT_TRUE(ssgi_render.find(
        "data.prev_view_proj = temporal.previous_view_projection;") !=
                std::string::npos);
    EXPECT_TRUE(ssgi_render.find(
        "temporal.current_frame_weight") != std::string::npos);
    EXPECT_TRUE(ssgi_render.find(
        "temporal.motion_vectors_enabled ? 1.0f : 0.0f") !=
                std::string::npos);
    EXPECT_TRUE(ssgi_temporal.find(
        "float current_weight = saturate(temporal_params.z);") !=
                std::string::npos);
    EXPECT_TRUE(ssgi_temporal.find(
        "float current_weight = 0.10;") == std::string::npos);
}

ACS_TEST(PostEffects, IncompletePassesCannotPublishStaleTemporalOrBloomTargets)
{
    const std::string post =
        ReadWorkspaceSource("src/render/PostProcess.cpp");
    EXPECT_TRUE(!post.empty());
    if (post.empty()) return;

    const std::string render =
        ExtractFunction(post, "void CPostProcess::Render");
    const std::string scene_input =
        ExtractFunction(post, "IRhiTexture* CPostProcess::SceneInput");
    const std::string tonemap =
        ExtractFunction(post, "bool CPostProcess::Pass_Tonemap");
    const std::string luma =
        ExtractFunction(post, "bool CPostProcess::Pass_LumaReduce");
    EXPECT_TRUE(!render.empty());
    EXPECT_TRUE(!scene_input.empty());
    EXPECT_TRUE(!tonemap.empty());
    EXPECT_TRUE(!luma.empty());
    if (render.empty() || scene_input.empty() ||
        tonemap.empty() || luma.empty()) {
        return;
    }

    const std::size_t reset_bloom =
        render.find("m_BloomOutputValid = false;");
    const std::size_t reset_taa =
        render.find("m_TaaOutputValid = false;");
    const std::size_t reset_exposure =
        render.find("m_ExposureOutputValid = false;");
    const std::size_t early_guard =
        render.find("if (!m_HdrRt || !m_PipeExtract) return;");
    EXPECT_TRUE(reset_bloom != std::string::npos);
    EXPECT_TRUE(reset_taa != std::string::npos);
    EXPECT_TRUE(reset_exposure != std::string::npos);
    EXPECT_TRUE(early_guard != std::string::npos);
    EXPECT_TRUE(reset_bloom < early_guard);
    EXPECT_TRUE(reset_taa < early_guard);
    EXPECT_TRUE(reset_exposure < early_guard);

    EXPECT_TRUE(render.find(
        "m_TaaOutputValid = Pass_TaaResolve(cmd, safe_params);") !=
                std::string::npos);
    EXPECT_TRUE(render.find(
        "m_ExposureOutputValid =\n"
        "                Pass_ExposureApply(cmd, *exposure_source);") !=
                std::string::npos);
    EXPECT_TRUE(render.find(
        "m_BloomOutputValid = bloom_complete;") !=
                std::string::npos);
    EXPECT_TRUE(render.find(
        "if (m_TaaOutputValid) {\n"
        "        m_TaaFrame++;") != std::string::npos);
    EXPECT_TRUE(render.find(
        "if (m_ExposureOutputValid) {\n"
        "        m_AutoFrame++;") != std::string::npos);

    EXPECT_TRUE(scene_input.find(
        "p.auto_exposure_enabled && m_ExposureOutputValid") !=
                std::string::npos);
    EXPECT_TRUE(scene_input.find(
        "p.taa_enabled && m_TaaOutputValid") !=
                std::string::npos);
    EXPECT_TRUE(tonemap.find(
        "IRhiTexture* bloom_input =") != std::string::npos);
    EXPECT_TRUE(tonemap.find(
        "m_BloomOutputValid && m_BloomMips[0]") !=
                std::string::npos);
    EXPECT_TRUE(tonemap.find(
        "? m_BloomMips[0].Get() : m_BlackFb.Get();") !=
                std::string::npos);
    EXPECT_TRUE(tonemap.find(
        "cmd.SetTexture(1, *bloom_input);") !=
                std::string::npos);

    // A missing reduction level is a failed frame, never permission to adapt
    // from the previous contents of the deepest luminance texture.
    EXPECT_TRUE(luma.find(
        "if (!src || !dst) return false;") !=
                std::string::npos);
    EXPECT_TRUE(luma.find(
        "if (!src || !dst) continue;") ==
                std::string::npos);
}

ACS_TEST(PostEffects, FxaaUsesBoundedLongEdgeSearchAndSubpixelCoverage)
{
    const std::string source =
        ReadWorkspaceSource("src/render/Fxaa.cpp");
    const std::string shader =
        ExtractRawShader(source, "const char* kFxaaHLSL");
    EXPECT_TRUE(!shader.empty());
    if (shader.empty()) return;

    EXPECT_TRUE(shader.find("float edgeHorizontal") != std::string::npos);
    EXPECT_TRUE(shader.find("float edgeVertical") != std::string::npos);
    EXPECT_TRUE(shader.find("lN  = SampleLuma") != std::string::npos);
    EXPECT_TRUE(shader.find("lS  = SampleLuma") != std::string::npos);
    EXPECT_TRUE(shader.find("lW  = SampleLuma") != std::string::npos);
    EXPECT_TRUE(shader.find("lE  = SampleLuma") != std::string::npos);

    EXPECT_TRUE(shader.find("kSearchStep[12]") != std::string::npos);
    EXPECT_TRUE(shader.find(
        "for (int i = 0; i < 12; ++i)") != std::string::npos);
    EXPECT_EQ(
        CountOccurrences(shader, "for (int i = 0; i < 12; ++i)"),
        std::size_t{1});
    EXPECT_TRUE(shader.find("reachedNegative && reachedPositive") !=
                std::string::npos);
    EXPECT_TRUE(shader.find("uvNegative -= edgeStep") !=
                std::string::npos);
    EXPECT_TRUE(shader.find("uvPositive += edgeStep") !=
                std::string::npos);

    EXPECT_TRUE(shader.find(
        "return clamp(uv, px * 0.5, 1.0 - px * 0.5);") !=
        std::string::npos);
    EXPECT_TRUE(shader.find("max(w, 1u)") != std::string::npos);
    EXPECT_TRUE(shader.find("max(h, 1u)") != std::string::npos);
    EXPECT_TRUE(shader.find("subpixel * subpixel * 0.75") !=
                std::string::npos);
    EXPECT_TRUE(shader.find(
        "min(max(edgeOffset, subpixel), 0.5)") !=
        std::string::npos);
    EXPECT_TRUE(shader.find(
        "return SampleColor(uv + normalStep * finalOffset, px);") !=
        std::string::npos);

    EXPECT_TRUE(shader.find("float rcpDirMin") == std::string::npos);
    EXPECT_TRUE(shader.find("float3 rgbA") == std::string::npos);
}

ACS_TEST(PostEffects, SubsurfaceAuthoringParamsRejectUnsafeValues)
{
    FSubsurfaceScatteringParams params{};
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    params.radius_world = nan;
    params.channel_radius = FVec3{-2.0f, infinity, nan};
    params.strength = 4.0f;
    params.depth_sigma = -10.0f;
    params.normal_power = infinity;
    params.max_radius_pixels = 10000.0f;
    params.Sanitize();

    EXPECT_NEAR(params.radius_world, 0.012f, 1e-6f);
    EXPECT_NEAR(params.channel_radius.x, 0.05f, 1e-6f);
    EXPECT_NEAR(params.channel_radius.y, 0.55f, 1e-6f);
    EXPECT_NEAR(params.channel_radius.z, 0.25f, 1e-6f);
    EXPECT_NEAR(params.strength, 1.0f, 1e-6f);
    EXPECT_NEAR(params.depth_sigma, 1e-6f, 1e-9f);
    EXPECT_NEAR(params.normal_power, 24.0f, 1e-6f);
    EXPECT_NEAR(params.max_radius_pixels, 128.0f, 1e-6f);
}

ACS_TEST(PostEffects, SubsurfaceDiffusionIsBilateralEnergyStableAndDiffuseOnly)
{
    const std::string source =
        ReadWorkspaceSource("src/render/SubsurfaceScattering.cpp");
    const std::string header =
        ReadWorkspaceSource("src/render/SubsurfaceScattering.h");
    const std::string module =
        ReadWorkspaceSource("src/render/Module.cmake");
    const std::string shader =
        ExtractRawShader(
            source, "const char* kSubsurfaceScatteringHlsl");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!header.empty());
    EXPECT_TRUE(!module.empty());
    EXPECT_TRUE(!shader.empty());
    if (source.empty() || header.empty() || module.empty() ||
        shader.empty()) {
        return;
    }

    EXPECT_TRUE(shader.find(
        "Texture2D scene_depth     : register(t1)") !=
        std::string::npos);
    EXPECT_TRUE(shader.find(
        "Texture2D normal_gbuffer  : register(t2)") !=
        std::string::npos);
    EXPECT_TRUE(shader.find(
        "Texture2D material_data   : register(t3)") !=
        std::string::npos);
    EXPECT_TRUE(shader.find(
        "Texture2D original_diffuse : register(t4)") !=
        std::string::npos);
    EXPECT_TRUE(shader.find(
        "Texture2D scene_color      : register(t5)") !=
        std::string::npos);

    EXPECT_TRUE(shader.find("kOffsets[6]") != std::string::npos);
    EXPECT_TRUE(shader.find(
        "for (int tap = 0; tap < 6; ++tap)") !=
        std::string::npos);
    EXPECT_TRUE(shader.find(
        "for (int side = 0; side < 2; ++side)") !=
        std::string::npos);
    EXPECT_TRUE(shader.find("float pixel_radius = clamp(") !=
                std::string::npos);
    EXPECT_TRUE(shader.find("max(profile.w, 1.0)") !=
                std::string::npos);

    EXPECT_TRUE(shader.find("float normal_weight = pow(") !=
                std::string::npos);
    EXPECT_TRUE(shader.find("float plane_delta = max(") !=
                std::string::npos);
    EXPECT_TRUE(shader.find("float depth_weight = exp2(") !=
                std::string::npos);
    EXPECT_TRUE(shader.find(
        "float3 center_radii = ResolveProfileRadii") !=
                std::string::npos);
    EXPECT_TRUE(shader.find(
        "float3 sample_radii =") !=
                std::string::npos);
    EXPECT_TRUE(shader.find(
        "float ProfileSimilarity(") !=
                std::string::npos);
    EXPECT_TRUE(shader.find(
        "float3 radial_weight =") !=
                std::string::npos);
    EXPECT_TRUE(shader.find(
        "sample_world_offset, pair_radii") !=
                std::string::npos);
    EXPECT_TRUE(shader.find(
        "saturate(center_material.a)") !=
                std::string::npos);
    EXPECT_TRUE(shader.find(
        "saturate(sample_material.a)") !=
                std::string::npos);

    EXPECT_TRUE(shader.find(
        "accumulated / max(normalization, 1e-5)") !=
        std::string::npos);
    EXPECT_TRUE(shader.find(
        "scene.rgb + (blurred - original) * mix_strength") !=
        std::string::npos);
    EXPECT_TRUE(shader.find("float4 PSBlur") != std::string::npos);
    EXPECT_TRUE(shader.find("float4 PSComposite") !=
                std::string::npos);

    EXPECT_TRUE(source.find(
        "m_HorizontalCb->Update(&horizontal") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "m_VerticalCb->Update(&vertical") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "command_list.SetConstantBuffer(0, *m_HorizontalCb)") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "command_list.SetConstantBuffer(0, *m_VerticalCb)") !=
        std::string::npos);

    EXPECT_TRUE(header.find(
        "opaque lighting + SSS buffers -> CSubsurfaceScattering") !=
        std::string::npos);
    EXPECT_TRUE(header.find(
        "scene-linear TAA -> exposure -> bloom -> tone map") !=
        std::string::npos);
    EXPECT_TRUE(module.find("SubsurfaceScattering.cpp") !=
                std::string::npos);
    EXPECT_TRUE(module.find("SubsurfaceScattering.h") !=
                std::string::npos);
}

ACS_TEST(PostEffects, PbrSubsurfaceMrtIsOptionalAsyncAndOrderedBeforeAtmosphere)
{
    const std::string pbr =
        ReadWorkspaceSource("src/render/PbrShader.cpp");
    const std::string ssss =
        ReadWorkspaceSource("src/render/SubsurfaceScattering.cpp");
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    const std::string legacy =
        ReadWorkspaceSource("src/gameframework/LegacyScene3DAdapter.cpp");
    const std::string legacy_header =
        ReadWorkspaceSource("src/gameframework/LegacyScene3DAdapter.h");
    const std::string rhi =
        ReadWorkspaceSource("src/render/IRhiCommandList.h");
    EXPECT_TRUE(!pbr.empty());
    EXPECT_TRUE(!ssss.empty());
    EXPECT_TRUE(!editor.empty());
    EXPECT_TRUE(!legacy.empty());
    EXPECT_TRUE(!legacy_header.empty());
    EXPECT_TRUE(!rhi.empty());
    if (pbr.empty() || ssss.empty() || editor.empty() ||
        legacy.empty() || legacy_header.empty() || rhi.empty()) {
        return;
    }

    EXPECT_TRUE(pbr.find("PbrSsssMrtOutput PSMainSsss") !=
                std::string::npos);
    EXPECT_TRUE(pbr.find("float4 scene_color : SV_TARGET0") !=
                std::string::npos);
    EXPECT_TRUE(pbr.find("float4 diffuse_lighting : SV_TARGET1") !=
                std::string::npos);
    EXPECT_TRUE(pbr.find("float4 ssss_material : SV_TARGET2") !=
                std::string::npos);
    EXPECT_TRUE(pbr.find("float4 world_normal : SV_TARGET3") !=
                std::string::npos);
    EXPECT_TRUE(pbr.find(
        "outputs.world_normal = float4(normalize(N), 1.0)") !=
                std::string::npos);
    EXPECT_TRUE(pbr.find("EvaluatePbr(v, true).scene_color") !=
                std::string::npos);
    EXPECT_TRUE(pbr.find("EvaluatePbr(v, false)") !=
                std::string::npos);
    EXPECT_TRUE(pbr.find(
        "coverage > 1e-4") != std::string::npos);
    EXPECT_TRUE(pbr.find(
        ": float4(0, 0, 0, 0)") != std::string::npos);
    EXPECT_TRUE(pbr.find(
        "float3 scatter_radius_world = min(") !=
        std::string::npos);
    EXPECT_TRUE(pbr.find(
        "? float4(scatter_radius_world, coverage)") !=
        std::string::npos);
    EXPECT_TRUE(pbr.find(
        "ssss_pd.rt_formats[2] = EFormat::R16G16B16A16_Float") !=
        std::string::npos);
    EXPECT_TRUE(pbr.find("ssss_pd.rt_count = 4u") !=
                std::string::npos);
    EXPECT_TRUE(pbr.find(
        "ssss_pd.rt_formats[3] = EFormat::R16G16B16A16_Float") !=
        std::string::npos);
    EXPECT_TRUE(editor.find(
        "material_desc.format = EFormat::R16G16B16A16_Float") !=
        std::string::npos);

    EXPECT_TRUE(ssss.find(
        "CSubsurfaceScattering::CompileShadersCpu") !=
        std::string::npos);
    EXPECT_TRUE(ssss.find(
        "CSubsurfaceScattering::BeginCompileShadersAsync") !=
        std::string::npos);
    EXPECT_TRUE(ssss.find(
        "CSubsurfaceScattering::InitWithCompiledShaders") !=
        std::string::npos);
    EXPECT_TRUE(editor.find("AdvanceRuntimeSsss(") !=
                std::string::npos);
    EXPECT_TRUE(editor.find(
        "host.ssss3d.Init(*device") == std::string::npos);
    EXPECT_TRUE(editor.find(
        "scene_has_ssss && dvCount > 0u") !=
        std::string::npos);
    EXPECT_TRUE(editor.find(
        "h.pbr3d.HasSubsurfaceMrtPipeline()") !=
        std::string::npos);
    EXPECT_TRUE(editor.find("IRhiTexture* ssss_targets[4]") !=
                std::string::npos);
    EXPECT_TRUE(editor.find("h.normal_rt.Get()") !=
                std::string::npos);

    const std::size_t draw_scene = editor.find("void DrawScene3D");
    EXPECT_TRUE(draw_scene != std::string::npos);
    if (draw_scene != std::string::npos) {
        const std::string frame = editor.substr(draw_scene);
        const std::size_t mrt_draw =
            frame.find("DrawEditorPbrNode(");
        const std::size_t ssss_render =
            frame.find("h.ssss3d.Render(");
        const std::size_t water =
            frame.find("DrawInteractiveWater3DPass(");
        const std::size_t atmosphere =
            frame.find("CompositeAerialPerspective(");
        EXPECT_TRUE(mrt_draw != std::string::npos);
        EXPECT_TRUE(editor.find("DrawMeshSubsurfaceMrt(") !=
                    std::string::npos);
        EXPECT_TRUE(ssss_render != std::string::npos);
        EXPECT_TRUE(water != std::string::npos);
        EXPECT_TRUE(atmosphere != std::string::npos);
        EXPECT_TRUE(mrt_draw < ssss_render);
        EXPECT_TRUE(ssss_render < water);
        EXPECT_TRUE(ssss_render < atmosphere);
        EXPECT_TRUE(frame.find(
            "EndRenderToTextureMrt(ssss_targets, 4u)") !=
            std::string::npos);
        EXPECT_TRUE(frame.find(
            "ssss_mrt_draws_valid &&") !=
            std::string::npos);
    }

    EXPECT_TRUE(legacy.find("ScanSceneRenderFeatures(") !=
                std::string::npos);
    EXPECT_TRUE(legacy.find(
        "CPbrShader::CompileShadersCpu(false)") !=
                std::string::npos);
    EXPECT_TRUE(legacy.find(
        "CPbrShader::CompileShadersCpu(true)") !=
                std::string::npos);
    EXPECT_TRUE(legacy.find(
        "BuildPipelineCandidateForRawDx12(") !=
                std::string::npos);
    EXPECT_TRUE(legacy.find(
        "InitPipelineResourcesWithCompiledShaders(") !=
                std::string::npos);
    EXPECT_TRUE(legacy.find("IRhiTexture* ssss_targets[4]") !=
                std::string::npos);
    EXPECT_TRUE(legacy.find(
        "BeginRenderToTextureMrtLoad(") != std::string::npos);
    EXPECT_TRUE(legacy.find(
        "EndRenderToTextureMrt(ssss_targets, 4u)") !=
                std::string::npos);
    EXPECT_TRUE(legacy.find(
        "scene_has_water\n        || (scene_needs_subsurface") !=
                std::string::npos);
    const std::string legacy_frame = ExtractFunction(
        legacy, "void ALegacyScene3DAdapter::OnRender(");
    const std::string legacy_sky = ExtractFunction(
        legacy, "void ALegacyScene3DAdapter::RenderSky(");
    EXPECT_TRUE(!legacy_frame.empty());
    EXPECT_TRUE(!legacy_sky.empty());
    if (!legacy_frame.empty() && !legacy_sky.empty()) {
        // 空の実装を補助関数へ移しても、フレーム上の呼び出し順を検証できるようにする。
        const std::size_t hdr_begin = legacy_frame.find(
            "command_list.BeginRenderToTexture(");
        const std::size_t sky = legacy_frame.find("RenderSky(", hdr_begin);
        const std::size_t sky_end = legacy_frame.find(
            "EndRenderToTexture(*hdr)", sky);
        const std::size_t mrt = legacy_frame.find(
            "BeginRenderToTextureMrtLoad(", sky_end);
        const std::size_t ssss_render = legacy_frame.find(
            "m_Ssss.Render(", mrt);
        const std::size_t blit = legacy_frame.find(
            "m_Blit.Copy(", ssss_render);
        const std::size_t water = legacy_frame.find(
            "DrawWaterScene(", blit);
        const std::size_t clouds = legacy_frame.find(
            "CompositeClouds(", water);
        const std::size_t reflection = legacy_frame.find(
            "RenderReflectionPass(", clouds);
        const std::size_t post = legacy_frame.find(
            "m_Post.Render(", reflection);
        EXPECT_TRUE(hdr_begin != std::string::npos);
        EXPECT_TRUE(sky != std::string::npos);
        EXPECT_TRUE(sky_end != std::string::npos);
        EXPECT_TRUE(mrt != std::string::npos);
        EXPECT_TRUE(ssss_render != std::string::npos);
        EXPECT_TRUE(blit != std::string::npos);
        EXPECT_TRUE(water != std::string::npos);
        EXPECT_TRUE(clouds != std::string::npos);
        EXPECT_TRUE(reflection != std::string::npos);
        EXPECT_TRUE(post != std::string::npos);
        EXPECT_TRUE(hdr_begin < sky);
        EXPECT_TRUE(sky < sky_end);
        EXPECT_TRUE(sky_end < mrt);
        EXPECT_TRUE(mrt < ssss_render);
        EXPECT_TRUE(ssss_render < blit);
        EXPECT_TRUE(blit < water);
        EXPECT_TRUE(water < clouds);
        EXPECT_TRUE(clouds < reflection);
        EXPECT_TRUE(reflection < post);
        EXPECT_TRUE(legacy_sky.find("m_Sky.Render(") != std::string::npos);
    }

    EXPECT_TRUE(rhi.find("EndRenderToTextureMrt(") !=
                std::string::npos);
}

ACS_TEST(PostEffects, EditorSsssHotRemoveDrainsCompletedWorkWithoutBlocking)
{
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    EXPECT_TRUE(!editor.empty());
    if (editor.empty()) return;

    const std::size_t function_begin =
        editor.find("bool AdvanceRuntimeSsss(");
    const std::size_t function_end =
        editor.find("bool EnsureSsssFrameResources(", function_begin);
    EXPECT_TRUE(function_begin != std::string::npos);
    EXPECT_TRUE(function_end != std::string::npos);
    if (function_begin == std::string::npos ||
        function_end == std::string::npos) {
        return;
    }

    const std::string function =
        editor.substr(function_begin, function_end - function_begin);
    const std::size_t hot_remove = function.find(
        "if (!requested && host.ssss3d_init_state == 1u)");
    const std::size_t request_gate = function.find(
        "if (!requested) {", hot_remove);
    EXPECT_TRUE(hot_remove != std::string::npos);
    EXPECT_TRUE(request_gate != std::string::npos);
    EXPECT_TRUE(hot_remove < request_gate);
    if (hot_remove == std::string::npos ||
        request_gate == std::string::npos) {
        return;
    }

    const std::string drain =
        function.substr(hot_remove, request_gate - hot_remove);
    const std::size_t raw_owner =
        drain.find("host.startup_worker_kind != 6u");
    const std::size_t poll =
        drain.find("PollStartupWorker(host)");
    const std::size_t running =
        drain.find("if (worker_result == 0) return false", poll);
    const std::size_t clear_kind =
        drain.find("host.startup_worker_kind = 0u", running);
    const std::size_t clear_device =
        drain.find("host.startup_ssss_candidate_device = nullptr",
                   clear_kind);
    const std::size_t shutdown =
        drain.find("host.ssss3d.Shutdown()", clear_device);
    const std::size_t diligent_status =
        drain.find("host.ssss3d_pending_shaders.Status()");
    const std::size_t diligent_running =
        drain.find(
            "shader_status == EShaderStatus::Compiling",
            diligent_status);
    const std::size_t discard =
        drain.find("host.ssss3d_pending_shaders = {}");
    const std::size_t reset =
        drain.find("host.ssss3d_init_state = 0u", discard);
    const std::size_t retry =
        drain.find("host.ssss3d_init_failed = false", reset);
    EXPECT_TRUE(raw_owner != std::string::npos);
    EXPECT_TRUE(poll != std::string::npos);
    EXPECT_TRUE(running != std::string::npos);
    EXPECT_TRUE(clear_kind != std::string::npos);
    EXPECT_TRUE(clear_device != std::string::npos);
    EXPECT_TRUE(shutdown != std::string::npos);
    EXPECT_TRUE(diligent_status != std::string::npos);
    EXPECT_TRUE(diligent_running != std::string::npos);
    EXPECT_TRUE(discard != std::string::npos);
    EXPECT_TRUE(reset != std::string::npos);
    EXPECT_TRUE(retry != std::string::npos);
    EXPECT_TRUE(raw_owner < poll);
    EXPECT_TRUE(poll < running);
    EXPECT_TRUE(running < clear_kind);
    EXPECT_TRUE(clear_kind < clear_device);
    EXPECT_TRUE(clear_device < shutdown);
    EXPECT_TRUE(discard < reset);
    EXPECT_TRUE(reset < retry);
    EXPECT_TRUE(drain.find("InitPipelineResourcesWithCompiledShaders(") ==
                std::string::npos);
    EXPECT_TRUE(drain.find("Resize(") == std::string::npos);

    const std::size_t poll_begin =
        editor.find("i32 PollStartupWorker(");
    const std::size_t poll_end =
        editor.find("void JoinStartupWorker(", poll_begin);
    EXPECT_TRUE(poll_begin != std::string::npos);
    EXPECT_TRUE(poll_end != std::string::npos);
    if (poll_begin != std::string::npos &&
        poll_end != std::string::npos) {
        const std::string poll_function =
            editor.substr(poll_begin, poll_end - poll_begin);
        const std::size_t acquire =
            poll_function.find("std::memory_order_acquire");
        const std::size_t nonblocking =
            poll_function.find("if (state == 1) return 0", acquire);
        const std::size_t join =
            poll_function.find("h.startup_worker.Join()", nonblocking);
        EXPECT_TRUE(acquire != std::string::npos);
        EXPECT_TRUE(nonblocking != std::string::npos);
        EXPECT_TRUE(join != std::string::npos);
        EXPECT_TRUE(acquire < nonblocking);
        EXPECT_TRUE(nonblocking < join);
    }
}

ACS_TEST(PostEffects, EditorPostStartupCompilesOffOwnerThreadAndFailsOpen)
{
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");
    EXPECT_TRUE(!editor.empty());
    if (editor.empty()) return;

    EXPECT_TRUE(editor.find(
        "CPostProcess::CompileShadersCpu()") != std::string::npos);
    EXPECT_TRUE(editor.find(
        "CPostProcess::BeginCompileShadersAsync(*dev)") !=
                std::string::npos);
    EXPECT_TRUE(editor.find(
        "h.startup_worker_kind = 7u;") != std::string::npos);
    EXPECT_TRUE(editor.find(
        "h.startup_async_shader_kind = 7u;") != std::string::npos);
    EXPECT_TRUE(editor.find(
        "host->startup_post_shaders = {};") != std::string::npos);

    const std::size_t phase_begin =
        editor.find("if (h.r3d_init_phase == 15u)");
    const std::size_t phase_end =
        editor.find("if (h.r3d_init_phase == 16u)", phase_begin);
    EXPECT_TRUE(phase_begin != std::string::npos);
    EXPECT_TRUE(phase_end != std::string::npos);
    if (phase_begin == std::string::npos ||
        phase_end == std::string::npos) {
        return;
    }

    const std::string phase =
        editor.substr(phase_begin, phase_end - phase_begin);
    const std::size_t async_submit =
        phase.find("CPostProcess::BeginCompileShadersAsync(*dev)");
    const std::size_t async_pending =
        phase.find("shader_status == EShaderStatus::Compiling");
    const std::size_t owner_commit =
        phase.find("h.post3d.InitWithCompiledShaders(");
    const std::size_t neutral_failure =
        phase.find("continuing without the post stack");
    EXPECT_TRUE(async_submit != std::string::npos);
    EXPECT_TRUE(async_pending != std::string::npos);
    EXPECT_TRUE(owner_commit != std::string::npos);
    EXPECT_TRUE(neutral_failure != std::string::npos);
    EXPECT_TRUE(async_pending < owner_commit);
    EXPECT_TRUE(phase.find(
        "h.startup_phase_pending = true;") != std::string::npos);
    EXPECT_TRUE(phase.find(
        "BeginPostCompileWorker(h)") != std::string::npos);
    EXPECT_TRUE(phase.find(
        "if (use_sync_fallback)") != std::string::npos);

    // Normal rendering may resize an initialized post stack, but it must not
    // restart eleven synchronous shader compilations after startup failed or
    // while a backend-managed compiler is still pending.
    EXPECT_TRUE(editor.find("h.post3d.Init(*pdev") ==
                std::string::npos);
    EXPECT_TRUE(editor.find(
        "if (pdev != nullptr && h.post3d_ready") !=
                std::string::npos);
}

ACS_TEST(PostEffects, RawDx12RetirementIsMainSubmitOrderedAndFailureSafe)
{
    const std::string device_header =
        ReadWorkspaceSource("src/render/Dx12/Dx12Device.h");
    const std::string device =
        ReadWorkspaceSource("src/render/Dx12/Dx12Device.cpp");
    const std::string texture =
        ReadWorkspaceSource("src/render/Dx12/Dx12Texture.cpp");
    const std::string buffer =
        ReadWorkspaceSource("src/render/Dx12/Dx12Buffer.cpp");
    const std::string command =
        ReadWorkspaceSource("src/render/Dx12/Dx12CommandList.cpp");
    EXPECT_TRUE(!device_header.empty());
    EXPECT_TRUE(!device.empty());
    EXPECT_TRUE(!texture.empty());
    EXPECT_TRUE(!buffer.empty());
    EXPECT_TRUE(!command.empty());
    if (device_header.empty() || device.empty() || texture.empty() ||
        buffer.empty() || command.empty()) {
        return;
    }

    auto section = [](const std::string& source,
                      const char* begin_token,
                      const char* end_token) {
        const std::size_t begin = source.find(begin_token);
        const std::size_t end =
            begin == std::string::npos
                ? std::string::npos
                : source.find(end_token, begin);
        if (begin == std::string::npos || end == std::string::npos ||
            end <= begin) {
            return std::string{};
        }
        return source.substr(begin, end - begin);
    };

    const std::string queue = section(
        device,
        "void CDx12Device::QueueRetiredResource(",
        "void CDx12Device::RetireResource(");
    EXPECT_TRUE(!queue.empty());
    EXPECT_TRUE(queue.find("m_RetiredResources.TryAdd") !=
                std::string::npos);
    EXPECT_TRUE(queue.find("m_EmergencyRetiredResources[") !=
                std::string::npos);
    EXPECT_TRUE(queue.find("kEmergencyRetirementCapacity") !=
                std::string::npos);
    EXPECT_TRUE(queue.find("WaitIdle();") == std::string::npos);
    EXPECT_TRUE(queue.find("ReleaseRetiredResource(retired)") ==
                std::string::npos);
    EXPECT_TRUE(queue.find("currently open/recorded command list") !=
                std::string::npos);
    EXPECT_TRUE(queue.find("retired.resource = nullptr") !=
                std::string::npos);

    const std::string submission = section(
        device,
        "u64 CDx12Device::ExecuteGraphicsCommandListsAndSignal(",
        "u64 CDx12Device::SubmitGraphicsCommandLists(");
    EXPECT_TRUE(!submission.empty());
    const std::size_t retirement_lock =
        submission.find("retirement_guard(m_RetirementLock)");
    const std::size_t submission_lock =
        submission.find("submission_guard(m_QueueSubmissionLock)");
    const std::size_t queue_execute =
        submission.find("m_GfxQueue->ExecuteCommandLists(");
    const std::size_t queue_signal =
        submission.find("SignalGraphicsQueueLocked()");
    const std::size_t submission_signal_success =
        submission.find("if (fence_value != 0u)", queue_signal);
    const std::size_t seal =
        submission.find(
            "SealPendingRetirements(fence_value)",
            submission_signal_success);
    EXPECT_TRUE(retirement_lock != std::string::npos);
    EXPECT_TRUE(submission_lock != std::string::npos);
    EXPECT_TRUE(queue_execute != std::string::npos);
    EXPECT_TRUE(queue_signal != std::string::npos);
    EXPECT_TRUE(submission_signal_success != std::string::npos);
    EXPECT_TRUE(seal != std::string::npos);
    EXPECT_TRUE(retirement_lock < submission_lock);
    EXPECT_TRUE(submission_lock < queue_execute);
    EXPECT_TRUE(queue_execute < queue_signal);
    EXPECT_TRUE(queue_signal < submission_signal_success);
    EXPECT_TRUE(submission_signal_success < seal);
    EXPECT_TRUE(submission.find(
        "On Signal failure the executed work has no completion proof") !=
                std::string::npos);

    const std::string main_submit = section(
        device,
        "u64 CDx12Device::SubmitGraphicsCommandLists(",
        "u64 CDx12Device::ExecuteOneOffGraphicsCommandList(");
    const std::string one_off_submit = section(
        device,
        "u64 CDx12Device::ExecuteOneOffGraphicsCommandList(",
        "// Generic queue waits never seal retirement records.");
    EXPECT_TRUE(main_submit.find(
        "command_lists, command_list_count, true") != std::string::npos);
    EXPECT_TRUE(one_off_submit.find(
        "lists, 1u, false") != std::string::npos);

    const std::string generic_signal = section(
        device,
        "u64 CDx12Device::SignalGraphicsQueue()",
        "void CDx12Device::WaitForFenceValue(");
    EXPECT_TRUE(!generic_signal.empty());
    EXPECT_TRUE(generic_signal.find(
        "submission_guard(m_QueueSubmissionLock)") != std::string::npos);
    EXPECT_TRUE(generic_signal.find("SealPendingRetirements(") ==
                std::string::npos);

    const std::string reset = section(
        device,
        "void CDx12Device::Reset()",
        "FHrResult CDx12Device::InitDescriptorHeaps()");
    EXPECT_TRUE(!reset.empty());
    const std::size_t final_signal =
        reset.find("const u64 final_fence = SignalGraphicsQueue()");
    const std::size_t signal_success =
        reset.find("if (final_fence != 0u)", final_signal);
    const std::size_t final_wait =
        reset.find("WaitForFenceValue(final_fence)", signal_success);
    const std::size_t completion_check =
        reset.find("completed >= final_fence", final_wait);
    const std::size_t final_release =
        reset.find("ReleaseAllRetiredResources()", completion_check);
    const std::size_t abandon_on_failure = reset.find("if (!final_signal_completed && m_GfxQueue != nullptr)");
    const std::size_t abandon =
        reset.find("AbandonAllRetiredResources()", abandon_on_failure);
    const std::size_t pipeline_cache_reset = reset.find("ResetPipelineCache(final_signal_completed || m_GfxQueue == nullptr)", abandon);
    EXPECT_TRUE(final_signal != std::string::npos);
    EXPECT_TRUE(signal_success != std::string::npos);
    EXPECT_TRUE(final_wait != std::string::npos);
    EXPECT_TRUE(completion_check != std::string::npos);
    EXPECT_TRUE(final_release != std::string::npos);
    EXPECT_TRUE(abandon_on_failure != std::string::npos);
    EXPECT_TRUE(abandon != std::string::npos);
    EXPECT_TRUE(pipeline_cache_reset != std::string::npos);
    EXPECT_TRUE(final_signal < signal_success);
    EXPECT_TRUE(signal_success < final_wait);
    EXPECT_TRUE(final_wait < completion_check);
    EXPECT_TRUE(completion_check < final_release);
    EXPECT_TRUE(final_release < abandon_on_failure);
    EXPECT_TRUE(abandon < pipeline_cache_reset);
    EXPECT_TRUE(reset.find("PSO と root signature を解放せず use-after-free を防ぐ", abandon) != std::string::npos);

    const std::string reset_pipeline_cache = section(device, "void CDx12Device::ResetPipelineCache(", "void CDx12Device::Reset()");
    EXPECT_TRUE(!reset_pipeline_cache.empty());
    // outer lock は owner の読取・切離し・Delete の全寿命を覆う。
    const std::size_t cache_outer_lock = reset_pipeline_cache.find("FExclusiveLockGuard cache_guard(m_PipelineCacheLock)");
    const std::size_t owner_load = reset_pipeline_cache.find("FPipelineCacheOwner* owner = m_PipelineCacheOwner", cache_outer_lock);
    const std::size_t owner_detach = reset_pipeline_cache.find("m_PipelineCacheOwner = nullptr", owner_load);
    EXPECT_TRUE(reset_pipeline_cache.find("if (release_objects)") != std::string::npos);
    EXPECT_TRUE(reset_pipeline_cache.find("ACS_SAFE_RELEASE(owner->pipeline_states[index])") != std::string::npos);
    EXPECT_TRUE(reset_pipeline_cache.find("ACS_SAFE_RELEASE(owner->root_signatures[index])") != std::string::npos);
    EXPECT_TRUE(reset_pipeline_cache.find("owner->pipeline_states[index] = nullptr") != std::string::npos);
    EXPECT_TRUE(reset_pipeline_cache.find("owner->root_signatures[index] = nullptr") != std::string::npos);
    // 所有配列を空にしてから key table を破棄する順序。
    const std::size_t abandoned_pipeline = reset_pipeline_cache.find("owner->pipeline_states[index] = nullptr");
    const std::size_t abandoned_root = reset_pipeline_cache.find("owner->root_signatures[index] = nullptr");
    const std::size_t key_table_reset = reset_pipeline_cache.find("owner->key_cache.Reset()");
    const std::size_t owner_delete = reset_pipeline_cache.find("Delete(allocator, owner)", key_table_reset);
    EXPECT_TRUE(cache_outer_lock < owner_load);
    EXPECT_TRUE(owner_load < owner_detach);
    EXPECT_TRUE(abandoned_pipeline < key_table_reset);
    EXPECT_TRUE(abandoned_root < key_table_reset);
    EXPECT_TRUE(key_table_reset < owner_delete);

    const std::string find_pipeline_cache = section(device, "bool CDx12Device::FindCachedPipeline(", "void CDx12Device::StoreCachedPipeline(");
    const std::string store_pipeline_cache = section(device, "void CDx12Device::StoreCachedPipeline(", "void CDx12Device::ResetPipelineCache(");
    EXPECT_TRUE(find_pipeline_cache.find("FExclusiveLockGuard cache_guard(m_PipelineCacheLock)") < find_pipeline_cache.find("FPipelineCacheOwner* owner = m_PipelineCacheOwner"));
    EXPECT_TRUE(store_pipeline_cache.find("FExclusiveLockGuard cache_guard(m_PipelineCacheLock)") < store_pipeline_cache.find("FPipelineCacheOwner* owner = m_PipelineCacheOwner"));

    EXPECT_TRUE(device_header.find(
        "Lock order is Retirement -> QueueSubmission and Retirement -> Descriptor") !=
                std::string::npos);
    EXPECT_TRUE(device_header.find(
        "kEmergencyRetirementCapacity = 256u") !=
                std::string::npos);
    EXPECT_TRUE(device.find(
        "m_EmergencyRetiredResourceCount") != std::string::npos);

    const std::string texture_reset = section(
        texture,
        "void FDx12Texture::Reset()",
        "FHrResult FDx12Texture::Init(");
    EXPECT_TRUE(texture_reset.find(
        "TArray<i32> rtv_slots = Move(m_RtvSlots)") !=
                std::string::npos);
    EXPECT_TRUE(texture_reset.find(
        "device->RetireTextureResource(") != std::string::npos);
    EXPECT_TRUE(texture_reset.find("m_Device->FreeSrvSlot") ==
                std::string::npos);
    EXPECT_TRUE(texture_reset.find("ACS_SAFE_RELEASE(m_Resource)") ==
                std::string::npos);

    const std::string buffer_reset = section(
        buffer,
        "void FDx12Buffer::Reset()",
        "FHrResult FDx12Buffer::Init(");
    EXPECT_TRUE(buffer_reset.find("device->RetireResource(resource)") !=
                std::string::npos);
    EXPECT_TRUE(buffer_reset.find("ACS_SAFE_RELEASE(m_Resource)") ==
                std::string::npos);

    // One-off uploads/readback are queue ordered but never seal pending main
    // retirements. If their Signal fails after Execute, transient COM objects
    // are intentionally abandoned instead of released under in-flight work.
    EXPECT_TRUE(buffer.find(
        "device.ExecuteOneOffGraphicsCommandList(command_list)") !=
                std::string::npos);
    EXPECT_TRUE(buffer.find("command_list = nullptr;") !=
                std::string::npos);
    EXPECT_TRUE(buffer.find("allocator = nullptr;") !=
                std::string::npos);
    EXPECT_TRUE(buffer.find("staging = nullptr;") !=
                std::string::npos);
    EXPECT_TRUE(texture.find(
        "device.ExecuteOneOffGraphicsCommandList(cl)") !=
                std::string::npos);
    EXPECT_TRUE(texture.find("cl = nullptr;") != std::string::npos);
    EXPECT_TRUE(texture.find("alloc = nullptr;") != std::string::npos);
    EXPECT_TRUE(texture.find("upload = nullptr;") != std::string::npos);
    const std::string readback = section(
        device,
        "bool CDx12Device::ReadTexture(",
        "// ファクトリ関数:");
    EXPECT_TRUE(readback.find(
        "ExecuteOneOffGraphicsCommandList(cl)") != std::string::npos);
    EXPECT_TRUE(readback.find("cl = nullptr;") != std::string::npos);
    EXPECT_TRUE(readback.find("alloc = nullptr;") != std::string::npos);
    EXPECT_TRUE(readback.find("rb = nullptr;") != std::string::npos);

    const std::string submit = section(
        command,
        "bool CDx12CommandList::SubmitInternal(",
        "bool CDx12CommandList::Submit()");
    const std::size_t main_submit_call =
        submit.find("SubmitGraphicsCommandLists(lists, 1u)");
    const std::size_t wait_gate =
        submit.find("if (wait_for_next_slot)");
    const std::size_t next_slot_wait =
        submit.find("WaitForFenceValue(m_FrameFences[next_slot])");
    const std::size_t retirement_collect =
        submit.find("CollectRetiredResources()", next_slot_wait);
    EXPECT_TRUE(main_submit_call != std::string::npos);
    EXPECT_TRUE(wait_gate != std::string::npos);
    EXPECT_TRUE(next_slot_wait != std::string::npos);
    EXPECT_TRUE(retirement_collect != std::string::npos);
    EXPECT_TRUE(main_submit_call < next_slot_wait);
    EXPECT_TRUE(wait_gate < next_slot_wait);
    EXPECT_TRUE(next_slot_wait < retirement_collect);
    EXPECT_TRUE(submit.find("GraphicsQueue()->ExecuteCommandLists") ==
                std::string::npos);
    EXPECT_TRUE(submit.find("SignalGraphicsQueue()") ==
                std::string::npos);
    EXPECT_TRUE(command.find(
        "bool CDx12CommandList::SubmitWithoutGpuWait()") !=
                std::string::npos);
    EXPECT_TRUE(command.find("SubmitInternal(false);") !=
                std::string::npos);
}

ACS_TEST(RenderLifecycle,
         SubmitAndPresentFailuresReachTheEditorFrameContract)
{
    const std::string renderer =
        ReadWorkspaceSource("src/render/Renderer.cpp");
    const std::string dx12_swapchain =
        ReadWorkspaceSource("src/render/Dx12/Dx12Swapchain.cpp");
    const std::string diligent_swapchain =
        ReadWorkspaceSource("src/render/Diligent/DiligentSwapchain.cpp");
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");

    const std::string end_frame =
        ExtractFunction(renderer, "bool CRenderer::EndFrameInternal(");
    const std::string raw_present =
        ExtractFunction(dx12_swapchain, "bool FDx12Swapchain::Present()");
    const std::string diligent_present =
        ExtractFunction(
            diligent_swapchain,
            "bool FDiligentSwapchain::Present()");
    const std::string attach =
        ExtractFunction(editor, "ACS_EDITOR_API int acs_editor_attach(");

    EXPECT_TRUE(end_frame.find("if (!submitted)") != std::string::npos);
    EXPECT_TRUE(end_frame.find("if (!m_Swapchain->Present())") !=
                std::string::npos);
    EXPECT_TRUE(raw_present.find("const HRESULT present_hr") !=
                std::string::npos);
    EXPECT_TRUE(raw_present.find("GetDeviceRemovedReason()") !=
                std::string::npos);
    EXPECT_TRUE(diligent_present.find(
        "NotifyPrimaryPresentFinished()") != std::string::npos);
    EXPECT_TRUE(diligent_present.find("IsDeviceHealthy()") !=
                std::string::npos);
    EXPECT_TRUE(editor.find("int SubmitAndPresentEditorFrame(") !=
                std::string::npos);
    EXPECT_TRUE(editor.find(
        "editor_frame::ShouldPublishProfiler(present_result)") !=
                std::string::npos);
    const std::size_t neutral_present_failure =
        attach.find("if (!PresentNeutralEditorFrame(*host, false))");
    const std::size_t attach_reset =
        attach.find("host->attached = false;", neutral_present_failure);
    const std::size_t attach_shutdown =
        attach.find("host->renderer.Shutdown();", attach_reset);
    const std::size_t attach_failure_return =
        attach.find("return 0;", attach_shutdown);
    EXPECT_TRUE(neutral_present_failure != std::string::npos);
    EXPECT_TRUE(attach_reset != std::string::npos);
    EXPECT_TRUE(attach_shutdown != std::string::npos);
    EXPECT_TRUE(attach_failure_return != std::string::npos);
}

ACS_TEST(RenderLifecycle,
         ResizeAndMsaaResourceMutationShareOneIdleBoundary)
{
    const std::string renderer =
        ReadWorkspaceSource("src/render/Renderer.cpp");
    const std::string dx12_swapchain =
        ReadWorkspaceSource("src/render/Dx12/Dx12Swapchain.cpp");
    const std::string editor =
        ReadWorkspaceSource("src/editor_abi/EditorAbi.cpp");

    const std::string resize =
        ExtractFunction(renderer, "bool CRenderer::OnResize(");
    const std::string backend_resize =
        ExtractFunction(dx12_swapchain, "bool FDx12Swapchain::Resize(");

    EXPECT_EQ(CountOccurrences(resize, "WaitIdle()"), 1u);
    EXPECT_EQ(CountOccurrences(backend_resize, "WaitIdle()"), 0u);
    EXPECT_TRUE(editor.find(
        "if (!host->resource_mutation_idle)") != std::string::npos);
    EXPECT_TRUE(editor.find(
        "host->resource_mutation_idle = true;") != std::string::npos);
}

ACS_TEST(RenderLifecycle,
         ResizeFailureStopsApplicationAndEasyBeforeFrameRecording)
{
    const std::string application =
        ReadWorkspaceSource("src/app/Application.cpp");
    const std::string easy =
        ReadWorkspaceSource("src/easy/Easy.cpp");

    const std::string app_bridge =
        ExtractFunction(application, "void CApplication::EventBridge(");
    const std::string app_run =
        ExtractFunction(application, "int CApplication::Run(");
    const std::string easy_bridge =
        ExtractFunction(easy, "void EasyEventBridge(");
    const std::string easy_frame =
        ExtractFunction(easy, "bool NextFrame()");

    const std::size_t app_resize_failure =
        app_bridge.find("if (!app->m_Renderer.OnResize(");
    const std::size_t app_failure_gate =
        app_run.find("if (m_RendererFailurePending)");
    const std::size_t app_begin =
        app_run.find("m_Renderer.BeginFrame(");
    EXPECT_TRUE(app_resize_failure != std::string::npos);
    EXPECT_TRUE(app_failure_gate != std::string::npos);
    EXPECT_TRUE(app_begin != std::string::npos);
    EXPECT_TRUE(app_failure_gate < app_begin);

    const std::size_t easy_resize_failure =
        easy_bridge.find("if (!g_state.renderer.OnResize(");
    const std::string easy_post_block = ExtractFunction(easy_bridge, "if (g_state.post_available)");
    const std::string easy_post_error_block = ExtractFunction(easy_post_block, "if (post_resize.IsErr())");
    const std::string easy_renderer_failure_block = ExtractFunction(easy_bridge, "if (!g_state.renderer.OnResize(");
    const std::string easy_failure_block = ExtractFunction(easy_frame, "if (g_state.renderer_failure_pending)");
    const std::size_t easy_post_guard =
        easy_bridge.find("if (g_state.post_available)");
    const std::size_t easy_post_resize =
        easy_post_block.find("g_state.post.Resize(");
    const std::size_t easy_post_error =
        easy_post_block.find("post_resize.IsErr()");
    const std::size_t easy_post_dimensions = easy_post_error_block.find("post-process resize failed (%u x %u)");
    const std::size_t easy_post_error_message =
        easy_post_error_block.find("post_resize.Error().message");
    const std::size_t easy_post_failure =
        easy_post_error_block.find("g_state.renderer_failure_pending = true;");
    const std::size_t easy_post_return =
        easy_post_error_block.find("return;");
    const std::size_t easy_renderer_failure_pending = easy_renderer_failure_block.find("g_state.renderer_failure_pending = true;");
    const std::size_t easy_renderer_return =
        easy_renderer_failure_block.find("return;");
    const std::size_t easy_post_discarded_result =
        easy_bridge.find("(void)g_state.post.Resize");
    const std::size_t easy_failure_gate =
        easy_frame.find("if (g_state.renderer_failure_pending)");
    const std::size_t easy_first_acquire =
        easy_frame.find("AcquireNextImage()");
    const std::size_t easy_shutdown =
        easy_failure_block.find("RunShutdownOnce()");
    const std::size_t easy_stop = easy_failure_block.find("return false;", easy_shutdown);
    const std::size_t easy_begin =
        easy_frame.find("g_state.renderer.BeginFrame(");
    EXPECT_TRUE(easy_resize_failure != std::string::npos);
    EXPECT_TRUE(!easy_post_block.empty());
    EXPECT_TRUE(!easy_post_error_block.empty());
    EXPECT_TRUE(!easy_renderer_failure_block.empty());
    EXPECT_TRUE(!easy_failure_block.empty());
    EXPECT_TRUE(easy_post_guard != std::string::npos);
    EXPECT_EQ(CountOccurrences(easy_post_block, "g_state.post.Resize("), std::size_t{1u});
    EXPECT_TRUE(easy_post_resize != std::string::npos);
    EXPECT_TRUE(easy_post_error != std::string::npos);
    EXPECT_TRUE(easy_post_dimensions != std::string::npos);
    EXPECT_TRUE(easy_post_error_message != std::string::npos);
    EXPECT_TRUE(easy_post_failure != std::string::npos);
    EXPECT_TRUE(easy_post_return != std::string::npos);
    EXPECT_TRUE(easy_post_resize < easy_post_error);
    EXPECT_TRUE(easy_post_dimensions < easy_post_error_message);
    EXPECT_TRUE(easy_post_error_message < easy_post_failure);
    EXPECT_TRUE(easy_post_failure < easy_post_return);
    EXPECT_EQ(CountOccurrences(easy_post_error_block, "post-process resize failed (%u x %u)"), std::size_t{1u});
    EXPECT_EQ(CountOccurrences(easy_post_error_block, "post_resize.Error().message"), std::size_t{1u});
    EXPECT_EQ(CountOccurrences(easy_post_error_block, "g_state.renderer_failure_pending = true;"), std::size_t{1u});
    EXPECT_EQ(CountOccurrences(easy_post_error_block, "return;"), std::size_t{1u});
    EXPECT_EQ(CountOccurrences(easy_renderer_failure_block, "g_state.renderer_failure_pending = true;"), std::size_t{1u});
    EXPECT_EQ(CountOccurrences(easy_renderer_failure_block, "return;"), std::size_t{1u});
    EXPECT_TRUE(easy_renderer_failure_pending != std::string::npos);
    EXPECT_TRUE(easy_renderer_return != std::string::npos);
    EXPECT_TRUE(easy_renderer_failure_pending < easy_renderer_return);
    EXPECT_TRUE(easy_resize_failure + easy_renderer_failure_block.size() <= easy_post_guard);
    EXPECT_TRUE(easy_resize_failure < easy_post_guard);
    EXPECT_EQ(easy_post_discarded_result, std::string::npos);
    EXPECT_TRUE(easy_failure_gate != std::string::npos);
    EXPECT_TRUE(easy_first_acquire != std::string::npos);
    EXPECT_TRUE(easy_shutdown != std::string::npos);
    EXPECT_TRUE(easy_stop != std::string::npos);
    EXPECT_EQ(CountOccurrences(easy_failure_block, "RunShutdownOnce()"), std::size_t{1u});
    EXPECT_EQ(CountOccurrences(easy_failure_block, "return false;"), std::size_t{1u});
    EXPECT_TRUE(easy_failure_gate < easy_first_acquire);
    EXPECT_TRUE(easy_shutdown < easy_stop);
    EXPECT_TRUE(easy_failure_gate + easy_failure_block.size() <= easy_first_acquire);
    EXPECT_TRUE(easy_begin != std::string::npos);
    EXPECT_TRUE(easy_failure_gate < easy_begin);
}
