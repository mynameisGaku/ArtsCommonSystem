// SPDX-License-Identifier: Apache-2.0
// HelloPbr — FApplication 派生クラス。
// FPbrShader (Cook-Torrance) で metallic × roughness の material ball グリッドを
// 描画する。地面 + 5x5 の sphere + 1 directional light + 1 point light。
#pragma once

#include "app/Application.h"

#include "render/PbrShader.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "math/Camera.h"

namespace hellopbr {

// グリッドサイズと配置間隔。main.cpp / HelloPbrApp.cpp で共有。
inline constexpr acs::u32 kGridSize = 5;        // 5x5 = 25 sphere
inline constexpr acs::f32 kSpacing  = 1.4f;

class FHelloPbrApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::FPbrShader   m_Shader;
    acs::FGpuMesh     m_GmSphere;
    acs::FGpuMesh     m_GmPlane;
    acs::FSpriteBatch m_Batch;
    acs::FFont        m_Font;
    acs::FCamera      m_Camera;
    acs::FVec3        m_CamPos {0, 2.0f, -7.5f};
    acs::f32         m_CamYaw   = 0.0f;
    acs::f32         m_CamPitch = 0.0f;
    acs::f32         m_Time      = 0.0f;
};

} // namespace hellopbr
