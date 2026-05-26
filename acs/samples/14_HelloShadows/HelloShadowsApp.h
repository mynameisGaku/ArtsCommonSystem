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
    ShadowsScene             _scene;
    acs::FSky                 _sky;
    acs::FStandardShader      _shader;
    acs::FShadowMap           _shadow;
    acs::FGpuMesh             _gm_cube;
    acs::FGpuMesh             _gm_sphere;
    acs::FGpuMesh             _gm_plane;
    acs::FSpriteBatch         _batch;
    acs::FFont                _font;
    acs::FCamera              _camera;
    acs::FVec3                _cam_pos    {0, 4, -10};
    acs::f32                 _cam_yaw    = 0.0f;
    acs::f32                 _cam_pitch  = 0.0f;
    acs::f32                 _sun_yaw    = 0.5f;
    acs::f32                 _time       = 0.0f;
};

} // namespace helloshadows
