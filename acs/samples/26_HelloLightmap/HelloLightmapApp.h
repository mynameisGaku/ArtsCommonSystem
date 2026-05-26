// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — Application 派生クラス。
//
// Cornell box シーンを構築し、起動時に CPU で multi-bounce path tracing を行って
// HDR lightmap を焼く。以降のフレームは PbrShader の lightmap slot 経由で
// 焼いた irradiance を mesh の uv で引いて表示する。動的ライトは使わず、ごく弱い
// ambient のみ。赤/緑の壁の照り返しが床に色づく color bleeding が見える。
//
// レンダリング:
//   ・HDR PostProcess (Bloom + ACES tonemap)。HDR lightmap の高輝度が tonemap で
//     自然にロールオフし、bloom で天井が光る。
//   ・SpriteBatch + Font は tonemap 後の LDR backbuffer に直接描く HUD。
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

class HelloLightmapApp : public acs::Application {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::PbrShader         _pbr;
    acs::PostProcess       _post;
    acs::PostProcessParams _post_params;
    Quad                   _quads[kQuadCount];
    acs::SpriteBatch       _batch;
    acs::Font              _font;
    acs::Camera            _camera;
    acs::Vec3              _cam_pos{0, 1.0f, -0.9f};
    acs::f32               _cam_yaw   = 0.0f;
    acs::f32               _cam_pitch = 0.0f;
    bool                   _show_lightmap = true;
};

} // namespace hellolightmap
