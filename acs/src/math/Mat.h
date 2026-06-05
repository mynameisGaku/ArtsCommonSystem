// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — FMat4 4x4 行列
// -----------------------------------------------------------------------------
// 行優先 (row-major) レイアウト。XMFLOAT4X4 と互換。
// HLSL 側はデフォルトで列優先なので、シェーダにアップロードする前に
// Transpose() するか、HLSL 側で `#pragma pack_matrix(row_major)` を指定する。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "math/Vec.h"

#include <DirectXMath.h>

namespace acs {

/**
 * 4x4 行列 (行優先、16 バイト整列、XMFLOAT4X4 互換)。
 *
 * @details
 * 行ベクトル規約で、変換は v * M の順に適用する。HLSL は既定で列優先なので
 * シェーダへ送る前に Transpose するか HLSL 側で row_major を指定すること。
 */
struct alignas(16) FMat4 {
    /** 行優先の 4x4 成分 (m[row][col])。 */
    f32 m[4][4];

    /** 単位行列として構築する。 */
    constexpr FMat4() noexcept
        : m{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}} {}

    /**
     * 16 個の成分を行優先順に指定して構築する。
     *
     * @param a00 0 行 0 列。
     * @param a01 0 行 1 列。
     * @param a02 0 行 2 列。
     * @param a03 0 行 3 列。
     * @param a10 1 行 0 列。
     * @param a11 1 行 1 列。
     * @param a12 1 行 2 列。
     * @param a13 1 行 3 列。
     * @param a20 2 行 0 列。
     * @param a21 2 行 1 列。
     * @param a22 2 行 2 列。
     * @param a23 2 行 3 列。
     * @param a30 3 行 0 列。
     * @param a31 3 行 1 列。
     * @param a32 3 行 2 列。
     * @param a33 3 行 3 列。
     */
    constexpr FMat4(f32 a00, f32 a01, f32 a02, f32 a03,
                   f32 a10, f32 a11, f32 a12, f32 a13,
                   f32 a20, f32 a21, f32 a22, f32 a23,
                   f32 a30, f32 a31, f32 a32, f32 a33) noexcept
        : m{{a00,a01,a02,a03},{a10,a11,a12,a13},{a20,a21,a22,a23},{a30,a31,a32,a33}} {}

    /**
     * 単位行列を返す。
     *
     * @return 単位行列。
     */
    static FMat4 Identity()                                                           noexcept { return FMat4(); }

    /**
     * 平行移動行列を返す。
     *
     * @param t 平行移動量。
     * @return t だけ平行移動する行列。
     */
    static FMat4 Translation(FVec3 t)                                                  noexcept;

    /**
     * スケール行列を返す。
     *
     * @param s 各軸のスケール係数。
     * @return s でスケールする行列。
     */
    static FMat4 Scale(FVec3 s)                                                        noexcept;

    /**
     * X 軸まわりの回転行列を返す。
     *
     * @param rad 回転角 (ラジアン)。
     * @return X 軸回転行列。
     */
    static FMat4 RotationX(f32 rad)                                                   noexcept;

    /**
     * Y 軸まわりの回転行列を返す。
     *
     * @param rad 回転角 (ラジアン)。
     * @return Y 軸回転行列。
     */
    static FMat4 RotationY(f32 rad)                                                   noexcept;

    /**
     * Z 軸まわりの回転行列を返す。
     *
     * @param rad 回転角 (ラジアン)。
     * @return Z 軸回転行列。
     */
    static FMat4 RotationZ(f32 rad)                                                   noexcept;

    /**
     * 左手系の透視投影行列を返す。
     *
     * @param fov_y_rad 垂直視野角 (ラジアン)。
     * @param aspect アスペクト比 (幅/高さ)。
     * @param z_near 近クリップ面距離。
     * @param z_far 遠クリップ面距離。
     * @return 左手系の透視投影行列。
     */
    static FMat4 PerspectiveFovLH(f32 fov_y_rad, f32 aspect, f32 z_near, f32 z_far)   noexcept;

    /**
     * 右手系の透視投影行列を返す。
     *
     * @param fov_y_rad 垂直視野角 (ラジアン)。
     * @param aspect アスペクト比 (幅/高さ)。
     * @param z_near 近クリップ面距離。
     * @param z_far 遠クリップ面距離。
     * @return 右手系の透視投影行列。
     */
    static FMat4 PerspectiveFovRH(f32 fov_y_rad, f32 aspect, f32 z_near, f32 z_far)   noexcept;

    /**
     * 左手系の正射影行列を返す。
     *
     * @param width ビュー幅。
     * @param height ビュー高さ。
     * @param z_near 近クリップ面距離。
     * @param z_far 遠クリップ面距離。
     * @return 左手系の正射影行列。
     */
    static FMat4 OrthoLH(f32 width, f32 height, f32 z_near, f32 z_far)                noexcept;

    /**
     * 左手系の注視ビュー行列を返す。
     *
     * @param eye カメラ位置。
     * @param at 注視点。
     * @param up 上方向ベクトル。
     * @return 左手系のビュー行列。
     */
    static FMat4 LookAtLH(FVec3 eye, FVec3 at, FVec3 up)                                 noexcept;

    /**
     * 右手系の注視ビュー行列を返す。
     *
     * @param eye カメラ位置。
     * @param at 注視点。
     * @param up 上方向ベクトル。
     * @return 右手系のビュー行列。
     */
    static FMat4 LookAtRH(FVec3 eye, FVec3 at, FVec3 up)                                 noexcept;
};

/** FMat4 と XMMATRIX の相互変換ヘルパ (内部実装用)。 */
namespace mat4_detail {

/**
 * FMat4 を XMMATRIX にロードする。
 *
 * @param m 入力行列。
 * @return ロードした XMMATRIX。
 */
ACS_FORCEINLINE dxm::XMMATRIX Load(const FMat4& m) noexcept {
    return dxm::XMLoadFloat4x4A(reinterpret_cast<const dxm::XMFLOAT4X4A*>(&m));
}

/**
 * XMMATRIX を FMat4 にストアする。
 *
 * @param x ストアする XMMATRIX。
 * @return 変換した FMat4。
 */
ACS_FORCEINLINE FMat4 Store(dxm::FXMMATRIX x) noexcept {
    FMat4 r;
    dxm::XMStoreFloat4x4A(reinterpret_cast<dxm::XMFLOAT4X4A*>(&r), x);
    return r;
}
} // namespace mat4_detail

/**
 * 平行移動行列を返す (DirectXMath 委譲)。
 *
 * @param t 平行移動量。
 * @return t だけ平行移動する行列。
 */
inline FMat4 FMat4::Translation(FVec3 t) noexcept {
    return mat4_detail::Store(dxm::XMMatrixTranslation(t.x, t.y, t.z));
}

/**
 * スケール行列を返す (DirectXMath 委譲)。
 *
 * @param s 各軸のスケール係数。
 * @return s でスケールする行列。
 */
inline FMat4 FMat4::Scale(FVec3 s) noexcept {
    return mat4_detail::Store(dxm::XMMatrixScaling(s.x, s.y, s.z));
}

/**
 * X 軸回転行列を返す (DirectXMath 委譲)。
 *
 * @param r 回転角 (ラジアン)。
 * @return X 軸回転行列。
 */
inline FMat4 FMat4::RotationX(f32 r) noexcept { return mat4_detail::Store(dxm::XMMatrixRotationX(r)); }

/**
 * Y 軸回転行列を返す (DirectXMath 委譲)。
 *
 * @param r 回転角 (ラジアン)。
 * @return Y 軸回転行列。
 */
inline FMat4 FMat4::RotationY(f32 r) noexcept { return mat4_detail::Store(dxm::XMMatrixRotationY(r)); }

/**
 * Z 軸回転行列を返す (DirectXMath 委譲)。
 *
 * @param r 回転角 (ラジアン)。
 * @return Z 軸回転行列。
 */
inline FMat4 FMat4::RotationZ(f32 r) noexcept { return mat4_detail::Store(dxm::XMMatrixRotationZ(r)); }

/**
 * 左手系の透視投影行列を返す (DirectXMath 委譲)。
 *
 * @param fov 垂直視野角 (ラジアン)。
 * @param aspect アスペクト比 (幅/高さ)。
 * @param zn 近クリップ面距離。
 * @param zf 遠クリップ面距離。
 * @return 左手系の透視投影行列。
 */
inline FMat4 FMat4::PerspectiveFovLH(f32 fov, f32 aspect, f32 zn, f32 zf) noexcept {
    return mat4_detail::Store(dxm::XMMatrixPerspectiveFovLH(fov, aspect, zn, zf));
}

/**
 * 右手系の透視投影行列を返す (DirectXMath 委譲)。
 *
 * @param fov 垂直視野角 (ラジアン)。
 * @param aspect アスペクト比 (幅/高さ)。
 * @param zn 近クリップ面距離。
 * @param zf 遠クリップ面距離。
 * @return 右手系の透視投影行列。
 */
inline FMat4 FMat4::PerspectiveFovRH(f32 fov, f32 aspect, f32 zn, f32 zf) noexcept {
    return mat4_detail::Store(dxm::XMMatrixPerspectiveFovRH(fov, aspect, zn, zf));
}

/**
 * 左手系の正射影行列を返す (DirectXMath 委譲)。
 *
 * @param w ビュー幅。
 * @param h ビュー高さ。
 * @param zn 近クリップ面距離。
 * @param zf 遠クリップ面距離。
 * @return 左手系の正射影行列。
 */
inline FMat4 FMat4::OrthoLH(f32 w, f32 h, f32 zn, f32 zf) noexcept {
    return mat4_detail::Store(dxm::XMMatrixOrthographicLH(w, h, zn, zf));
}

/**
 * 左手系の注視ビュー行列を返す (DirectXMath 委譲)。
 *
 * @param eye カメラ位置。
 * @param at 注視点。
 * @param up 上方向ベクトル。
 * @return 左手系のビュー行列。
 */
inline FMat4 FMat4::LookAtLH(FVec3 eye, FVec3 at, FVec3 up) noexcept {
    return mat4_detail::Store(dxm::XMMatrixLookAtLH(
        vec3_detail::Load(eye), vec3_detail::Load(at), vec3_detail::Load(up)));
}

/**
 * 右手系の注視ビュー行列を返す (DirectXMath 委譲)。
 *
 * @param eye カメラ位置。
 * @param at 注視点。
 * @param up 上方向ベクトル。
 * @return 右手系のビュー行列。
 */
inline FMat4 FMat4::LookAtRH(FVec3 eye, FVec3 at, FVec3 up) noexcept {
    return mat4_detail::Store(dxm::XMMatrixLookAtRH(
        vec3_detail::Load(eye), vec3_detail::Load(at), vec3_detail::Load(up)));
}

/**
 * 行列積を返す。
 *
 * @details 行ベクトル規約のため a * b は「先に a、次に b を適用」する合成になる。
 * @param a 左辺行列。
 * @param b 右辺行列。
 * @return a * b。
 */
inline FMat4 operator*(const FMat4& a, const FMat4& b) noexcept {
    return mat4_detail::Store(dxm::XMMatrixMultiply(mat4_detail::Load(a), mat4_detail::Load(b)));
}

/**
 * 転置行列を返す (HLSL 列優先側へアップロードする際に使う)。
 *
 * @param m 入力行列。
 * @return m の転置。
 */
inline FMat4 Transpose(const FMat4& m) noexcept {
    return mat4_detail::Store(dxm::XMMatrixTranspose(mat4_detail::Load(m)));
}

/**
 * 逆行列を返す (行列式は破棄)。
 *
 * @param m 入力行列。
 * @return m の逆行列 (非可逆なら結果は不定)。
 */
inline FMat4 Inverse(const FMat4& m) noexcept {
    dxm::XMVECTOR det;
    return mat4_detail::Store(dxm::XMMatrixInverse(&det, mat4_detail::Load(m)));
}

/**
 * 4D ベクトルを行列で変換する (点・方向・同次座標すべてに対応)。
 *
 * @param v 入力ベクトル (w 込み)。
 * @param m 変換行列。
 * @return v * m (w 除算なし)。
 */
inline FVec4 Transform(FVec4 v, const FMat4& m) noexcept {
    return vec4_detail::Store(dxm::XMVector4Transform(vec4_detail::Load(v), mat4_detail::Load(m)));
}

/**
 * 点として変換する (w=1 とみなし、最後に w で割る)。
 *
 * @param p 入力点。
 * @param m 変換行列。
 * @return 平行移動を含めて変換した点。
 */
inline FVec3 TransformPoint(FVec3 p, const FMat4& m) noexcept {
    return vec3_detail::Store(dxm::XMVector3TransformCoord(vec3_detail::Load(p), mat4_detail::Load(m)));
}

/**
 * 方向ベクトルとして変換する (w=0、平行移動成分は無視)。
 *
 * @param v 入力方向ベクトル。
 * @param m 変換行列。
 * @return 回転・スケールのみ適用した方向ベクトル。
 */
inline FVec3 TransformVector(FVec3 v, const FMat4& m) noexcept {
    return vec3_detail::Store(dxm::XMVector3TransformNormal(vec3_detail::Load(v), mat4_detail::Load(m)));
}

} // namespace acs
