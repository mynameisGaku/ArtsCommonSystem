// SPDX-License-Identifier: Apache-2.0
// Reversible AScene host for legacy ACS3D editor documents.
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Scene.h"
#include "gameframework/Scene3D.h"
#include "gameframework/Scene3DSerialize.h"
#include "math/Camera.h"
#include "render/Blit.h"
#include "render/PbrShader.h"
#include "render/PostProcess.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/SubsurfaceScattering.h"
#include "render/WaterSurface3D.h"
#include "threading/Thread.h"

#include <atomic>

namespace acs::game {

class IAssetPackReader;
class AMeshComponent3D;
class AWaterSurface3DComponent;

/**
 * Exact gameplay hit returned by ALegacyScene3DAdapter::RaycastWater.
 *
 * @details Point and normal are expressed in world space. Distance is the
 * parameter on the caller's ray (`origin + direction * distance`), so callers
 * may use normalized or deliberately scaled directions without a hidden
 * conversion.
 */
struct FWaterRaycastHit {
    FNodeId Node{};
    FVec3 Point{};
    FVec3 Normal{0.0f, 1.0f, 0.0f};
    f32 Distance = 0.0f;

    bool IsValid() const noexcept { return Node.IsValid(); }
};

/**
 * Active projection state for the canonical scene runtime.
 *
 * @details ACS3D CAM3D may author the initial projection. An editor 2D view may
 * still select Orthographic without converting the root graph or the dedicated
 * 2D renderer/physics subsystems.
 */
enum class ESceneProjectionMode : u8 {
    Perspective = 0,
    Orthographic = 1,
};

/**
 * Standalone AScene bridge for the legacy `ACS3D v2` document adapter.
 *
 * @details The scene graph is the AScene-owned ANode/FTransform3D graph shared with the editor
 * runtime (docs/SceneUnification.md Phase 2). This class is
 * intentionally an adapter rather than a second permanent scene asset type: packages expose one
 * `main.acscene` bootstrap entry and choose the legacy .acscene/.acs3d reader from its validated
 * header. Sprite batching, Canvas/UI, and 2D physics stay on their dedicated runtime path.
 */
class ALegacyScene3DAdapter : public AScene {
public:
    ALegacyScene3DAdapter() noexcept = default;
    ~ALegacyScene3DAdapter() noexcept override;

    ALegacyScene3DAdapter(const ALegacyScene3DAdapter&) = delete;
    ALegacyScene3DAdapter& operator=(const ALegacyScene3DAdapter&) = delete;

    /** Load a loose legacy ACS3D document and all of its mesh/material dependencies. */
    FScene3DLoadResult LoadFile(const char* path = "main.acscene") noexcept;

    /** Load an in-memory ACS3D document transactionally (tests/tools/hot reload). */
    FScene3DLoadResult LoadText(const char* text, u32 size) noexcept;

    /** Load a legacy ACS3D document and all dependencies from one mounted asset pack. */
    FScene3DLoadResult LoadAssetPack(
        IAssetPackReader& pack,
        const char* virtual_path = "main.acscene") noexcept;

    /** Last checked document/dependency result. */
    const FScene3DLoadResult& LoadResult() const noexcept { return m_LoadResult; }

    /** Select the active camera projection without changing the scene document. */
    void SetProjectionMode(ESceneProjectionMode mode) noexcept { m_Projection = mode; }

    /** Active camera projection. */
    ESceneProjectionMode ProjectionMode() const noexcept { return m_Projection; }

    /** Camera used for standalone preview/gameplay. */
    CCamera& Camera() noexcept { return m_Camera; }

    /** Read-only standalone camera. */
    const CCamera& Camera() const noexcept { return m_Camera; }

    /** Deterministically selected authored camera, or null for frame-scene fallback. */
    const FScene3DCameraState* AuthoredCamera() const noexcept {
        return m_UseAuthoredCamera ? &m_AuthoredCamera : nullptr;
    }

    /** Number of graph-owned authored camera components. */
    u32 CameraCount() const noexcept;

    /**
     * Switch by canonical stable identity. Unknown or effectively disabled
     * identities fail without changing the active camera.
     */
    bool SetActiveCamera(const char* stable_id) noexcept;

    /**
     * Switch by serialized N3D node id. Unknown or effectively disabled nodes
     * fail without changing the active camera.
     */
    bool SetActiveCamera(i32 node_id) noexcept;

    /** Return an explicit runtime camera cut to authored automatic selection. */
    bool ClearActiveCameraOverride() noexcept;

    /** Descriptive alias for returning to deterministic authored selection. */
    bool UseAutomaticCameraSelection() noexcept {
        return ClearActiveCameraOverride();
    }

    /** Refresh the active camera from its node's current world transform. */
    bool RefreshActiveCamera() noexcept {
        return RefreshAuthoredCameraPose();
    }

    /** Recompute a useful camera target/distance from all renderable nodes. */
    void FrameScene() noexcept;

    /**
     * Raycast authorable water without consuming or capturing platform input.
     *
     * @details Plane primitives use an exact bounded-plane test and custom
     * meshes use their authored triangles. A nearer visible opaque mesh rejects
     * a water hit, preventing interactions through foreground geometry.
     */
    bool RaycastWater(
        const FRay3& ray,
        FWaterRaycastHit& out_hit,
        f32 max_distance = 3.4028235e38f) const noexcept;

    /** Add an impact to one exact water surface. */
    bool AddWaterDisturbance(
        FNodeId surface,
        FVec3 world_point,
        f32 radius = 0.18f,
        f32 strength = 0.22f) noexcept;

    /** Add a directional wake to one exact water surface. */
    bool AddWaterWake(
        FNodeId surface,
        FVec3 world_point,
        FVec3 world_velocity,
        f32 radius = 0.20f,
        f32 strength = 0.14f) noexcept;

    /** Number of currently active disturbances owned by one water surface. */
    u32 ActiveWaterRippleCount(FNodeId surface) const noexcept {
        return m_Water.ActiveRippleCountForSurface(
            static_cast<u64>(surface.m_Packed));
    }

    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(FRenderContext& context) noexcept override;

private:
    struct FCustomGpuMesh {
        AMeshComponent3D* Component = nullptr;
        FGpuMesh Mesh;
    };

    enum class EWaterGpuState : u8 {
        Unavailable = 0,
        Compiling,
        CpuCompiling,
        PendingCommit,
        Buffering,
        Ready,
        Failed,
    };

    enum class ESkyGpuState : u8 {
        Unavailable = 0,
        Compiling,
        CpuCompiling,
        PendingCommit,
        Ready,
        Failed,
    };

    enum class EShaderGpuState : u8 {
        Unavailable = 0,
        Compiling,
        CpuCompiling,
        PendingCommit,
        Ready,
        Failed,
    };

    /** The one renderer subsystem allowed to publish GPU state this frame. */
    enum class EGpuCommitSubsystem : u8 {
        None = 0,
        Post,
        HdrPbr,
        HdrSsss,
        Subsurface,
        Sky,
        Blit,
        Water,
    };

    struct FWaterDraw {
        const ANode* Node = nullptr;
        const AMeshComponent3D* Mesh = nullptr;
        const AWaterSurface3DComponent* Water = nullptr;
        const FGpuMesh* Gpu = nullptr;
    };

    bool EnsureGpu(FRenderContext& context) noexcept;
    bool EnsureHdrFrameResources(
        IRhiDevice& device, u32 width, u32 height,
        EFormat swapchain_format, EFormat depth_format,
        EGpuCommitSubsystem& frame_commit) noexcept;
    bool BeginSkyCpuCompilation() noexcept;
    bool BeginWaterCpuCompilation() noexcept;
    bool BeginHdrPbrCpuCompilation(
        IRhiDevice& device,
        EFormat rt_format,
        EFormat depth_format) noexcept;
    bool BeginHdrSsssCpuCompilation(
        IRhiDevice& device,
        EFormat rt_format,
        EFormat depth_format) noexcept;
    bool BeginSubsurfaceCpuCompilation(
        IRhiDevice& device) noexcept;
    bool BeginPostCpuCompilation() noexcept;
    bool BeginBlitCpuCompilation() noexcept;
    static void SkyCpuCompileWorkerEntry(void* user) noexcept;
    static void WaterCpuCompileWorkerEntry(void* user) noexcept;
    static void HdrPbrCpuCompileWorkerEntry(void* user) noexcept;
    static void HdrSsssCpuCompileWorkerEntry(void* user) noexcept;
    static void SubsurfaceCpuCompileWorkerEntry(void* user) noexcept;
    static void PostCpuCompileWorkerEntry(void* user) noexcept;
    static void BlitCpuCompileWorkerEntry(void* user) noexcept;
    void JoinCpuCompileWorkers() noexcept;
    static bool TryClaimGpuCommit(
        EGpuCommitSubsystem& frame_commit,
        EGpuCommitSubsystem subsystem) noexcept;
    void AdvanceSkyInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit) noexcept;
    void AdvanceWaterInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit,
        bool scene_has_water) noexcept;
    void AdvanceHdrPbrInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit) noexcept;
    void AdvanceHdrSsssInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit,
        bool scene_needs_subsurface) noexcept;
    void AdvanceSubsurfaceInitialization(
        IRhiDevice& device, u32 width, u32 height,
        EGpuCommitSubsystem& frame_commit,
        bool scene_needs_subsurface) noexcept;
    void EnsureSubsurfaceAuxTargets(
        IRhiDevice& device, u32 width, u32 height,
        EGpuCommitSubsystem& frame_commit,
        bool scene_needs_subsurface) noexcept;
    void AdvancePostInitialization(
        IRhiDevice& device, u32 width, u32 height,
        EFormat swapchain_format,
        EGpuCommitSubsystem& frame_commit) noexcept;
    void AdvanceBlitInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit,
        bool requested) noexcept;
    bool UploadGraphMeshes(IRhiDevice& device) noexcept;
    void DrainAndReleaseGpu() noexcept;
    void ReleaseGpu() noexcept;
    void UpdateCameraProjection(u32 width, u32 height) noexcept;
    void UpdateCameraView() noexcept;
    void AdoptLoadedCamera() noexcept;
    bool RefreshAuthoredCameraPose() noexcept;
    const FGpuMesh* GpuMeshFor(const AMeshComponent3D& component) const noexcept;
    u32 CollectWaterDraws(
        FWaterDraw (&draws)[CWaterSurface3D::kMaxTrackedSurfaces],
        IRhiTexture* depth, u32 width, u32 height) const noexcept;
    bool DrawPbrScene(
        FRenderContext& context,
        CPbrShader& shader,
        const FWaterDraw* excluded_water,
        u32 excluded_count,
        bool subsurface_mrt = false) noexcept;
    void DrawWaterScene(
        FRenderContext& context,
        const FWaterDraw* water_draws,
        u32 water_count,
        IRhiTexture& background,
        IRhiTexture& opaque_depth_snapshot) noexcept;
    void DrawWaterFallback(
        FRenderContext& context,
        const FWaterDraw* water_draws,
        u32 water_count) noexcept;
    CPbrShader& ActiveHdrShader() noexcept {
        return m_HdrShaders[m_HdrActiveSlot];
    }
    const CPbrShader& ActiveHdrShader() const noexcept {
        return m_HdrShaders[m_HdrActiveSlot];
    }

    FScene3DLoadResult m_LoadResult{};
    CPbrShader m_HdrShaders[2];
    CPbrShader::FCompiledShaders m_HdrPendingShaders{};
    FThread m_HdrCompileWorker;
    std::atomic<i32> m_HdrCompileWorkerState{0};
    IRhiDevice* m_HdrCompileDevice = nullptr;
    EFormat m_HdrCompileRtFormat = EFormat::R16G16B16A16_Float;
    EFormat m_HdrCompileDepthFormat = EFormat::D32_Float;
    u8 m_HdrActiveSlot = 0u;
    u8 m_HdrPendingSlot = 1u;
    bool m_HdrPendingIsInitialized = false;
    CPbrShader::FCompiledShaders m_HdrSsssPendingShaders{};
    FThread m_HdrSsssCompileWorker;
    std::atomic<i32> m_HdrSsssCompileWorkerState{0};
    IRhiDevice* m_HdrSsssCompileDevice = nullptr;
    EFormat m_HdrSsssCompileRtFormat = EFormat::R16G16B16A16_Float;
    EFormat m_HdrSsssCompileDepthFormat = EFormat::D32_Float;
    u8 m_HdrSsssPendingSlot = 0u;
    bool m_HdrSsssPendingIsInitialized = false;
    CSubsurfaceScattering m_Ssss;
    CSubsurfaceScattering::FCompiledShaders m_SsssPendingShaders{};
    FThread m_SsssCompileWorker;
    std::atomic<i32> m_SsssCompileWorkerState{0};
    IRhiDevice* m_SsssCompileDevice = nullptr;
    bool m_SsssPendingIsInitialized = false;
    TUniquePtr<IRhiTexture> m_SsssDiffuse;
    TUniquePtr<IRhiTexture> m_SsssMaterial;
    TUniquePtr<IRhiTexture> m_SsssNormal;
    TUniquePtr<IRhiTexture> m_SsssPendingDiffuse;
    TUniquePtr<IRhiTexture> m_SsssPendingMaterial;
    TUniquePtr<IRhiTexture> m_SsssPendingNormal;
    CPostProcess m_Post;
    CPostProcess::FCompiledShaders m_PostPendingShaders{};
    FThread m_PostCompileWorker;
    std::atomic<i32> m_PostCompileWorkerState{0};
    CBlit m_Blit;
    CBlit::FCompiledShaders m_BlitPendingShaders{};
    FThread m_BlitCompileWorker;
    std::atomic<i32> m_BlitCompileWorkerState{0};
    CSky m_Sky;
    CSky::FCompiledShaders m_SkyPendingShaders{};
    FThread m_SkyCompileWorker;
    std::atomic<i32> m_SkyCompileWorkerState{0};
    CWaterSurface3D m_Water;
    CWaterSurface3D::FCompiledShaders m_WaterPendingShaders{};
    FThread m_WaterCompileWorker;
    std::atomic<i32> m_WaterCompileWorkerState{0};
    TUniquePtr<IRhiTexture> m_WaterBackground;
    TUniquePtr<IRhiTexture> m_WaterDepthSnapshot;
    FGpuMesh m_Cube;
    FGpuMesh m_Sphere;
    FGpuMesh m_Plane;
    TArray<FCustomGpuMesh> m_CustomMeshes;
    CCamera m_Camera;
    FScene3DCameraState m_AuthoredCamera{};
    bool m_UseAuthoredCamera = false;
    bool m_HasExplicitCameraOverride = false;
    i32 m_ActiveCameraNodeId = -1;
    FVec3 m_Target{0.0f, 0.0f, 0.0f};
    f32 m_Distance = 8.0f;
    f32 m_Yaw = 0.0f;
    f32 m_Pitch = 0.22f;
    f32 m_Time = 0.0f;
    FPostProcessParams m_PostParams{};
    ESceneProjectionMode m_Projection = ESceneProjectionMode::Perspective;
    EShaderGpuState m_HdrShaderGpuState = EShaderGpuState::Unavailable;
    EShaderGpuState m_HdrSsssGpuState = EShaderGpuState::Unavailable;
    EShaderGpuState m_SsssGpuState = EShaderGpuState::Unavailable;
    EShaderGpuState m_PostGpuState = EShaderGpuState::Unavailable;
    EShaderGpuState m_BlitGpuState = EShaderGpuState::Unavailable;
    ESkyGpuState m_SkyGpuState = ESkyGpuState::Unavailable;
    EWaterGpuState m_WaterGpuState = EWaterGpuState::Unavailable;
    u32 m_FrameWidth = 0u;
    u32 m_FrameHeight = 0u;
    u32 m_BackgroundAttemptWidth = 0u;
    u32 m_BackgroundAttemptHeight = 0u;
    u32 m_DepthAttemptWidth = 0u;
    u32 m_DepthAttemptHeight = 0u;
    u32 m_SsssResizeAttemptWidth = 0u;
    u32 m_SsssResizeAttemptHeight = 0u;
    u32 m_SsssAuxAttemptWidth = 0u;
    u32 m_SsssAuxAttemptHeight = 0u;
    u32 m_SsssPendingAuxWidth = 0u;
    u32 m_SsssPendingAuxHeight = 0u;
    EFormat m_FrameDepthFormat = EFormat::D32_Float;
    bool m_DepthSnapshotFailed = false;
    bool m_SsssRequested = false;
    bool m_GpuReady = false;
    bool m_GpuAttempted = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FLegacyScene3DAdapter = ALegacyScene3DAdapter;

} // namespace acs::game
