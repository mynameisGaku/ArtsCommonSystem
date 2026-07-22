// SPDX-License-Identifier: Apache-2.0
// Reversible FScene host for legacy ACS3D editor documents.
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Scene.h"
#include "gameframework/Scene3D.h"
#include "gameframework/Scene3DSerialize.h"
#include "math/Camera.h"
#include "render/PbrShader.h"
#include "render/RenderAssets.h"

namespace acs::game {

class IAssetPackReader;
class AMeshComponent3D;

/**
 * Per-camera projection state for the canonical scene runtime.
 *
 * @details This is runtime camera state, not a property of the scene asset. The legacy ACS3D
 * adapter starts in Perspective; an editor 2D mode may select Orthographic without converting
 * the root graph or the dedicated 2D renderer/physics subsystems.
 */
enum class ESceneProjectionMode : u8 {
    Perspective = 0,
    Orthographic = 1,
};

/**
 * Standalone FScene bridge for the legacy `ACS3D v2` document adapter.
 *
 * @details The owned graph is the same ANode/FTransform3D graph used by the editor. This class is
 * intentionally an adapter rather than a second permanent scene asset type: packages expose one
 * `main.acscene` bootstrap entry and choose the legacy .acscene/.acs3d reader from its validated
 * header. Sprite batching, Canvas/UI, and 2D physics stay on their dedicated runtime path.
 */
class FLegacyScene3DAdapter : public FScene {
public:
    FLegacyScene3DAdapter() noexcept = default;
    ~FLegacyScene3DAdapter() noexcept override = default;

    FLegacyScene3DAdapter(const FLegacyScene3DAdapter&) = delete;
    FLegacyScene3DAdapter& operator=(const FLegacyScene3DAdapter&) = delete;

    /** Load a loose legacy ACS3D document and all of its mesh/material dependencies. */
    FScene3DLoadResult LoadFile(const char* path = "main.acscene") noexcept;

    /** Load a legacy ACS3D document and all dependencies from one mounted asset pack. */
    FScene3DLoadResult LoadAssetPack(
        IAssetPackReader& pack,
        const char* virtual_path = "main.acscene") noexcept;

    /** Mutable canonical ANode graph used by gameplay components. */
    FScene3D& Graph() noexcept { return m_Graph; }

    /** Read-only canonical ANode graph. */
    const FScene3D& Graph() const noexcept { return m_Graph; }

    /** Last checked document/dependency result. */
    const FScene3DLoadResult& LoadResult() const noexcept { return m_LoadResult; }

    /** Select the active camera projection without changing the scene document. */
    void SetProjectionMode(ESceneProjectionMode mode) noexcept { m_Projection = mode; }

    /** Active camera projection. */
    ESceneProjectionMode ProjectionMode() const noexcept { return m_Projection; }

    /** Camera used for standalone preview/gameplay. */
    FCamera& Camera() noexcept { return m_Camera; }

    /** Read-only standalone camera. */
    const FCamera& Camera() const noexcept { return m_Camera; }

    /** Recompute a useful camera target/distance from all renderable nodes. */
    void FrameScene() noexcept;

    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnFixedUpdate(f32 fixed_dt) noexcept override;
    void OnRender(FRenderContext& context) noexcept override;

private:
    struct FCustomGpuMesh {
        AMeshComponent3D* Component = nullptr;
        FGpuMesh Mesh;
    };

    bool EnsureGpu(FRenderContext& context) noexcept;
    bool UploadGraphMeshes(IRhiDevice& device) noexcept;
    void ReleaseGpu() noexcept;
    void UpdateCameraProjection(u32 width, u32 height) noexcept;
    void UpdateCameraView() noexcept;
    const FGpuMesh* GpuMeshFor(const AMeshComponent3D& component) const noexcept;

    FScene3D m_Graph;
    FScene3DLoadResult m_LoadResult{};
    FPbrShader m_Shader;
    FGpuMesh m_Cube;
    FGpuMesh m_Sphere;
    FGpuMesh m_Plane;
    TArray<FCustomGpuMesh> m_CustomMeshes;
    FCamera m_Camera;
    FVec3 m_Target{0.0f, 0.0f, 0.0f};
    f32 m_Distance = 8.0f;
    f32 m_Yaw = 0.0f;
    f32 m_Pitch = 0.22f;
    f32 m_Time = 0.0f;
    ESceneProjectionMode m_Projection = ESceneProjectionMode::Perspective;
    bool m_GpuReady = false;
    bool m_GpuAttempted = false;
};

} // namespace acs::game
