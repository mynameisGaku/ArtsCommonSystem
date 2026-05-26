// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — カメラ操作とレイキャスト判定。
//
// 役割:
//   ・WASD / 矢印キーでカメラの位置と yaw / pitch を更新する。
//   ・カメラ前方ベクトルに沿ったレイで RaycastTargets の最近傍ヒットを求める。
//
// なぜ独立クラスにしたか:
//   ・「入力 → カメラ → レイ判定」は描画と直交した責務。分離するとサンプル中で
//     カメラ操作だけを差し替えやすい (例: 将来マウス Look 対応に置き換えるなど)。
#pragma once

#include "Types.h"

#include "math/Camera.h"
#include "math/Vec.h"

namespace helloraycast3d {

class RaycastTargets;

class RayCaster {
public:
    // aspect は描画開始時の swapchain 比率。Perspective 行列を構築する。
    void Init(acs::f32 aspect) noexcept;

    // 入力でカメラを動かしてから、targets に対するレイテストを行い結果を書き戻す。
    void Update(acs::f32 dt, RaycastTargets& targets) noexcept;

    // 描画側 (StandardShader::SetLights) が ViewProjection と Eye を要求する。
    const acs::FCamera& FCamera()  const noexcept { return _camera; }
    acs::FVec3          Eye()     const noexcept { return _cam_pos; }
    acs::FVec3          Forward() const noexcept { return _cam_forward; }

private:
    acs::FCamera _camera;
    acs::FVec3   _cam_pos     = kCamInitialPos;
    acs::FVec3   _cam_forward {0, 0, 1};
    acs::f32    _cam_yaw     = 0.0f;
    acs::f32    _cam_pitch   = 0.0f;
};

} // namespace helloraycast3d
