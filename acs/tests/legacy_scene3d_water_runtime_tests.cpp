// SPDX-License-Identifier: Apache-2.0

#include "test/Test.h"
#include "test/Expect.h"

#include "asset/MeshAsset.h"
#include "gameframework/CameraComponent3D.h"
#include "gameframework/LegacyScene3DAdapter.h"
#include "gameframework/MeshComponent3D.h"
#include "gameframework/Scene3DSerialize.h"
#include "gameframework/SceneSerialize.h"
#include "gameframework/WaterSurface3DComponent.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "math/Quat.h"
#include "memory/SharedPtr.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

using namespace acs;
using namespace acs::game;

namespace {

/** 雲天候制御を含む現在のWin64公開配置。後続機能は既存余白内へ収める。 */
constexpr usize kLegacyScene3DAdapterLayoutSize = 377408u;

/** protected更新をcamera選択回帰から実行するLegacy 3D scene。 */
class CCameraSelectionTestScene final : public ALegacyScene3DAdapter {
public:
    /** 一回の可変更新を実行する。 */
    void UpdateForTest(f32 delta_seconds) noexcept { OnUpdate(delta_seconds); }

    /** 派生sceneへ毎frame hookが届いた回数を返す。 */
    int TickCount() const noexcept { return m_TickCount; }

protected:
    /** Legacy adapterの基底更新から呼ばれる毎frame hookを数える。 */
    void OnTick(f32 /*delta_seconds*/) noexcept override { ++m_TickCount; }

private:
    int m_TickCount = 0;
};

/** workspace内のcamera adapter headerをsource契約検査用に読む。 */
std::string ReadLegacyCameraWorkspaceSource(const char* relative_path) {
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

/** 指定signatureの宣言行にvirtualが無ければtrueを返す。 */
bool IsNonVirtualDeclarationLine(
    const std::string& source,
    const char* signature) {
    const std::size_t declaration = source.find(signature);
    if (declaration == std::string::npos) return false;
    const std::size_t line_begin = source.rfind('\n', declaration);
    const std::size_t line_end = source.find('\n', declaration);
    const std::size_t begin =
        line_begin == std::string::npos ? 0u : line_begin + 1u;
    const std::size_t count =
        line_end == std::string::npos
            ? source.size() - begin : line_end - begin;
    return source.substr(begin, count).find("virtual") == std::string::npos;
}

void ExpectVec3Near(FVec3 actual, FVec3 expected, f32 epsilon) noexcept {
    EXPECT_NEAR(actual.x, expected.x, epsilon);
    EXPECT_NEAR(actual.y, expected.y, epsilon);
    EXPECT_NEAR(actual.z, expected.z, epsilon);
}

FScene3DSpawnResult SpawnWaterPlane(
    ALegacyScene3DAdapter& runtime,
    FStringView name) noexcept {
    FScene3DSpawnResult spawned = runtime.Graph().TrySpawn(name);
    if (!spawned) return spawned;
    spawned.Node->AddComponent<AMeshComponent3D>(
        EMeshPrimitive3D::Plane);
    spawned.Node->AddComponent<AWaterSurface3DComponent>();
    return spawned;
}

TSharedPtr<AMeshAsset> MakePlanarTriangle() noexcept {
    TSharedPtr<AMeshAsset> mesh = MakeShared<AMeshAsset>();
    if (!mesh) return {};
    mesh->Vertices().Add(
        FMeshVertex{FVec3{0.0f, 0.0f, 0.0f},
                    FVec3{0.0f, 1.0f, 0.0f}, 0.0f, 0.0f});
    mesh->Vertices().Add(
        FMeshVertex{FVec3{2.0f, 0.0f, 0.0f},
                    FVec3{0.0f, 1.0f, 0.0f}, 1.0f, 0.0f});
    mesh->Vertices().Add(
        FMeshVertex{FVec3{0.0f, 0.0f, 2.0f},
                    FVec3{0.0f, 1.0f, 0.0f}, 0.0f, 1.0f});
    mesh->Indices().Add(0u);
    mesh->Indices().Add(1u);
    mesh->Indices().Add(2u);
    return mesh;
}

} // namespace

ACS_TEST(LegacyScene3DCameraContract,
         SelectionAccessorsRemainNonVirtualAndLayoutStable) {
    const std::string header = ReadLegacyCameraWorkspaceSource(
        "src/gameframework/LegacyScene3DAdapter.h");
    const std::string source = ReadLegacyCameraWorkspaceSource(
        "src/gameframework/LegacyScene3DAdapter.cpp");
    EXPECT_FALSE(header.empty());
    EXPECT_FALSE(source.empty());
    EXPECT_TRUE(IsNonVirtualDeclarationLine(
        header, "void SetOrbitCameraActive(bool active) noexcept;"));
    EXPECT_TRUE(IsNonVirtualDeclarationLine(
        header, "bool OrbitCameraActive() const noexcept"));
    EXPECT_TRUE(IsNonVirtualDeclarationLine(
        header, "bool OrbitCameraOverrideActive() const noexcept;"));
    EXPECT_TRUE(IsNonVirtualDeclarationLine(
        header, "bool AuthoredCameraOverrideActive() const noexcept;"));
    EXPECT_TRUE(source.find(
        "constexpr i32 kExplicitOrbitCameraNodeId =")
        != std::string::npos);
    EXPECT_TRUE(source.find(
        "m_ActiveCameraNodeId == kExplicitOrbitCameraNodeId")
        != std::string::npos);
    EXPECT_TRUE(source.find(
        "m_ActiveCameraNodeId != kExplicitOrbitCameraNodeId")
        != std::string::npos);
    EXPECT_TRUE(source.find(
        "m_ActiveCameraNodeId = kExplicitOrbitCameraNodeId;")
        != std::string::npos);
#if defined(_WIN64)
    EXPECT_EQ(
        sizeof(ALegacyScene3DAdapter),
        kLegacyScene3DAdapterLayoutSize);
#endif
}

ACS_TEST(LegacyScene3DUpdate,
         ForwardsEachVariableUpdateToDerivedTickHook) {
    CCameraSelectionTestScene runtime;
    constexpr char kScene[] =
        "ACS3D v2\n"
        "N3D 10 -1 -1 4 5 6 0 0 0 1 1 1 1 1 1 1 Authored\n"
        "CAM3D 10 authored.main 1 5 1 45 18 0.2 4000\n";
    runtime.SetFreeCameraEnabled(false);
    EXPECT_TRUE(runtime.LoadText(kScene, sizeof(kScene) - 1u).Succeeded());
    runtime.SetOrbitCameraActive(true);
    runtime.UpdateForTest(1.0f / 60.0f);
    EXPECT_EQ(runtime.TickCount(), 1);
    runtime.UpdateForTest(1.0f / 60.0f);
    EXPECT_EQ(runtime.TickCount(), 2);
}

ACS_TEST(LegacyScene3DSpriteRuntime,
         SynchronizesBeforeDrawWithoutStaleComponentPointers) {
    const std::string header = ReadLegacyCameraWorkspaceSource(
        "src/gameframework/LegacyScene3DAdapter.h");
    const std::string source = ReadLegacyCameraWorkspaceSource(
        "src/gameframework/LegacyScene3DAdapter.cpp");
    EXPECT_FALSE(header.empty());
    EXPECT_FALSE(source.empty());

    EXPECT_TRUE(header.find("TSharedPtr<AAsset> SourceImage;")
        != std::string::npos);
    EXPECT_TRUE(header.find(
        "const ASprite3DComponent* Component = nullptr;")
        == std::string::npos);
    EXPECT_TRUE(header.find(
        "bool SynchronizeGraphSprites_Internal(IRhiDevice& device) noexcept;")
        != std::string::npos);

    const std::size_t render = source.find(
        "void ALegacyScene3DAdapter::OnRender(");
    const std::size_t device_check = source.find(
        "if (device == nullptr || swapchain == nullptr) return;", render);
    const std::size_t synchronize = source.find(
        "SynchronizeGraphSprites_Internal(*device)", device_check);
    const std::size_t skinned = source.find(
        "UpdateSkinnedMeshes(*device)", synchronize);
    EXPECT_TRUE(render != std::string::npos);
    EXPECT_TRUE(device_check != std::string::npos);
    EXPECT_TRUE(synchronize != std::string::npos);
    EXPECT_TRUE(skinned != std::string::npos);
    EXPECT_TRUE(device_check < synchronize);
    EXPECT_TRUE(synchronize < skinned);

    const std::size_t sync_function = source.find(
        "bool ALegacyScene3DAdapter::SynchronizeGraphSprites_Internal(");
    const std::size_t unchanged = source.find(
        "const bool unchanged =", sync_function);
    const std::size_t unchanged_return = source.find(
        "if (unchanged) {", unchanged);
    const std::size_t upload = source.find(
        "UploadTexture(device, *image)", unchanged_return);
    const std::size_t texture_lookup = source.find(
        "IRhiTexture* ALegacyScene3DAdapter::TextureFor(", upload);
    EXPECT_TRUE(sync_function != std::string::npos);
    EXPECT_TRUE(unchanged != std::string::npos);
    EXPECT_TRUE(unchanged_return != std::string::npos);
    EXPECT_TRUE(upload != std::string::npos);
    EXPECT_TRUE(texture_lookup != std::string::npos);
    EXPECT_TRUE(sync_function < unchanged);
    EXPECT_TRUE(unchanged < unchanged_return);
    EXPECT_TRUE(unchanged_return < upload);
    EXPECT_TRUE(upload < texture_lookup);

    const std::string sync_body = source.substr(
        sync_function, texture_lookup - sync_function);
    EXPECT_TRUE(sync_body.find("node->IsPendingDestroy()")
        != std::string::npos);
    EXPECT_TRUE(sync_body.find(
        "previous.SourceImage.Get() == plan.SourceImage.Get()")
        != std::string::npos);
    EXPECT_TRUE(sync_body.find(
        "Move(m_CustomSprites[plan.ExistingIndex].Texture)")
        != std::string::npos);
    EXPECT_TRUE(sync_body.find("TexturePath(") == std::string::npos);
    EXPECT_TRUE(source.find(
        "scene_features.has_sprites && !m_CustomSprites.IsEmpty()")
        != std::string::npos);
    EXPECT_TRUE(source.find(
        "if (texture == nullptr) continue;")
        != std::string::npos);

#if defined(_WIN64)
    EXPECT_EQ(sizeof(ALegacyScene3DAdapter), kLegacyScene3DAdapterLayoutSize);
#endif
}

ACS_TEST(LegacyScene3DAerialPerspective,
         DefaultIsDisabledAndOptInPreservesLayout) {
    ALegacyScene3DAdapter runtime;
    EXPECT_FALSE(runtime.AerialPerspectiveEnabled());
    runtime.SetAerialPerspectiveEnabled(true);
    EXPECT_TRUE(runtime.AerialPerspectiveEnabled());
    runtime.SetAerialPerspectiveEnabled(false);
    EXPECT_FALSE(runtime.AerialPerspectiveEnabled());

#if defined(_WIN64)
    EXPECT_EQ(
        sizeof(ALegacyScene3DAdapter),
        kLegacyScene3DAdapterLayoutSize);
#endif
}

ACS_TEST(LegacyScene3DAerialPerspective,
         UsesPhysicalOnlyAtmosphereBetweenWaterAndClouds) {
    const std::string header = ReadLegacyCameraWorkspaceSource(
        "src/gameframework/LegacyScene3DAdapter.h");
    const std::string source = ReadLegacyCameraWorkspaceSource(
        "src/gameframework/LegacyScene3DAdapter.cpp");
    EXPECT_FALSE(header.empty());
    EXPECT_FALSE(source.empty());
    EXPECT_TRUE(IsNonVirtualDeclarationLine(
        header,
        "void SetAerialPerspectiveEnabled(bool enabled) noexcept"));
    EXPECT_TRUE(IsNonVirtualDeclarationLine(
        header,
        "bool AerialPerspectiveEnabled() const noexcept"));

    const std::size_t render = source.find(
        "void ALegacyScene3DAdapter::OnRender(");
    const std::size_t volume_defaults = source.find(
        "IRhiTexture* aerial_volume = nullptr;", render);
    const std::size_t transmittance_default = source.find(
        "IRhiTexture* aerial_transmittance = nullptr;", volume_defaults);
    const std::size_t projection_check = source.find(
        "const bool perspective_camera =", transmittance_default);
    const std::size_t aerial_condition = source.find(
        "if (m_AerialPerspectiveEnabled && perspective_camera",
        projection_check);
    const std::size_t build = source.find(
        "aerial_volume = m_Atmosphere.BuildAerialPerspective(", render);
    const std::size_t incomplete_volume_fallback = source.find(
        "if (aerial_transmittance == nullptr) aerial_volume = nullptr;",
        build);
    const std::size_t water = source.find(
        "DrawWaterScene(", build);
    const std::size_t composite = source.find(
        "m_Atmosphere.CompositeAerialPerspective(", water);
    const std::size_t clouds = source.find(
        "CompositeClouds(", composite);
    const std::size_t transparent = source.find(
        "OnRenderTransparent3D(", clouds);
    const std::size_t post = source.find(
        "m_Post.Render(", transparent);
    EXPECT_TRUE(render != std::string::npos);
    EXPECT_TRUE(volume_defaults != std::string::npos);
    EXPECT_TRUE(transmittance_default != std::string::npos);
    EXPECT_TRUE(projection_check != std::string::npos);
    EXPECT_TRUE(aerial_condition != std::string::npos);
    EXPECT_TRUE(build != std::string::npos);
    EXPECT_TRUE(incomplete_volume_fallback != std::string::npos);
    EXPECT_TRUE(water != std::string::npos);
    EXPECT_TRUE(composite != std::string::npos);
    EXPECT_TRUE(clouds != std::string::npos);
    EXPECT_TRUE(transparent != std::string::npos);
    EXPECT_TRUE(post != std::string::npos);
    EXPECT_TRUE(volume_defaults < transmittance_default);
    EXPECT_TRUE(transmittance_default < projection_check);
    EXPECT_TRUE(projection_check < aerial_condition);
    EXPECT_TRUE(aerial_condition < build);
    EXPECT_TRUE(build < incomplete_volume_fallback);
    EXPECT_TRUE(incomplete_volume_fallback < water);
    EXPECT_TRUE(build < water);
    EXPECT_TRUE(water < composite);
    EXPECT_TRUE(composite < clouds);
    EXPECT_TRUE(clouds < transparent);
    EXPECT_TRUE(transparent < post);

    if (projection_check != std::string::npos
        && aerial_condition != std::string::npos) {
        const std::string projection_gate = source.substr(
            projection_check, aerial_condition - projection_check);
        EXPECT_TRUE(projection_gate.find(
            "EScene3DCameraProjection::Perspective")
            != std::string::npos);
        EXPECT_TRUE(projection_gate.find(
            "ESceneProjectionMode::Perspective")
            != std::string::npos);
    }

    // 空気遠近を作れない場合も、nullの体積を渡して通常の雲合成を続ける。
    const std::size_t cloud_fallback_guard = source.rfind(
        "if (depth != nullptr) {", clouds);
    EXPECT_TRUE(cloud_fallback_guard != std::string::npos);
    if (cloud_fallback_guard != std::string::npos
        && clouds != std::string::npos) {
        const std::string cloud_fallback = source.substr(
            cloud_fallback_guard, clouds - cloud_fallback_guard);
        EXPECT_TRUE(cloud_fallback.find("aerial_volume")
                    == std::string::npos);
        EXPECT_TRUE(cloud_fallback.find("aerial_transmittance")
                    == std::string::npos);
    }

    const std::size_t build_end = source.find(
        "if (aerial_volume != nullptr)", build);
    EXPECT_TRUE(build_end != std::string::npos);
    if (build != std::string::npos && build_end != std::string::npos) {
        const std::string physical_build =
            source.substr(build, build_end - build);
        EXPECT_TRUE(physical_build.find("m_Fog") == std::string::npos);
        EXPECT_TRUE(physical_build.find("FVolumetricFogParams")
                    == std::string::npos);
    }
    EXPECT_TRUE(source.find(
        "shader.SetFog(m_Fog.Color, m_Fog.Density")
        != std::string::npos);
    EXPECT_TRUE(source.find(
        "m_Clouds.Composite(\n"
        "        command_list, scene_depth, width, height,\n"
        "        aerial_volume, aerial_transmittance,")
        != std::string::npos);
}

ACS_TEST(LegacyScene3DCloudEnvironmentLighting,
         DefaultsToCloudContributionWithoutChangingLayout) {
    FScene3DClouds clouds{};
    EXPECT_TRUE(clouds.bAffectEnvironmentLighting);
    clouds.bAffectEnvironmentLighting = false;
    EXPECT_FALSE(clouds.bAffectEnvironmentLighting);

#if defined(_WIN64)
    EXPECT_EQ(
        sizeof(ALegacyScene3DAdapter),
        kLegacyScene3DAdapterLayoutSize);
#endif
}

ACS_TEST(LegacyScene3DCloudEnvironmentLighting,
         BuildsAfterCloudsAndFallsBackToVisibleSkyEnvironment) {
    const std::string header = ReadLegacyCameraWorkspaceSource(
        "src/gameframework/LegacyScene3DAdapter.h");
    const std::string source = ReadLegacyCameraWorkspaceSource(
        "src/gameframework/LegacyScene3DAdapter.cpp");
    EXPECT_FALSE(header.empty());
    EXPECT_FALSE(source.empty());
    EXPECT_TRUE(header.find(
        "bool bAffectEnvironmentLighting = true;")
        != std::string::npos);

    const std::size_t render = source.find(
        "void ALegacyScene3DAdapter::OnRender(");
    const std::size_t clouds = source.find(
        "RenderClouds(*device, context.Cmd()", render);
    const std::size_t environment = source.find(
        "EnsureEnvironmentLighting(*device, context.Cmd())", clouds);
    const std::size_t hdr = source.find(
        "EnsureHdrFrameResources(", environment);
    EXPECT_TRUE(render != std::string::npos);
    EXPECT_TRUE(clouds != std::string::npos);
    EXPECT_TRUE(environment != std::string::npos);
    EXPECT_TRUE(hdr != std::string::npos);
    EXPECT_TRUE(clouds < environment);
    EXPECT_TRUE(environment < hdr);

    const auto render_clouds_begin = source.find("void ALegacyScene3DAdapter::RenderClouds(");
    const auto render_clouds_end = source.find("void ALegacyScene3DAdapter::CompositeClouds(", render_clouds_begin);
    EXPECT_TRUE(render_clouds_begin != source.npos);
    EXPECT_TRUE(render_clouds_end != source.npos);
    if (render_clouds_begin != source.npos && render_clouds_end != source.npos) {
        const auto render_clouds = source.substr(render_clouds_begin, render_clouds_end - render_clouds_begin);
        EXPECT_TRUE(render_clouds.find("m_CloudsDrawn = m_Clouds.LastFrameWorkload().submitted;") != render_clouds.npos);
        EXPECT_TRUE(render_clouds.find("m_CloudsDrawn = true;") == render_clouds.npos);
    }

    const std::size_t ensure_begin = source.find(
        "bool ALegacyScene3DAdapter::EnsureEnvironmentLighting(");
    const std::size_t ensure_end = source.find(
        "bool ALegacyScene3DAdapter::EnsureShadowMap(", ensure_begin);
    EXPECT_TRUE(ensure_begin != std::string::npos);
    EXPECT_TRUE(ensure_end != std::string::npos);
    if (ensure_begin != std::string::npos
        && ensure_end != std::string::npos) {
        const std::string ensure = source.substr(
            ensure_begin, ensure_end - ensure_begin);
        EXPECT_TRUE(ensure.find(
            "m_CloudsDrawn && m_CloudParams.bAffectEnvironmentLighting")
            != std::string::npos);
        EXPECT_TRUE(ensure.find("m_Clouds.RenderedEnvironmentLightingUpdateSignature()") != ensure.npos);
        EXPECT_TRUE(ensure.find(
            "cloud_signature == m_IblBakedCloudSignature")
            != std::string::npos);
        const std::size_t cadence_frame = ensure.find("m_Clouds.LastFrameWorkload().submission_index");
        const auto update_signature = ensure.find("m_Clouds.RenderedEnvironmentLightingUpdateSignature()", cadence_frame);
        const auto duplicate_gate = ensure.find("cloud_signature == m_IblBakedCloudSignature", update_signature);
        const std::size_t cadence_gate = ensure.find("CVolumetricClouds::IsEnvironmentLightingRefreshFrame(", cadence_frame);
        const std::size_t cloud_build = ensure.find("m_Clouds.BuildEnvironmentCubemap(", cadence_gate);
        EXPECT_TRUE(cadence_frame != std::string::npos);
        EXPECT_TRUE(update_signature != ensure.npos);
        EXPECT_TRUE(duplicate_gate != ensure.npos);
        EXPECT_TRUE(cadence_gate != std::string::npos);
        EXPECT_TRUE(cloud_build != std::string::npos);
        EXPECT_TRUE(cadence_frame < update_signature);
        EXPECT_TRUE(update_signature < duplicate_gate);
        EXPECT_TRUE(duplicate_gate < cadence_gate);
        EXPECT_TRUE(cadence_gate < cloud_build);
        EXPECT_TRUE(ensure.find("const bool cloud_mode_changed =") != std::string::npos);
        EXPECT_TRUE(ensure.find(
            "if (rebuild_base_environment) {")
            != std::string::npos);
        EXPECT_TRUE(ensure.find(
            "m_Atmosphere.BakeEquirect(")
            != std::string::npos);
        EXPECT_TRUE(ensure.find(
            "m_Clouds.BuildEnvironmentCubemap(")
            != std::string::npos);
        EXPECT_TRUE(ensure.find(
            "m_Ibl.RebuildDerivedMapsFromEnvironment(")
            != std::string::npos);
        EXPECT_TRUE(ensure.find(
            "device, command_list, *base_environment);")
            != std::string::npos);
        EXPECT_TRUE(ensure.find(
            "m_IblBakedCloudSignature = cloud_signature;")
            != std::string::npos);
        EXPECT_TRUE(ensure.find(
            "if (m_BrdfLutUnavailable) return false;")
            != std::string::npos);
        EXPECT_TRUE(ensure.find(
            "m_BrdfLutUnavailable = true;")
            != std::string::npos);
        EXPECT_TRUE(ensure.find(
            "m_CloudParams.Wind,\n                m_Time")
            == std::string::npos);
        EXPECT_TRUE(ensure.find("command_list.Submit(") == std::string::npos);
    }

    EXPECT_TRUE(header.find(
        "bool m_BrdfLutUnavailable = false;")
        != std::string::npos);
    const std::size_t release_begin = source.find(
        "void ALegacyScene3DAdapter::ReleaseGpu()");
    const std::size_t release_end = source.find(
        "void ALegacyScene3DAdapter::JoinCpuCompileWorkers()",
        release_begin);
    EXPECT_TRUE(release_begin != std::string::npos);
    EXPECT_TRUE(release_end != std::string::npos);
    if (release_begin != std::string::npos
        && release_end != std::string::npos) {
        const std::string release = source.substr(
            release_begin, release_end - release_begin);
        EXPECT_TRUE(release.find("m_Clouds.Shutdown();")
            != std::string::npos);
        EXPECT_TRUE(release.find("m_Atmosphere.Shutdown();")
            != std::string::npos);
        EXPECT_TRUE(release.find("m_Ibl.Shutdown();")
            != std::string::npos);
        EXPECT_TRUE(release.find("m_Shadow.Shutdown();")
            != std::string::npos);
        EXPECT_TRUE(release.find("m_CloudsReady = false;")
            != std::string::npos);
        EXPECT_TRUE(release.find("m_IblReady = false;")
            != std::string::npos);
        EXPECT_TRUE(release.find("m_ShadowReady = false;")
            != std::string::npos);
        EXPECT_TRUE(release.find(
            "m_BrdfLutUnavailable = false;")
            != std::string::npos);
    }

    // 表示用EnvCubemapは雲なしのまま描き、画面用雲は従来の後段合成を一度だけ使う。
    const std::size_t sky_begin = source.find(
        "void ALegacyScene3DAdapter::RenderSky(");
    const std::size_t lighting_begin = source.find(
        "bool ALegacyScene3DAdapter::EnsureEnvironmentLighting(",
        sky_begin);
    EXPECT_TRUE(sky_begin != std::string::npos);
    EXPECT_TRUE(lighting_begin != std::string::npos);
    if (sky_begin != std::string::npos
        && lighting_begin != std::string::npos) {
        const std::string sky = source.substr(
            sky_begin, lighting_begin - sky_begin);
        EXPECT_TRUE(sky.find("m_Ibl.DrawEnvSkyboxCameraRelative(")
                    != std::string::npos);
        EXPECT_TRUE(sky.find("cloud_environment")
                    == std::string::npos);
    }
    EXPECT_TRUE(source.find(
        "m_Clouds.Composite(\n"
        "        command_list, scene_depth, width, height,")
        != std::string::npos);
}

ACS_TEST(LegacyScene3DSelectionHighlight, PublicApiIsTransactionalDefaultOffAndLayoutStable) {
    ALegacyScene3DAdapter runtime;
    EXPECT_FALSE(runtime.SelectionHighlightNode().IsValid());

    FScene3DSpawnResult parent = runtime.Graph().TrySpawn(FStringView("FocusParent"));
    EXPECT_TRUE(parent.Succeeded());
    FScene3DSpawnResult child = runtime.Graph().TrySpawn(FStringView("VisibleChild"), parent.Node);
    EXPECT_TRUE(child.Succeeded());
    child.Node->AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Cube);

    EXPECT_TRUE(runtime.SetSelectionHighlight(parent.Id));
    EXPECT_TRUE(runtime.SelectionHighlightNode() == parent.Id);
    EXPECT_TRUE(runtime.SetSelectionHighlight(child.Id, FVec3{0.2f, 0.7f, 1.0f}, 1.5f, 3.0f));
    EXPECT_TRUE(runtime.SelectionHighlightNode() == child.Id);

    const f32 invalid = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(runtime.SetSelectionHighlight(parent.Id, FVec3{invalid, 0.5f, 0.5f}, 1.0f, 2.0f));
    EXPECT_FALSE(runtime.SetSelectionHighlight(parent.Id, FVec3{1.0f, 0.5f, 0.0f}, 0.0f, 2.0f));
    EXPECT_FALSE(runtime.SetSelectionHighlight(parent.Id, FVec3{1.0f, 0.5f, 0.0f}, 1.0f, 4.1f));
    EXPECT_TRUE(runtime.SelectionHighlightNode() == child.Id);

    FScene3DSpawnResult stale = runtime.Graph().TrySpawn(FStringView("StaleFocus"));
    EXPECT_TRUE(stale.Succeeded());
    const FNodeId stale_id = stale.Id;
    EXPECT_TRUE(runtime.Graph().Destroy(stale_id));
    runtime.Graph().Update(0.0f);
    EXPECT_FALSE(runtime.SetSelectionHighlight(stale_id));
    EXPECT_TRUE(runtime.SelectionHighlightNode() == child.Id);

    // Graph.Updateを待たず、破棄予約された選択nodeを公開しない。
    EXPECT_TRUE(runtime.Graph().Destroy(child.Id));
    EXPECT_FALSE(runtime.SelectionHighlightNode().IsValid());
    EXPECT_FALSE(runtime.SetSelectionHighlight(child.Id));
    runtime.Graph().Update(0.0f);
    EXPECT_FALSE(runtime.SelectionHighlightNode().IsValid());

    runtime.ClearSelectionHighlight();
    EXPECT_FALSE(runtime.SelectionHighlightNode().IsValid());
    runtime.ClearSelectionHighlight();
    EXPECT_FALSE(runtime.SelectionHighlightNode().IsValid());

#if defined(_WIN64)
    EXPECT_EQ(sizeof(ALegacyScene3DAdapter), kLegacyScene3DAdapterLayoutSize);
#endif
}

ACS_TEST(LegacyScene3DSelectionHighlight, RuntimeMarkerNeverEntersSceneArchives) {
    ALegacyScene3DAdapter runtime;
    const FScene3DSpawnResult selected =
        runtime.Graph().TrySpawn(FStringView("ArchiveInvisibleSelection"));
    EXPECT_TRUE(selected.Succeeded());
    if (!selected.Succeeded()) return;

    u8 binary_before[4096]{};
    u8 binary_selected[4096]{};
    char text_before[4096]{};
    char text_selected[4096]{};
    const FSceneSaveResult binary_without_marker =
        TrySaveNodeTree(&runtime.Graph().Root(), binary_before, sizeof(binary_before));
    const FScene3DSaveResult text_without_marker =
        TrySaveScene3DText(runtime.Graph(), text_before, sizeof(text_before));
    EXPECT_TRUE(binary_without_marker.Succeeded());
    EXPECT_TRUE(text_without_marker.Succeeded());

    EXPECT_TRUE(runtime.SetSelectionHighlight(selected.Id));
    const FSceneSaveResult binary_with_marker =
        TrySaveNodeTree(&runtime.Graph().Root(), binary_selected, sizeof(binary_selected));
    const FScene3DSaveResult text_with_marker =
        TrySaveScene3DText(runtime.Graph(), text_selected, sizeof(text_selected));
    EXPECT_TRUE(binary_with_marker.Succeeded());
    EXPECT_TRUE(text_with_marker.Succeeded());
    EXPECT_EQ(binary_with_marker.BytesWritten, binary_without_marker.BytesWritten);
    EXPECT_EQ(binary_with_marker.ComponentCount, binary_without_marker.ComponentCount);
    EXPECT_EQ(text_with_marker.BytesWritten, text_without_marker.BytesWritten);
    EXPECT_TRUE(std::memcmp(binary_selected, binary_before, binary_with_marker.BytesWritten) == 0);
    EXPECT_TRUE(std::memcmp(text_selected, text_before, text_with_marker.BytesWritten) == 0);
    EXPECT_TRUE(runtime.SelectionHighlightNode() == selected.Id);
}

ACS_TEST(LegacyScene3DSelectionHighlight, UsesDepthTestedSubtreeMaskBeforeFxaaAndHud) {
    const std::string header = ReadLegacyCameraWorkspaceSource("src/gameframework/LegacyScene3DAdapter.h");
    const std::string adapter = ReadLegacyCameraWorkspaceSource("src/gameframework/LegacyScene3DAdapter.cpp");
    const std::string motion = ReadLegacyCameraWorkspaceSource("src/render/MotionVector.cpp");
    const std::string post = ReadLegacyCameraWorkspaceSource("src/render/PostProcess.cpp");
    EXPECT_FALSE(header.empty());
    EXPECT_FALSE(adapter.empty());
    EXPECT_FALSE(motion.empty());
    EXPECT_FALSE(post.empty());

    EXPECT_TRUE(IsNonVirtualDeclarationLine(header, "bool SetSelectionHighlight("));
    EXPECT_TRUE(IsNonVirtualDeclarationLine(header, "void ClearSelectionHighlight() noexcept;"));
    EXPECT_TRUE(IsNonVirtualDeclarationLine(header, "FNodeId SelectionHighlightNode() const noexcept;"));
    EXPECT_TRUE(adapter.find("class ASelectionHighlightMarker3D final : public AComponent") != std::string::npos);
    EXPECT_TRUE(adapter.find("ACS_GAME_COMPONENT_KIND(ASelectionHighlightMarker3D)") == std::string::npos);
    EXPECT_TRUE(adapter.find("return ComponentKindOf<ASelectionHighlightMarker3D>();") != std::string::npos);
    EXPECT_TRUE(adapter.find("const char* ReflectName() const noexcept override { return nullptr; }") != std::string::npos);
    EXPECT_TRUE(adapter.find("IsSelectionHighlightOwnerLive_Internal(Graph(), *owner)") != std::string::npos);
    EXPECT_TRUE(adapter.find("marker == nullptr || owner == nullptr || !IsEffectivelyActive(*owner)") != std::string::npos);
    EXPECT_TRUE(adapter.find("current = current->Parent();") != std::string::npos);
    EXPECT_TRUE(adapter.find("|| selection_requested;") != std::string::npos);
    EXPECT_TRUE(adapter.find("draw.SelectionHighlighted") != std::string::npos);
    EXPECT_TRUE(motion.find("saturate(normal_row0.w)") != std::string::npos);
    EXPECT_TRUE(motion.find("selection_mask ? 1.0f : 0.0f") != std::string::npos);
    EXPECT_TRUE(post.find("Texture2D    selection_mask : register(t4);") != std::string::npos);
    EXPECT_TRUE(post.find("saturate(neighbor_mask - center_mask)") != std::string::npos);
    EXPECT_TRUE(post.find("pd.texture_slots = 5;") != std::string::npos);
    EXPECT_TRUE(post.find("cmd.SetTexture(4, *selection_input);") != std::string::npos);

    const std::size_t frame = adapter.find("void ALegacyScene3DAdapter::OnRender(");
    const std::size_t mask = adapter.find("IRhiTexture* selection_mask =", frame);
    const std::size_t outlined_post = adapter.find("selection_mask, selection_highlight->Color()", mask);
    const std::size_t normal_post = adapter.find("m_Post.Render(command_list, *swapchain, renderer.CurrentBuffer(), m_PostParams);", outlined_post + 1u);
    const std::size_t hud = adapter.find("EnsureSpriteBatch(context)", normal_post);
    EXPECT_TRUE(frame != std::string::npos);
    EXPECT_TRUE(mask != std::string::npos);
    EXPECT_TRUE(outlined_post != std::string::npos);
    EXPECT_TRUE(normal_post != std::string::npos);
    EXPECT_TRUE(hud != std::string::npos);
    EXPECT_TRUE(frame < mask);
    EXPECT_TRUE(mask < outlined_post);
    EXPECT_TRUE(outlined_post < normal_post);
    EXPECT_TRUE(normal_post < hud);
}

ACS_TEST(LegacyScene3DWaterRuntime, TransformedPlaneUsesExactWorldHit) {
    ALegacyScene3DAdapter runtime;
    FScene3DSpawnResult surface =
        SpawnWaterPlane(runtime, FStringView("TransformedWater"));
    EXPECT_TRUE(surface.Succeeded());

    surface.Node->Local().position = FVec3{3.0f, 1.5f, -2.0f};
    surface.Node->Local().rotation =
        FQuat::AxisAngle(FVec3{1.0f, 0.0f, 0.0f}, 0.42f);
    surface.Node->Local().scale = FVec3{4.0f, 1.5f, 3.0f};

    const FMat4 model = surface.Node->World().ToMat4();
    const FVec3 expected_point =
        TransformPoint(FVec3{0.20f, 0.0f, 0.10f}, model);
    const FVec3 expected_normal = Normalize(TransformVector(
        FVec3{0.0f, 1.0f, 0.0f}, Transpose(Inverse(model))));
    const FRay3 ray{
        expected_point + expected_normal * 5.0f,
        -expected_normal,
    };

    FWaterRaycastHit hit{};
    EXPECT_TRUE(runtime.RaycastWater(ray, hit, 20.0f));
    EXPECT_TRUE(hit.Node == surface.Id);
    EXPECT_NEAR(hit.Distance, 5.0f, 1.0e-4f);
    EXPECT_NEAR(hit.Point.x, expected_point.x, 1.0e-4f);
    EXPECT_NEAR(hit.Point.y, expected_point.y, 1.0e-4f);
    EXPECT_NEAR(hit.Point.z, expected_point.z, 1.0e-4f);
    EXPECT_NEAR(hit.Normal.x, expected_normal.x, 1.0e-4f);
    EXPECT_NEAR(hit.Normal.y, expected_normal.y, 1.0e-4f);
    EXPECT_NEAR(hit.Normal.z, expected_normal.z, 1.0e-4f);

    EXPECT_TRUE(runtime.AddWaterDisturbance(
        hit.Node, hit.Point, 0.24f, 0.31f));
    EXPECT_TRUE(runtime.AddWaterWake(
        hit.Node, hit.Point, FVec3{3.0f, 0.0f, 1.0f},
        0.28f, 0.17f));
    EXPECT_EQ(runtime.ActiveWaterRippleCount(surface.Id), 2u);
}

ACS_TEST(LegacyScene3DWaterRuntime, OpaqueForegroundRejectsInteraction) {
    ALegacyScene3DAdapter runtime;
    FScene3DSpawnResult surface =
        SpawnWaterPlane(runtime, FStringView("OccludedWater"));
    EXPECT_TRUE(surface.Succeeded());
    surface.Node->Local().scale = FVec3{10.0f, 1.0f, 10.0f};

    FScene3DSpawnResult blocker =
        runtime.Graph().TrySpawn(FStringView("ForegroundCube"));
    EXPECT_TRUE(blocker.Succeeded());
    blocker.Node->Local().position = FVec3{0.0f, 2.0f, 0.0f};
    blocker.Node->AddComponent<AMeshComponent3D>(
        EMeshPrimitive3D::Cube);

    const FRay3 ray{
        FVec3{0.0f, 5.0f, 0.0f},
        FVec3{0.0f, -1.0f, 0.0f},
    };
    FWaterRaycastHit hit{};
    EXPECT_FALSE(runtime.RaycastWater(ray, hit, 20.0f));
    EXPECT_FALSE(hit.IsValid());

    blocker.Node->SetVisible(false);
    EXPECT_TRUE(runtime.RaycastWater(ray, hit, 20.0f));
    EXPECT_TRUE(hit.Node == surface.Id);

    blocker.Node->SetVisible(true);
    blocker.Node->SetEnabled(false);
    EXPECT_TRUE(runtime.RaycastWater(ray, hit, 20.0f));
    EXPECT_TRUE(hit.Node == surface.Id);
}

ACS_TEST(LegacyScene3DWaterRuntime, HiddenAncestorRejectsDirectInteraction) {
    ALegacyScene3DAdapter runtime;
    FScene3DSpawnResult group =
        runtime.Graph().TrySpawn(FStringView("WaterGroup"));
    EXPECT_TRUE(group.Succeeded());
    FScene3DSpawnResult surface =
        runtime.Graph().TrySpawn(
            FStringView("NestedWater"), group.Node);
    EXPECT_TRUE(surface.Succeeded());
    surface.Node->AddComponent<AMeshComponent3D>(
        EMeshPrimitive3D::Plane);
    surface.Node->AddComponent<AWaterSurface3DComponent>();

    const FVec3 point{0.0f, 0.0f, 0.0f};
    group.Node->SetVisible(false);
    EXPECT_FALSE(runtime.AddWaterDisturbance(
        surface.Id, point, 0.2f, 0.2f));
    group.Node->SetVisible(true);
    group.Node->SetEnabled(false);
    EXPECT_FALSE(runtime.AddWaterWake(
        surface.Id, point, FVec3{1.0f, 0.0f, 0.0f},
        0.2f, 0.2f));
    EXPECT_EQ(runtime.ActiveWaterRippleCount(surface.Id), 0u);
}

ACS_TEST(LegacyScene3DWaterRuntime, CustomWaterUsesAuthoredTriangles) {
    ALegacyScene3DAdapter runtime;
    FScene3DSpawnResult surface =
        runtime.Graph().TrySpawn(FStringView("TriangleWater"));
    EXPECT_TRUE(surface.Succeeded());
    auto& mesh_component =
        surface.Node->AddComponent<AMeshComponent3D>();
    TSharedPtr<AMeshAsset> triangle = MakePlanarTriangle();
    EXPECT_TRUE(static_cast<bool>(triangle));
    mesh_component.SetMeshAsset(TSharedPtr<AAsset>(triangle));
    surface.Node->AddComponent<AWaterSurface3DComponent>();

    FWaterRaycastHit hit{};
    EXPECT_TRUE(runtime.RaycastWater(
        FRay3{FVec3{0.25f, 4.0f, 0.25f},
              FVec3{0.0f, -1.0f, 0.0f}},
        hit, 10.0f));
    EXPECT_TRUE(hit.Node == surface.Id);

    // This point is inside the triangle's AABB but outside the authored
    // triangle. A bounds-only water picker would return a false hit here.
    EXPECT_FALSE(runtime.RaycastWater(
        FRay3{FVec3{1.80f, 4.0f, 1.80f},
              FVec3{0.0f, -1.0f, 0.0f}},
        hit, 10.0f));
}

ACS_TEST(LegacyScene3DWaterRuntime, NonPlanarCustomMeshStaysOpaque) {
    ALegacyScene3DAdapter runtime;
    FScene3DSpawnResult surface =
        runtime.Graph().TrySpawn(FStringView("NonPlanarWater"));
    EXPECT_TRUE(surface.Succeeded());
    auto& mesh_component =
        surface.Node->AddComponent<AMeshComponent3D>();
    TSharedPtr<AMeshAsset> mesh = MakePlanarTriangle();
    EXPECT_TRUE(static_cast<bool>(mesh));
    mesh->Vertices()[2].position.y = 1.0f;
    mesh_component.SetMeshAsset(TSharedPtr<AAsset>(mesh));
    surface.Node->AddComponent<AWaterSurface3DComponent>();

    FWaterRaycastHit hit{};
    EXPECT_FALSE(runtime.RaycastWater(
        FRay3{FVec3{0.25f, 4.0f, 0.25f},
              FVec3{0.0f, -1.0f, 0.0f}},
        hit, 10.0f));
    EXPECT_FALSE(runtime.AddWaterDisturbance(
        surface.Id, FVec3{0.25f, 0.0f, 0.25f}));
    EXPECT_EQ(runtime.ActiveWaterRippleCount(surface.Id), 0u);
}

ACS_TEST(LegacyScene3DCameraRuntime,
         SelectsSwitchesAndRefreshesLiveHierarchicalPose) {
    constexpr char kScene[] =
        "ACS3D v2\n"
        "N3D 10 -1 -1 10 0 0 0 90 0 2 3 4 1 1 1 1 Rig\n"
        "N3D 20 10 -1 1 2 3 0 0 0 1 1 1 1 1 1 1 Gameplay\n"
        "CAM3D 20 camera-b 0 5 1 70 12 0.1 2000\n"
        "N3D 30 -1 -1 -4 5 6 10 20 30 7 0.5 3 1 1 1 1 Cinematic\n"
        "CAM3D 30 camera-a 1 5 1 45 18 0.2 4000\n";

    ALegacyScene3DAdapter runtime;
    const FScene3DLoadResult loaded =
        runtime.LoadText(kScene, sizeof(kScene) - 1u);
    EXPECT_TRUE(loaded.Succeeded());
    EXPECT_EQ(loaded.CameraCount, 2u);
    EXPECT_EQ(loaded.ActivePreferredCameraCount, 2u);
    EXPECT_TRUE(loaded.ActiveCamera.IsAuthored);
    EXPECT_TRUE(
        std::strcmp(loaded.ActiveCamera.StableId, "camera-a") == 0);
    EXPECT_EQ(runtime.CameraCount(), 2u);
    EXPECT_TRUE(runtime.AuthoredCamera() != nullptr);
    if (runtime.AuthoredCamera() == nullptr) return;
    EXPECT_TRUE(
        std::strcmp(runtime.AuthoredCamera()->StableId, "camera-a") == 0);
    EXPECT_EQ(
        runtime.AuthoredCamera()->Projection,
        EScene3DCameraProjection::Orthographic);

    ANode* gameplay = runtime.Graph().Root().FindBySerialId(20);
    ANode* cinematic = runtime.Graph().Root().FindBySerialId(30);
    EXPECT_TRUE(gameplay != nullptr);
    EXPECT_TRUE(cinematic != nullptr);
    if (gameplay == nullptr || cinematic == nullptr) return;
    EXPECT_TRUE(gameplay->GetComponent<ACameraComponent3D>() != nullptr);
    EXPECT_TRUE(cinematic->GetComponent<ACameraComponent3D>() != nullptr);

    ACameraComponent3D* gameplay_component =
        gameplay->GetComponent<ACameraComponent3D>();
    EXPECT_TRUE(gameplay_component != nullptr);
    if (gameplay_component == nullptr) return;
    FScene3DCameraState promoted_gameplay;
    promoted_gameplay.IsAuthored = true;
    promoted_gameplay.IsActivePreferred = true;
    promoted_gameplay.Priority = 50;
    promoted_gameplay.Projection = EScene3DCameraProjection::Perspective;
    promoted_gameplay.FovYDegrees = 70.0f;
    promoted_gameplay.OrthographicHeight = 12.0f;
    promoted_gameplay.NearPlane = 0.1f;
    promoted_gameplay.FarPlane = 2000.0f;
    std::memcpy(promoted_gameplay.StableId, "camera-b", 9u);
    EXPECT_TRUE(
        gameplay_component->TrySetAuthoredState(promoted_gameplay));
    EXPECT_TRUE(runtime.RefreshActiveCamera());
    EXPECT_EQ(runtime.AuthoredCamera()->NodeId, 20);

    EXPECT_TRUE(runtime.SetActiveCamera(30));
    EXPECT_TRUE(runtime.RefreshActiveCamera());
    EXPECT_EQ(runtime.AuthoredCamera()->NodeId, 30);
    EXPECT_TRUE(runtime.UseAutomaticCameraSelection());
    EXPECT_EQ(runtime.AuthoredCamera()->NodeId, 20);

    EXPECT_TRUE(runtime.SetActiveCamera("camera-b"));
    const FScene3DCameraState* gameplay_camera = runtime.AuthoredCamera();
    EXPECT_TRUE(gameplay_camera != nullptr);
    if (gameplay_camera == nullptr) return;
    EXPECT_EQ(gameplay_camera->NodeId, 20);
    EXPECT_EQ(
        gameplay_camera->Projection,
        EScene3DCameraProjection::Perspective);
    EXPECT_NEAR(gameplay_camera->FovYDegrees, 70.0f, 1e-6f);
    ExpectVec3Near(
        gameplay_camera->Position, gameplay->World().position, 1e-5f);
    ExpectVec3Near(
        gameplay_camera->Forward,
        Rotate(gameplay->World().rotation, FVec3{0, 0, 1}), 1e-5f);
    ExpectVec3Near(
        gameplay_camera->Up,
        Rotate(gameplay->World().rotation, FVec3{0, 1, 0}), 1e-5f);

    ANode* rig = runtime.Graph().Root().FindBySerialId(10);
    EXPECT_TRUE(rig != nullptr);
    if (rig == nullptr) return;
    rig->Local().position = FVec3{100, 20, -30};
    rig->Local().SetEulerDeg(FVec3{-15, 130, 25});
    EXPECT_TRUE(runtime.RefreshActiveCamera());
    gameplay_camera = runtime.AuthoredCamera();
    EXPECT_TRUE(gameplay_camera != nullptr);
    if (gameplay_camera == nullptr) return;
    ExpectVec3Near(
        gameplay_camera->Position, gameplay->World().position, 1e-4f);
    ExpectVec3Near(
        gameplay_camera->Forward,
        Rotate(gameplay->World().rotation, FVec3{0, 0, 1}), 1e-4f);

    EXPECT_TRUE(runtime.SetActiveCamera(30));
    const i32 stable_node = runtime.AuthoredCamera()->NodeId;
    rig->SetEnabled(false);
    EXPECT_TRUE(!runtime.SetActiveCamera("camera-b"));
    EXPECT_TRUE(!runtime.SetActiveCamera("missing-camera"));
    EXPECT_EQ(runtime.AuthoredCamera()->NodeId, stable_node);

    cinematic->SetEnabled(false);
    EXPECT_TRUE(!runtime.RefreshActiveCamera());
    EXPECT_TRUE(runtime.AuthoredCamera() == nullptr);
    EXPECT_EQ(
        runtime.ProjectionMode(),
        ESceneProjectionMode::Perspective);
}

ACS_TEST(LegacyScene3DCameraRuntime,
         ExplicitOrbitSelectionSurvivesAutomaticRefreshUntilReleased) {
    constexpr char kScene[] =
        "ACS3D v2\n"
        "N3D 10 -1 -1 4 5 6 0 0 0 1 1 1 1 1 1 1 Authored\n"
        "CAM3D 10 authored.main 1 5 1 45 18 0.2 4000\n";

    CCameraSelectionTestScene runtime;
    runtime.SetFreeCameraEnabled(false);
    const FScene3DLoadResult loaded =
        runtime.LoadText(kScene, sizeof(kScene) - 1u);
    EXPECT_TRUE(loaded.Succeeded());
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCamera() != nullptr);

    EXPECT_TRUE(runtime.SetActiveCamera("authored.main"));
    EXPECT_TRUE(runtime.AuthoredCameraOverrideActive());
    runtime.SetOrbit(FVec3{1.0f, 2.0f, 3.0f}, 0.0f, 0.0f, 5.0f);
    runtime.SetOrbitCameraActive(true);
    EXPECT_TRUE(runtime.OrbitCameraActive());
    EXPECT_TRUE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCamera() == nullptr);
    EXPECT_EQ(runtime.ProjectionMode(), ESceneProjectionMode::Perspective);
    ExpectVec3Near(runtime.Camera().Eye(), FVec3{1.0f, 2.0f, -2.0f}, 1.0e-6f);

    runtime.UpdateForTest(1.0f / 60.0f);
    EXPECT_TRUE(runtime.OrbitCameraActive());
    EXPECT_TRUE(runtime.OrbitCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCamera() == nullptr);
    EXPECT_FALSE(runtime.SetActiveCamera("missing.camera"));
    EXPECT_TRUE(runtime.OrbitCameraActive());
    EXPECT_TRUE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());

    EXPECT_TRUE(runtime.SetActiveCamera(10));
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCamera() != nullptr);
    EXPECT_EQ(runtime.AuthoredCamera()->NodeId, 10);
    ExpectVec3Near(runtime.Camera().Eye(), FVec3{4.0f, 5.0f, 6.0f}, 1.0e-6f);

    runtime.SetOrbitCameraActive(true);
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
    EXPECT_TRUE(runtime.ClearActiveCameraOverride());
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
    runtime.SetOrbitCameraActive(true);
    EXPECT_TRUE(runtime.UseAutomaticCameraSelection());
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
    runtime.SetOrbitCameraActive(true);
    runtime.SetOrbitCameraActive(false);
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
}

ACS_TEST(LegacyScene3DCameraRuntime,
         AutomaticSelectionKeepsOrbitOnlyWhileNoAuthoredCameraExists) {
    CCameraSelectionTestScene runtime;
    runtime.SetFreeCameraEnabled(false);
    EXPECT_TRUE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
    runtime.SetOrbitCameraActive(true);
    EXPECT_TRUE(runtime.OrbitCameraOverrideActive());
    runtime.SetOrbitCameraActive(false);
    EXPECT_TRUE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
    runtime.SetOrbitCameraActive(true);
    EXPECT_TRUE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());

    FScene3DSpawnResult spawned =
        runtime.Graph().TrySpawn(FStringView("RuntimeCamera"));
    EXPECT_TRUE(spawned.Succeeded());
    if (!spawned) return;
    auto& camera = spawned.Node->AddComponent<ACameraComponent3D>();
    FScene3DCameraState authored;
    authored.IsAuthored = true;
    authored.IsActivePreferred = true;
    authored.Priority = 10;
    std::memcpy(authored.StableId, "runtime.camera", 15u);
    EXPECT_TRUE(camera.TrySetAuthoredState(authored));

    runtime.UpdateForTest(1.0f / 60.0f);
    EXPECT_TRUE(runtime.OrbitCameraActive());
    EXPECT_TRUE(runtime.OrbitCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCamera() == nullptr);
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());

    runtime.SetOrbitCameraActive(false);
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCamera() != nullptr);
    EXPECT_EQ(runtime.AuthoredCamera()->NodeId, spawned.Node->SerialId());
}

ACS_TEST(LegacyScene3DCameraRuntime,
         RuntimeStableIdOverrideDoesNotCollideWithExplicitOrbit) {
    CCameraSelectionTestScene runtime;
    runtime.SetFreeCameraEnabled(false);
    FScene3DSpawnResult spawned =
        runtime.Graph().TrySpawn(FStringView("RuntimeCamera"));
    EXPECT_TRUE(spawned.Succeeded());
    if (!spawned) return;
    auto& camera = spawned.Node->AddComponent<ACameraComponent3D>();
    FScene3DCameraState authored;
    authored.IsAuthored = true;
    authored.IsActivePreferred = true;
    authored.Priority = 10;
    std::memcpy(authored.StableId, "runtime.stable", 15u);
    EXPECT_TRUE(camera.TrySetAuthoredState(authored));
    EXPECT_EQ(spawned.Node->SerialId(), -1);

    EXPECT_TRUE(runtime.SetActiveCamera("runtime.stable"));
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCamera() != nullptr);
    EXPECT_EQ(runtime.AuthoredCamera()->NodeId, -1);

    runtime.UpdateForTest(1.0f / 60.0f);
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_TRUE(runtime.AuthoredCameraOverrideActive());
    EXPECT_EQ(runtime.AuthoredCamera()->NodeId, -1);
    EXPECT_FALSE(runtime.SetActiveCamera("missing.runtime"));
    EXPECT_FALSE(runtime.SetActiveCamera(nullptr));
    EXPECT_FALSE(runtime.SetActiveCamera(""));
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_TRUE(runtime.AuthoredCameraOverrideActive());
    EXPECT_TRUE(std::strcmp(
        runtime.AuthoredCamera()->StableId, "runtime.stable") == 0);

    runtime.SetOrbitCameraActive(true);
    EXPECT_TRUE(runtime.OrbitCameraActive());
    EXPECT_TRUE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
    EXPECT_TRUE(runtime.SetActiveCamera("runtime.stable"));
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_TRUE(runtime.AuthoredCameraOverrideActive());

    EXPECT_TRUE(runtime.ClearActiveCameraOverride());
    EXPECT_FALSE(runtime.OrbitCameraActive());
    EXPECT_FALSE(runtime.OrbitCameraOverrideActive());
    EXPECT_FALSE(runtime.AuthoredCameraOverrideActive());
}

ACS_TEST(LegacyScene3DCameraRuntime,
         RejectsMalformedAndDuplicateCameraContractsTransactionally) {
    constexpr char kInvalidOptics[] =
        "ACS3D v2\n"
        "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Camera\n"
        "CAM3D 1 camera 0 0 1 60 10 1 1\n";
    constexpr char kDuplicateIdentity[] =
        "ACS3D v2\n"
        "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 A\n"
        "CAM3D 1 duplicate 0 0 1 60 10 0.1 1000\n"
        "N3D 2 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 B\n"
        "CAM3D 2 duplicate 0 1 0 60 10 0.1 1000\n";
    constexpr char kInvalidIdentity[] =
        "ACS3D v2\n"
        "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Camera\n"
        "CAM3D 1 bad/id 0 0 1 60 10 0.1 1000\n";

    ALegacyScene3DAdapter runtime;
    runtime.Graph().Spawn(FStringView("Keep"));
    const FScene3DLoadResult optics =
        runtime.LoadText(kInvalidOptics, sizeof(kInvalidOptics) - 1u);
    EXPECT_EQ(optics.Error, EScene3DSerializeError::InvalidCamera);
    EXPECT_TRUE(runtime.Graph().FindByName(FStringView("Keep")) != nullptr);
    const FScene3DLoadResult duplicate = runtime.LoadText(
        kDuplicateIdentity, sizeof(kDuplicateIdentity) - 1u);
    EXPECT_EQ(duplicate.Error, EScene3DSerializeError::DuplicateCamera);
    EXPECT_TRUE(runtime.Graph().FindByName(FStringView("Keep")) != nullptr);
    const FScene3DLoadResult identity =
        runtime.LoadText(kInvalidIdentity, sizeof(kInvalidIdentity) - 1u);
    EXPECT_EQ(identity.Error, EScene3DSerializeError::InvalidCamera);
    EXPECT_TRUE(runtime.Graph().FindByName(FStringView("Keep")) != nullptr);
}

ACS_TEST(LegacyScene3DCameraRuntime,
         SaveRoundTripPreservesCamerasAndPublishedGraphOwnership) {
    constexpr char kScene[] =
        "ACS3D v2\n"
        "N3D 10 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Rig\n"
        "N3D 20 10 -1 1 2 3 0 10 0 1 1 1 1 1 1 1 Gameplay\n"
        "CAM3D 20 gameplay.main 0 9 1 72 14 0.05 9000\n"
        "N3D 30 -1 -1 4 5 6 0 20 0 1 1 1 1 1 1 1 Cinematic\n"
        "CAM3D 30 cinematic.main 1 3 0 45 21 0.2 12000\n";

    CSceneNodeGraph source;
    FScene3DLoadResult loaded =
        TryLoadScene3DText(source, kScene, sizeof(kScene) - 1u);
    EXPECT_TRUE(loaded.Succeeded());
    EXPECT_EQ(loaded.CameraCount, 2u);

    const FScene3DSaveResult query =
        TrySaveScene3DText(source, nullptr, 0u);
    EXPECT_EQ(query.Error, EScene3DSerializeError::BufferTooSmall);
    EXPECT_EQ(query.CameraCount, 2u);

    char saved_text[4096]{};
    const FScene3DSaveResult saved =
        TrySaveScene3DText(source, saved_text, sizeof(saved_text));
    EXPECT_TRUE(saved.Succeeded());
    EXPECT_EQ(saved.CameraCount, 2u);
    EXPECT_EQ(saved.RequiredBytes, query.RequiredBytes);
    EXPECT_EQ(saved.RequiredBytes, std::strlen(saved_text) + 1u);
    EXPECT_TRUE(std::strstr(
        saved_text,
        "CAM3D 2 gameplay.main 0 9 1") != nullptr);
    EXPECT_TRUE(std::strstr(
        saved_text,
        "CAM3D 3 cinematic.main 1 3 0") != nullptr);

    CSceneNodeGraph destination;
    destination.Spawn(FStringView("MustBeReplaced"));
    const FScene3DLoadResult roundtrip = TryLoadScene3DText(
        destination, saved_text, saved.BytesWritten);
    EXPECT_TRUE(roundtrip.Succeeded());
    EXPECT_EQ(roundtrip.CameraCount, 2u);
    EXPECT_TRUE(roundtrip.ActiveCamera.IsAuthored);
    EXPECT_TRUE(std::strcmp(
        roundtrip.ActiveCamera.StableId, "gameplay.main") == 0);

    ANode* gameplay =
        destination.Root().FindBySerialId(roundtrip.ActiveCamera.NodeId);
    ANode* rig = destination.Root().FindBySerialId(1);
    EXPECT_TRUE(gameplay != nullptr);
    EXPECT_TRUE(rig != nullptr);
    if (gameplay == nullptr || rig == nullptr) return;
    EXPECT_TRUE(rig->Parent() == &destination.Root());
    EXPECT_TRUE(gameplay->Parent() == rig);
    EXPECT_TRUE(destination.Get(gameplay->Id()) == gameplay);
    EXPECT_TRUE(destination.Get(rig->Id()) == rig);
    const ACameraComponent3D* camera =
        gameplay->GetComponent<ACameraComponent3D>();
    EXPECT_TRUE(camera != nullptr);
    if (camera == nullptr) return;
    EXPECT_TRUE(std::strcmp(camera->StableId(), "gameplay.main") == 0);
    EXPECT_NEAR(camera->FovYDegrees(), 72.0f, 1.0e-6f);
}

ACS_TEST(LegacyScene3DCameraRuntime,
         CheckedComponentSetterIsStatePreserving) {
    ACameraComponent3D component;
    FScene3DCameraState valid;
    valid.IsAuthored = true;
    valid.Priority = 7;
    valid.FovYDegrees = 75.0f;
    valid.OrthographicHeight = 8.0f;
    valid.NearPlane = 0.2f;
    valid.FarPlane = 4000.0f;
    std::memcpy(valid.StableId, "safe-camera", 12u);
    EXPECT_TRUE(component.TrySetAuthoredState(valid));

    FScene3DCameraState invalid = valid;
    std::memset(
        invalid.StableId, 'x',
        sizeof(invalid.StableId));
    invalid.FovYDegrees = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_TRUE(!component.TrySetAuthoredState(invalid));
    EXPECT_TRUE(std::strcmp(component.StableId(), "safe-camera") == 0);
    EXPECT_EQ(component.Priority(), 7);
    EXPECT_NEAR(component.FovYDegrees(), 75.0f, 1.0e-6f);

    CSceneNodeGraph duplicate_component_scene;
    ANode& camera_node =
        duplicate_component_scene.Spawn(FStringView("Camera"));
    ACameraComponent3D& first =
        camera_node.AddComponent<ACameraComponent3D>();
    ACameraComponent3D& second =
        camera_node.AddComponent<ACameraComponent3D>();
    FScene3DCameraState first_state = valid;
    std::memcpy(first_state.StableId, "camera-a", 9u);
    FScene3DCameraState second_state = valid;
    std::memcpy(second_state.StableId, "camera-b", 9u);
    EXPECT_TRUE(first.TrySetAuthoredState(first_state));
    EXPECT_TRUE(second.TrySetAuthoredState(second_state));
    char output[1024]{};
    EXPECT_EQ(
        TrySaveScene3DText(
            duplicate_component_scene, output, sizeof(output)).Error,
        EScene3DSerializeError::DuplicateCamera);
}
