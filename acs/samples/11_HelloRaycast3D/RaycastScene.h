// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — シーン本体。
// 8 個のオブジェクト (球 / 立方体) を保持し、カメラ操作 + 前方レイ判定 +
// マルチライト Blinn-Phong 描画 + HUD を担当する。
//
// Application 派生はリソース所有 (StandardShader / SpriteBatch / Font) を
// 担当し、毎フレームの update / render を RaycastScene に委譲する。
#pragma once

#include "Types.h"

#include "render/StandardShader.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiCommandList.h"

#include "math/Camera.h"
#include "math/Vec.h"

namespace helloraycast3d {

class RaycastScene {
public:
    // プリミティブを GPU にアップロードしてカメラ + オブジェクト配置を構成。
    bool Init(acs::IRhiDevice& dev, acs::f32 aspect) noexcept;

    void Shutdown() noexcept;

    // カメラ入力 + レイキャスト判定。
    void Update(acs::f32 dt) noexcept;

    // 3D + HUD を描画。HUD 用の screen_w / screen_h は SpriteBatch::Begin に必要。
    void Render(acs::StandardShader& shader,
                acs::IRhiCommandList& cl,
                acs::SpriteBatch& batch,
                acs::Font& font,
                acs::u32 screen_w,
                acs::u32 screen_h) noexcept;

private:
    // HSV→RGB（0..1 範囲）
    static acs::Vec3 HsvToRgb(acs::f32 h, acs::f32 s, acs::f32 v) noexcept;

    acs::GpuMesh _gm_sphere, _gm_cube, _gm_plane;

    Object _objects[kNumObjects] {};
    acs::Camera _camera;
    acs::Vec3   _cam_pos     {0, 2, -8};
    acs::Vec3   _cam_forward {0, 0, 1};
    acs::f32    _cam_yaw   = 0.0f;
    acs::f32    _cam_pitch = 0.0f;
    acs::f32    _time      = 0.0f;
    acs::i32    _hit_index = -1;
    acs::Vec3   _hit_point;
};

} // namespace helloraycast3d
