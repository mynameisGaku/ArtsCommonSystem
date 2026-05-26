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

struct alignas(16) FQuat {
    f32 x, y, z, w;

    // 単位回転 (恒等 quat: 0,0,0,1)
    constexpr FQuat() noexcept : x(0), y(0), z(0), w(1) {}
    constexpr FQuat(f32 x_, f32 y_, f32 z_, f32 w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}

    static FQuat Identity() noexcept { return FQuat(); }

    // ---- ファクトリ ----
    static FQuat AxisAngle(FVec3 axis, f32 rad) noexcept;             // 軸 + 角度から構築
    static FQuat Euler(f32 pitch_rad, f32 yaw_rad, f32 roll_rad) noexcept;  // オイラー角から
    static FQuat FromMatrix(const FMat4& m) noexcept;                  // 回転行列から
};

namespace quat_detail {
ACS_FORCEINLINE dxm::XMVECTOR Load(const FQuat& q) noexcept {
    return dxm::XMLoadFloat4A(reinterpret_cast<const dxm::XMFLOAT4A*>(&q));
}
ACS_FORCEINLINE FQuat Store(dxm::XMVECTOR x) noexcept {
    FQuat r;
    dxm::XMStoreFloat4A(reinterpret_cast<dxm::XMFLOAT4A*>(&r), x);
    return r;
}
} // namespace quat_detail

// ---- ファクトリ実装 ----
inline FQuat FQuat::AxisAngle(FVec3 axis, f32 rad) noexcept {
    return quat_detail::Store(dxm::XMQuaternionRotationAxis(vec3_detail::Load(axis), rad));
}
inline FQuat FQuat::Euler(f32 p, f32 y, f32 r) noexcept {
    return quat_detail::Store(dxm::XMQuaternionRotationRollPitchYaw(p, y, r));
}
inline FQuat FQuat::FromMatrix(const FMat4& m) noexcept {
    return quat_detail::Store(dxm::XMQuaternionRotationMatrix(mat4_detail::Load(m)));
}

// ---- 演算 ----
// クォータニオン合成（A * B = まず B、次に A の回転を適用）
inline FQuat operator*(FQuat a, FQuat b) noexcept {
    return quat_detail::Store(dxm::XMQuaternionMultiply(quat_detail::Load(a), quat_detail::Load(b)));
}
// 共役（単位 quat なら逆元と同じ）
inline FQuat Conjugate(FQuat q) noexcept {
    return quat_detail::Store(dxm::XMQuaternionConjugate(quat_detail::Load(q)));
}
// 逆元
inline FQuat Inverse(FQuat q) noexcept {
    return quat_detail::Store(dxm::XMQuaternionInverse(quat_detail::Load(q)));
}
// 正規化
inline FQuat Normalize(FQuat q) noexcept {
    return quat_detail::Store(dxm::XMQuaternionNormalize(quat_detail::Load(q)));
}
// 球面線形補間
inline FQuat Slerp(FQuat a, FQuat b, f32 t) noexcept {
    return quat_detail::Store(dxm::XMQuaternionSlerp(quat_detail::Load(a), quat_detail::Load(b), t));
}

// ベクトルを quat で回転
inline FVec3 Rotate(FQuat q, FVec3 v) noexcept {
    return vec3_detail::Store(dxm::XMVector3Rotate(vec3_detail::Load(v), quat_detail::Load(q)));
}

// quat → 回転行列
inline FMat4 ToMatrix(FQuat q) noexcept {
    return mat4_detail::Store(dxm::XMMatrixRotationQuaternion(quat_detail::Load(q)));
}

} // namespace acs
