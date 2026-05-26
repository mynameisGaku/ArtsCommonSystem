// SPDX-License-Identifier: Apache-2.0
// HelloAnimation — FApplication 派生クラス。
// 4 ボーンの「ヘビ風」スキンメッシュを GPU スキニングで描画するデモ。
//
// リソース所有 (sky / shader / sprite / font / camera / mesh / player) と
// 入力ハンドリングを担当し、毎フレームの「FSky → 地面 → スキンメッシュ」
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

class HelloAnimationApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    AnimationScene                _scene;
    acs::FSky                      _sky;
    acs::FSkinnedShader            _shader;       // スキンメッシュ用
    acs::FStandardShader           _std_shader;   // 地面用
    acs::FSpriteBatch              _batch;
    acs::FFont                     _font;

    acs::TRc<acs::FSkinnedMeshAsset> _snake;
    acs::FSkinnedGpuMesh            _gm_snake;
    acs::FGpuMesh                   _gm_plane;
    acs::FAnimationPlayer           _player;

    acs::FCamera                   _camera;
    acs::FVec3                     _cam_pos;
    acs::f32                      _cam_yaw = 0.6f;
};

} // namespace helloanim
