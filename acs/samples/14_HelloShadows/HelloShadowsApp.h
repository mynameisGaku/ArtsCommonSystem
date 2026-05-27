// SPDX-License-Identifier: Apache-2.0
// HelloShadows — FApplication 派生クラス。
// 太陽光 + シャドウマッピング (PCSS) で 8 個のキャスタを地面に影として落とす。
//
// リソース所有 (sky / shader / shadow / sprite / font / camera) と入力
// ハンドリングを担当し、毎フレームの「シャドウパス + 主パス」描画は
// ShadowsScene に委譲する。
#pragma once

#include "ShadowsScene.h"

#include "app/Application.h"
#include "math/Camera.h"
#include "render/Font.h"
#include "render/RenderAssets.h"
#include "render/ShadowMap.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/StandardShader.h"

namespace helloshadows {

class HelloShadowsApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    ShadowsScene             m_Scene;
    acs::FSky                 m_Sky;
    acs::FStandardShader      m_Shader;
    acs::FShadowMap           m_Shadow;
    acs::GpuMesh             m_GmCube;
    acs::GpuMesh             m_GmSphere;
    acs::GpuMesh             m_GmPlane;
    acs::FSpriteBatch         m_Batch;
    acs::Font                m_Font;
    acs::FCamera              m_Camera;
    acs::FVec3                m_CamPos    {0, 4, -10};
    acs::f32                 m_CamYaw    = 0.0f;
    acs::f32                 m_CamPitch  = 0.0f;
    acs::f32                 m_SunYaw    = 0.5f;
    acs::f32                 m_Time       = 0.0f;
};

} // namespace helloshadows
