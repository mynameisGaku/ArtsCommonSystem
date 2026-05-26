// SPDX-License-Identifier: Apache-2.0
// HelloAnimation — Application 派生クラス。
// 4 ボーンの「ヘビ風」スキンメッシュを GPU スキニングで描画するデモ。
//
// リソース所有 (sky / shader / sprite / font / camera / mesh / player) と
// 入力ハンドリングを担当し、毎フレームの「Sky → 地面 → スキンメッシュ」
// 描画は AnimationScene に委譲する。
#pragma once

#include "AnimationScene.h"

#include "app/Application.h"
#include "asset/SkinnedMesh.h"
#include "math/Camera.h"
#include "memory/Rc.h"
#include "render/Font.h"
#include "render/RenderAssets.h"
#include "render/SkinnedShader.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/StandardShader.h"

namespace helloanim {

class HelloAnimationApp : public acs::Application {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    AnimationScene                _scene;
    acs::Sky                      _sky;
    acs::SkinnedShader            _shader;       // スキンメッシュ用
    acs::StandardShader           _std_shader;   // 地面用
    acs::SpriteBatch              _batch;
    acs::Font                     _font;

    acs::TRc<acs::SkinnedMeshAsset> _snake;
    acs::SkinnedGpuMesh            _gm_snake;
    acs::GpuMesh                   _gm_plane;
    acs::AnimationPlayer           _player;

    acs::Camera                   _camera;
    acs::FVec3                     _cam_pos;
    acs::f32                      _cam_yaw = 0.6f;
};

} // namespace helloanim
