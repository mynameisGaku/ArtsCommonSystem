// SPDX-License-Identifier: Apache-2.0
// HelloLights — FApplication 派生クラス。
//
// 役割分担: このクラスは GPU リソースとカメラ入力の所有者。毎フレームの
// ライト計算と物体描画は LightsScene に委譲し、App は薄く保つ。
#pragma once

#include "LightsScene.h"

#include "app/Application.h"
#include "math/Camera.h"
#include "render/Font.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/StandardShader.h"

namespace hellolights {

class HelloLightsApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    LightsScene             m_Scene;
    acs::FStandardShader     m_Shader;
    acs::GpuMesh            m_GmCube;
    acs::GpuMesh            m_GmSphere;
    acs::GpuMesh            m_GmPlane;
    acs::FSpriteBatch        m_Batch;
    acs::Font               m_Font;
    acs::FCamera             m_Camera;
    acs::FVec3               m_CamPos    {0, 3, -8};
    acs::f32                m_CamYaw    = 0.0f;
    acs::f32                m_CamPitch  = 0.0f;
    acs::f32                m_Time       = 0.0f;
};

} // namespace hellolights
