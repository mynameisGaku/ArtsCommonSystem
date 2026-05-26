// SPDX-License-Identifier: Apache-2.0
// HelloSky — Application 派生クラス。
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

class HelloSkyApp : public acs::Application {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    SkyScene             _scene;
    acs::Sky             _sky;
    acs::StandardShader  _shader;
    acs::SpriteBatch     _batch;
    acs::Font            _font;
    acs::GpuMesh         _gm_sphere;
    acs::GpuMesh         _gm_plane;
    acs::Camera          _camera;
    acs::FVec3            _cam_pos;
    acs::f32             _cam_yaw = 0.5f;
    acs::f32             _angle   = 0.0f;
};

} // namespace hellosky
