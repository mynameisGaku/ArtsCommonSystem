// SPDX-License-Identifier: Apache-2.0
// カメラ（ビュー行列 + プロジェクション行列のヘルパ）
//
// 使い方:
//   FCamera cam;
//   cam.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 1000.0f);
//   cam.SetLookAt({0,2,-5}, {0,0,0}, FVec3::Up());
//   FMat4 view_proj = cam.ViewProjection();   // GPU に送る用
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Math.h"

namespace acs {

class FCamera {
public:
    FCamera() noexcept = default;

    // パースペクティブ投影（左手系: Z+ が画面奥）
    void SetPerspective(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) noexcept {
        m_Projection = FMat4::PerspectiveFovLH(fov_y_rad, aspect, near_z, far_z);
    }
    // 正射影投影
    void SetOrthographic(f32 width, f32 height, f32 near_z, f32 far_z) noexcept {
        m_Projection = FMat4::OrthoLH(width, height, near_z, far_z);
    }

    // 注視点指定でビュー行列を設定
    void SetLookAt(FVec3 eye, FVec3 target, FVec3 up = FVec3::Up()) noexcept {
        m_Eye = eye;
        m_View = FMat4::LookAtLH(eye, target, up);
    }

    // 行列直接アクセス
    const FMat4& View()           const noexcept { return m_View; }
    const FMat4& Projection()     const noexcept { return m_Projection; }
    FMat4        ViewProjection() const noexcept { return m_View * m_Projection; }
    FVec3        Eye()            const noexcept { return m_Eye; }

    // アスペクト比だけ更新（ウィンドウリサイズ時用）
    void UpdateAspect(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) noexcept {
        SetPerspective(fov_y_rad, aspect, near_z, far_z);
    }

private:
    FMat4 m_View;
    FMat4 m_Projection;
    FVec3 m_Eye{0, 0, 0};
};

} // namespace acs
