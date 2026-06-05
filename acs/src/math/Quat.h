// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — FQuat (クォータニオン)
// -----------------------------------------------------------------------------
// 16 バイト整列の FVec4 同様のレイアウト。回転を表現するのに使う。
// 内部は DirectXMath 委譲。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "math/Vec.h"
#include "math/Mat.h"

#include <DirectXMath.h>

namespace acs {

/**
 * 回転を表すクォータニオン (16 バイト整列、(x,y,z,w) レイアウト)。
 *
 * @details
 * 既定は恒等回転 (0,0,0,1)。内部演算は DirectXMath に委譲する。
 */
struct alignas(16) FQuat {
    /** 虚部 x 成分。 */
    f32 x;

    /** 虚部 y 成分。 */
    f32 y;

    /** 虚部 z 成分。 */
    f32 z;

    /** 実部 w 成分。 */
    f32 w;

    /** 恒等回転 (0,0,0,1) として構築する。 */
    constexpr FQuat() noexcept : x(0), y(0), z(0), w(1) {}

    /**
     * 各成分を指定して構築する。
     *
     * @param x_ 虚部 x。
     * @param y_ 虚部 y。
     * @param z_ 虚部 z。
     * @param w_ 実部 w。
     */
    constexpr FQuat(f32 x_, f32 y_, f32 z_, f32 w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}

    /**
     * 恒等回転を返す。
     *
     * @return 単位クォータニオン (0,0,0,1)。
     */
    static FQuat Identity() noexcept { return FQuat(); }

    /**
     * 軸と角度から回転クォータニオンを構築する。
     *
     * @param axis 回転軸 (正規化を想定)。
     * @param rad 回転角 (ラジアン)。
     * @return axis まわりに rad 回転するクォータニオン。
     */
    static FQuat AxisAngle(FVec3 axis, f32 rad) noexcept;

    /**
     * オイラー角から回転クォータニオンを構築する。
     *
     * @param pitch_rad ピッチ (X 軸まわり、ラジアン)。
     * @param yaw_rad ヨー (Y 軸まわり、ラジアン)。
     * @param roll_rad ロール (Z 軸まわり、ラジアン)。
     * @return 合成回転を表すクォータニオン。
     */
    static FQuat Euler(f32 pitch_rad, f32 yaw_rad, f32 roll_rad) noexcept;

    /**
     * 回転行列からクォータニオンを構築する。
     *
     * @param m 回転行列。
     * @return m と等価な回転を表すクォータニオン。
     */
    static FQuat FromMatrix(const FMat4& m) noexcept;
};

/** FQuat と XMVECTOR の相互変換ヘルパ (内部実装用)。 */
namespace quat_detail {

/**
 * FQuat を XMVECTOR にロードする。
 *
 * @param q 入力クォータニオン。
 * @return ロードした XMVECTOR。
 */
ACS_FORCEINLINE dxm::XMVECTOR Load(const FQuat& q) noexcept {
    return dxm::XMLoadFloat4A(reinterpret_cast<const dxm::XMFLOAT4A*>(&q));
}

/**
 * XMVECTOR を FQuat にストアする。
 *
 * @param x ストアする XMVECTOR。
 * @return 変換した FQuat。
 */
ACS_FORCEINLINE FQuat Store(dxm::XMVECTOR x) noexcept {
    FQuat r;
    dxm::XMStoreFloat4A(reinterpret_cast<dxm::XMFLOAT4A*>(&r), x);
    return r;
}
} // namespace quat_detail

/**
 * 軸と角度から回転クォータニオンを構築する (DirectXMath 委譲)。
 *
 * @param axis 回転軸 (正規化を想定)。
 * @param rad 回転角 (ラジアン)。
 * @return axis まわりに rad 回転するクォータニオン。
 */
inline FQuat FQuat::AxisAngle(FVec3 axis, f32 rad) noexcept {
    return quat_detail::Store(dxm::XMQuaternionRotationAxis(vec3_detail::Load(axis), rad));
}

/**
 * オイラー角から回転クォータニオンを構築する (DirectXMath 委譲)。
 *
 * @param p ピッチ (X 軸まわり、ラジアン)。
 * @param y ヨー (Y 軸まわり、ラジアン)。
 * @param r ロール (Z 軸まわり、ラジアン)。
 * @return 合成回転を表すクォータニオン。
 */
inline FQuat FQuat::Euler(f32 p, f32 y, f32 r) noexcept {
    return quat_detail::Store(dxm::XMQuaternionRotationRollPitchYaw(p, y, r));
}

/**
 * 回転行列からクォータニオンを構築する (DirectXMath 委譲)。
 *
 * @param m 回転行列。
 * @return m と等価な回転を表すクォータニオン。
 */
inline FQuat FQuat::FromMatrix(const FMat4& m) noexcept {
    return quat_detail::Store(dxm::XMQuaternionRotationMatrix(mat4_detail::Load(m)));
}

/**
 * クォータニオンを合成する。
 *
 * @details a * b は「先に b、次に a の回転を適用」する合成になる。
 * @param a 後に適用する回転。
 * @param b 先に適用する回転。
 * @return 合成回転 a * b。
 */
inline FQuat operator*(FQuat a, FQuat b) noexcept {
    return quat_detail::Store(dxm::XMQuaternionMultiply(quat_detail::Load(a), quat_detail::Load(b)));
}

/**
 * 共役を返す (単位クォータニオンなら逆元と一致)。
 *
 * @param q 入力クォータニオン。
 * @return 虚部を符号反転したクォータニオン。
 */
inline FQuat Conjugate(FQuat q) noexcept {
    return quat_detail::Store(dxm::XMQuaternionConjugate(quat_detail::Load(q)));
}

/**
 * 逆元を返す。
 *
 * @param q 入力クォータニオン。
 * @return q の逆回転を表すクォータニオン。
 */
inline FQuat Inverse(FQuat q) noexcept {
    return quat_detail::Store(dxm::XMQuaternionInverse(quat_detail::Load(q)));
}

/**
 * 正規化したクォータニオンを返す。
 *
 * @param q 入力クォータニオン。
 * @return 長さ 1 のクォータニオン。
 */
inline FQuat Normalize(FQuat q) noexcept {
    return quat_detail::Store(dxm::XMQuaternionNormalize(quat_detail::Load(q)));
}

/**
 * 2 つのクォータニオンを球面線形補間する。
 *
 * @param a t=0 のときの回転。
 * @param b t=1 のときの回転。
 * @param t 補間係数。
 * @return a と b を最短弧で補間した回転。
 */
inline FQuat Slerp(FQuat a, FQuat b, f32 t) noexcept {
    return quat_detail::Store(dxm::XMQuaternionSlerp(quat_detail::Load(a), quat_detail::Load(b), t));
}

/**
 * ベクトルをクォータニオンで回転する。
 *
 * @param q 回転クォータニオン。
 * @param v 回転対象のベクトル。
 * @return q で回転した v。
 */
inline FVec3 Rotate(FQuat q, FVec3 v) noexcept {
    return vec3_detail::Store(dxm::XMVector3Rotate(vec3_detail::Load(v), quat_detail::Load(q)));
}

/**
 * クォータニオンを回転行列に変換する。
 *
 * @param q 入力クォータニオン。
 * @return q と等価な回転行列。
 */
inline FMat4 ToMatrix(FQuat q) noexcept {
    return mat4_detail::Store(dxm::XMMatrixRotationQuaternion(quat_detail::Load(q)));
}

} // namespace acs
