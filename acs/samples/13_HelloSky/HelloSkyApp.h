// SPDX-License-Identifier: Apache-2.0
// HelloSky — CApplication 派生クラス。
// 手続き生成スカイ (昼/夕焼け/夜のプリセット切替可) で 1 つの回転球を照らす。
//
// このクラスはリソース所有 (sky / shader / sprite / font / camera) と入力
// ハンドリングのみを担当し、毎フレームの描画ロジックは SkyScene に委譲する
// (役割分離 — 描画だけ単体テストしたいときも置き換えやすい)。
#pragma once

#include "SkyScene.h"

#include "app/Application.h"
#include "math/Camera.h"
#include "render/Font.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/StandardShader.h"

namespace hellosky {

class CHelloSkyApp : public acs::CApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    CSkyScene             m_Scene;
    acs::CSky             m_Sky;
    acs::CStandardShader  m_Shader;
    acs::CSpriteBatch     m_Batch;
    acs::FFont            m_Font;
    acs::FGpuMesh         m_GmSphere;
    acs::FGpuMesh         m_GmPlane;
    acs::CCamera          m_Camera;
    acs::FVec3            m_CamPos;
    acs::f32             m_CamYaw = 0.5f;
    acs::f32             m_Angle   = 0.0f;
};

} // namespace hellosky
