// SPDX-License-Identifier: Apache-2.0
// Interactive 3D water showcase.
#pragma once

#include "app/Application.h"
#include "foundation/Types.h"
#include "math/Camera.h"
#include "math/Vec.h"
#include "memory/UniquePtr.h"
#include "render/Blit.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"
#include "render/PostProcess.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/StandardShader.h"
#include "render/WaterSurface3D.h"

namespace hellowater3d {

class FHelloWater3DApp final : public acs::FApplication {
public:
    FHelloWater3DApp() noexcept = default;
    ~FHelloWater3DApp() noexcept override = default;

    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::FEvent& event) noexcept override;

private:
    bool ResizeFrameResources(acs::u32 width, acs::u32 height,
                              bool resize_post_process) noexcept;
    bool MouseToWater(acs::FVec3& world_point) noexcept;
    void UpdateCamera() noexcept;

    acs::FPostProcess m_Post;
    acs::FSky m_Sky;
    acs::FStandardShader m_OpaqueShader;
    acs::FWaterSurface3D m_Water;
    acs::FBlit m_Blit;
    acs::FSpriteBatch m_Batch;
    acs::FFont m_Font;
    acs::FGpuMesh m_WaterMesh;
    acs::FGpuMesh m_FloorMesh;
    acs::TUniquePtr<acs::IRhiTexture> m_FloorTexture;
    acs::TUniquePtr<acs::IRhiTexture> m_SceneCopy;
    acs::FPostProcessParams m_PostParams;
    acs::FCamera m_Camera;

    acs::FVec3 m_CameraPosition{0, 4.8f, -9.0f};
    acs::FVec3 m_CameraTarget{0, -0.15f, 0};
    acs::f32 m_CameraYaw = 0.22f;
    acs::f32 m_CameraPitch = 0.48f;
    acs::f32 m_CameraDistance = 10.8f;
    acs::f32 m_FrameDt = 1.0f / 60.0f;

    acs::FVec3 m_LastDragPoint{};
    acs::f32 m_DragTravel = 0.0f;
    bool m_HasLastDragPoint = false;
    bool m_MouseHitsWater = false;
};

} // namespace hellowater3d
