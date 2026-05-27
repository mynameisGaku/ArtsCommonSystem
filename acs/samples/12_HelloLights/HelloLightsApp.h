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
    LightsScene             _scene;
    acs::FStandardShader     _shader;
    acs::GpuMesh            _gm_cube;
    acs::GpuMesh            _gm_sphere;
    acs::GpuMesh            _gm_plane;
    acs::FSpriteBatch        _batch;
    acs::Font               _font;
    acs::FCamera             _camera;
    acs::FVec3               _cam_pos    {0, 3, -8};
    acs::f32                _cam_yaw    = 0.0f;
    acs::f32                _cam_pitch  = 0.0f;
    acs::f32                _time       = 0.0f;
};

} // namespace hellolights
