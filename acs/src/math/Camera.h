// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Math.h"

namespace acs {

/**
 * ビュー行列とプロジェクション行列を保持するカメラヘルパ。
 *
 * @details
 * 左手系 (Z+ が画面奥) で透視/正射影を設定し、注視点指定でビュー行列を作る。
 * ViewProjection() で GPU 送信用の合成行列を取得する。
 */
class CCamera {
public:
    /** 単位ビュー・単位プロジェクションで構築する。 */
    CCamera() noexcept = default;

    /**
     * 透視投影行列を設定する (左手系: Z+ が画面奥)。
     *
     * @param fov_y_rad 垂直視野角 (ラジアン)。
     * @param aspect アスペクト比 (幅/高さ)。
     * @param near_z 近クリップ面距離。
     * @param far_z 遠クリップ面距離。
     */
    void SetPerspective(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) noexcept {
        m_Projection = FMat4::PerspectiveFovLH(fov_y_rad, aspect, near_z, far_z);
    }

    /**
     * 正射影投影行列を設定する。
     *
     * @param width ビュー幅。
     * @param height ビュー高さ。
     * @param near_z 近クリップ面距離。
     * @param far_z 遠クリップ面距離。
     */
    void SetOrthographic(f32 width, f32 height, f32 near_z, f32 far_z) noexcept {
        m_Projection = FMat4::OrthoLH(width, height, near_z, far_z);
    }

    /**
     * 注視点を指定してビュー行列を設定する。
     *
     * @param eye カメラ位置。
     * @param target 注視点。
     * @param up 上方向ベクトル (既定は +Y)。
     */
    void SetLookAt(FVec3 eye, FVec3 target, FVec3 up = FVec3::Up()) noexcept {
        m_Eye = eye;
        m_View = FMat4::LookAtLH(eye, target, up);
    }

    /**
     * ビュー行列を返す。
     *
     * @return 現在のビュー行列への const 参照。
     */
    const FMat4& View()           const noexcept { return m_View; }

    /**
     * プロジェクション行列を返す。
     *
     * @return 現在のプロジェクション行列への const 参照。
     */
    const FMat4& Projection()     const noexcept { return m_Projection; }

    /**
     * ビュー × プロジェクションの合成行列を返す。
     *
     * @return GPU 送信用の view-projection 行列。
     */
    FMat4        ViewProjection() const noexcept { return m_View * m_Projection; }

    /**
     * カメラ位置を返す。
     *
     * @return SetLookAt で設定した eye 位置。
     */
    FVec3        Eye()            const noexcept { return m_Eye; }

    /**
     * アスペクト比変更時に透視投影を作り直す (ウィンドウリサイズ用)。
     *
     * @param fov_y_rad 垂直視野角 (ラジアン)。
     * @param aspect 新しいアスペクト比 (幅/高さ)。
     * @param near_z 近クリップ面距離。
     * @param far_z 遠クリップ面距離。
     */
    void UpdateAspect(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) noexcept {
        SetPerspective(fov_y_rad, aspect, near_z, far_z);
    }

private:
    /** ビュー行列。 */
    FMat4 m_View;

    /** プロジェクション行列。 */
    FMat4 m_Projection;

    /** カメラ位置 (SetLookAt で更新)。 */
    FVec3 m_Eye{0, 0, 0};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FCamera = CCamera;

} // namespace acs
