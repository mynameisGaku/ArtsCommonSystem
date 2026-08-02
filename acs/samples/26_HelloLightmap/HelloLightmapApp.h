// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — CApplication 派生クラス。
//
// Cornell box シーンを構築し、起動時に CPU で multi-bounce path tracing を行って
// HDR lightmap を焼く。以降のフレームは CPbrShader の lightmap slot 経由で
// 焼いた irradiance を mesh の uv で引いて表示する。動的ライトは使わず、ごく弱い
// ambient のみ。赤/緑の壁の照り返しが床に色づく color bleeding が見える。
//
// レンダリング:
//   ・HDR CPostProcess (Bloom + ACES tonemap)。HDR lightmap の高輝度が tonemap で
//     自然にロールオフし、bloom で天井が光る。
//   ・CSpriteBatch + FFont は tonemap 後の LDR backbuffer に直接描く HUD。
//
// 操作:
//   WASD 移動 / 矢印 視点 / L: lightmap on-off / Esc: 終了
#pragma once

#include "app/Application.h"
#include "render/PbrShader.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/PostProcess.h"
#include "math/Camera.h"
#include "math/Vec.h"
#include "foundation/Types.h"

#include "LightmapTypes.h"

namespace hellolightmap {

class CHelloLightmapApp : public acs::CApplication {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::CPbrShader         m_Pbr;
    acs::CPostProcess       m_Post;
    acs::FPostProcessParams m_PostParams;
    FQuad                   m_Quads[kQuadCount];
    acs::CSpriteBatch       m_Batch;
    acs::FFont              m_Font;
    acs::CCamera            m_Camera;
    acs::FVec3              m_CamPos{0, 1.0f, -0.9f};
    acs::f32               m_CamYaw   = 0.0f;
    acs::f32               m_CamPitch = 0.0f;
    bool                   m_bShowLightmap = true;
};

} // namespace hellolightmap
