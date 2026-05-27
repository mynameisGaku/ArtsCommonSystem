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

class HelloPbrApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::FPbrShader   _shader;
    acs::GpuMesh     _gm_sphere;
    acs::GpuMesh     _gm_plane;
    acs::FSpriteBatch _batch;
    acs::Font        _font;
    acs::FCamera      _camera;
    acs::FVec3        _cam_pos {0, 2.0f, -7.5f};
    acs::f32         _cam_yaw   = 0.0f;
    acs::f32         _cam_pitch = 0.0f;
    acs::f32         _time      = 0.0f;
};

} // namespace hellopbr
