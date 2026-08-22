// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Math.h"

namespace acs {

/**
 * カメラ位置を含まないビュープロジェクション行列を作る。
 *
 * @param view カメラのビュー行列。
 * @param projection 同じカメラの投影行列。
 * @return 平行移動を除いたビューと投影を合成した行列。
 */
inline FMat4 BuildCameraRelativeViewProjection(const FMat4& view, const FMat4& projection) noexcept {
    FMat4 cameraRelativeView = view;
    cameraRelativeView.m[3][0] = 0.0f;
    cameraRelativeView.m[3][1] = 0.0f;
    cameraRelativeView.m[3][2] = 0.0f;
    return cameraRelativeView * projection;
}

/**
 * カメラ位置を含まない逆ビュープロジェクション行列を作る。
 *
 * @param view カメラのビュー行列。
 * @param projection 同じカメラの投影行列。
 * @return 平行移動を除いたビューと投影を合成してから反転した行列。
 */
inline FMat4 BuildCameraRelativeInverseViewProjection(const FMat4& view, const FMat4& projection) noexcept {
    // 回転と投影だけを反転し、遠方座標を含む行列の反転で失われる精度を避ける。
    return Inverse(BuildCameraRelativeViewProjection(view, projection));
}

/**
 * カメラ相対逆行列から画面上の視線方向を復元する。
 *
 * @details 遠クリップ面と近クリップ面の比が大きい透視投影では、遠点の同次座標 w が
 * 0 に丸められて無限遠点になる。この場合も xyz は視線方向を保持するため、w では割らない。
 * 正射影では画面位置によらない Z 方向を逆変換する。
 * @param inverse_view_projection カメラ位置を含まない逆ビュープロジェクション行列。
 * @param ndc_x 画面の X 座標 (-1から1)。
 * @param ndc_y 画面の Y 座標 (-1から1)。
 * @param direction 成功時に書き込む正規化済み視線方向。
 * @return 有効な視線を復元できた場合はtrue。
 */
inline bool TryBuildCameraRelativeViewDirection(
    const FMat4& inverse_view_projection,
    f32 ndc_x,
    f32 ndc_y,
    FVec3& direction) noexcept {
    /** 透視投影では無限遠点を含めて視線方向を保持する同次座標。 */
    const FVec4 far_homogeneous = Transform(
        FVec4{ndc_x, ndc_y, 1.0f, 1.0f}, inverse_view_projection);
    /** 逆投影の Z 行が w を変える場合は透視投影。 */
    const bool perspective = Abs(inverse_view_projection.m[2][3]) > 1.0e-7f;
    /** 正射影では画面位置に依存しない奥行き方向を使う。 */
    const FVec4 orthographic_homogeneous = perspective
        ? FVec4{}
        : Transform(FVec4{0.0f, 0.0f, 1.0f, 0.0f}, inverse_view_projection);
    const FVec3 candidate = perspective
        ? FVec3{far_homogeneous.x, far_homogeneous.y, far_homogeneous.z}
        : FVec3{orthographic_homogeneous.x, orthographic_homogeneous.y,
                orthographic_homogeneous.z};
    /** 非数と無限大は比較結果または長さ上限で拒否する。 */
    const f32 length_squared =
        candidate.x * candidate.x + candidate.y * candidate.y +
        candidate.z * candidate.z;
    constexpr f32 kMaximumFinite = 3.402823466e+38f;
    if (!(length_squared > 1.0e-12f) || length_squared > kMaximumFinite) {
        return false;
    }
    const f32 inverse_length = 1.0f / Sqrt(length_squared);
    const FVec3 resolved{
        candidate.x * inverse_length,
        candidate.y * inverse_length,
        candidate.z * inverse_length};
    if (!(resolved.x == resolved.x) || !(resolved.y == resolved.y) ||
        !(resolved.z == resolved.z) || Abs(resolved.x) > 1.0f ||
        Abs(resolved.y) > 1.0f || Abs(resolved.z) > 1.0f) {
        return false;
    }
    direction = resolved;
    return true;
}

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
     * 注視方向を指定してビュー行列を設定する。
     *
     * @details 遠方で注視点をeyeへ加算すると向きの有効桁が失われるため、既知の方向はこの入口へ直接渡す。
     * @param eye カメラ位置。
     * @param direction カメラから前方への方向。長さは任意。
     * @param up 上方向ベクトル。
     */
    void SetLookDirection(FVec3 eye, FVec3 direction, FVec3 up = FVec3::Up()) noexcept {
        m_Eye = eye;
        // 原点で回転を作ってから平行移動を加え、方向の計算へ遠方座標を混ぜない。
        const FMat4 cameraRelativeView = FMat4::LookAtLH(FVec3{}, direction, up);
        m_View = FMat4::Translation(FVec3{-eye.x, -eye.y, -eye.z}) * cameraRelativeView;
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
     * @return SetLookAtまたはSetLookDirectionで設定したeye位置。
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

    /** SetLookAtまたはSetLookDirectionで更新するカメラ位置。 */
    FVec3 m_Eye{0, 0, 0};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FCamera = CCamera;

} // namespace acs
