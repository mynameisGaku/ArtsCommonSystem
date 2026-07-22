// SPDX-License-Identifier: Apache-2.0
// HelloBloom — HDR シーン + Bloom + ACES Tonemap のミニデモ。
//
// OnCustomFrame() を override して HDR レンダーターゲット経由の描画パイプラインを
// 組む。HUD は Tonemap 後の LDR backbuffer に直接書く (HDR 値が HUD に乗らないように)。
// Bloom 強度は 1/2/3 キーで 3 段切替できる (Bloom の効きを目視確認しやすくするため)。
#pragma once

#include "app/Application.h"
#include "render/StandardShader.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/PostProcess.h"
#include "math/Camera.h"
#include "foundation/Types.h"

namespace hellobloom {

class FHelloBloomApp : public acs::FApplication {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::FPostProcess       m_Post;
    acs::FSky               m_Sky;
    acs::FStandardShader    m_Shader;
    acs::FSpriteBatch       m_Batch;
    acs::FFont              m_Font;
    acs::FGpuMesh           m_GmSphere;
    acs::FGpuMesh           m_GmPlane;
    acs::FCamera            m_Camera;
    acs::FPostProcessParams m_Params;
    acs::f32               m_CamYaw = 0.5f;
    acs::f32               m_Angle   = 0.0f;
};

} // namespace hellobloom
