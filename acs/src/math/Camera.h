// SPDX-License-Identifier: Apache-2.0
// カメラ（ビュー行列 + プロジェクション行列のヘルパ）
//
// 使い方:
//   Camera cam;
//   cam.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 1000.0f);
//   cam.SetLookAt({0,2,-5}, {0,0,0}, Vec3::Up());
//   Mat4 view_proj = cam.ViewProjection();   // GPU に送る用
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Math.h"

namespace acs {

class Camera {
public:
    Camera() noexcept = default;

    // パースペクティブ投影（左手系: Z+ が画面奥）
    void SetPerspective(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) noexcept {
        _projection = Mat4::PerspectiveFovLH(fov_y_rad, aspect, near_z, far_z);
    }
    // 正射影投影
    void SetOrthographic(f32 width, f32 height, f32 near_z, f32 far_z) noexcept {
        _projection = Mat4::OrthoLH(width, height, near_z, far_z);
    }

    // 注視点指定でビュー行列を設定
    void SetLookAt(Vec3 eye, Vec3 target, Vec3 up = Vec3::Up()) noexcept {
        _eye = eye;
        _view = Mat4::LookAtLH(eye, target, up);
    }

    // 行列直接アクセス
    const Mat4& View()           const noexcept { return _view; }
    const Mat4& Projection()     const noexcept { return _projection; }
    Mat4        ViewProjection() const noexcept { return _view * _projection; }
    Vec3        Eye()            const noexcept { return _eye; }

    // アスペクト比だけ更新（ウィンドウリサイズ時用）
    void UpdateAspect(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) noexcept {
        SetPerspective(fov_y_rad, aspect, near_z, far_z);
    }

private:
    Mat4 _view;
    Mat4 _projection;
    Vec3 _eye{0, 0, 0};
};

} // namespace acs
