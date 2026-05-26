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

struct alignas(16) FMat4 {
    f32 m[4][4];

    // デフォルトは単位行列
    constexpr FMat4() noexcept
        : m{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}} {}

    constexpr FMat4(f32 a00, f32 a01, f32 a02, f32 a03,
                   f32 a10, f32 a11, f32 a12, f32 a13,
                   f32 a20, f32 a21, f32 a22, f32 a23,
                   f32 a30, f32 a31, f32 a32, f32 a33) noexcept
        : m{{a00,a01,a02,a03},{a10,a11,a12,a13},{a20,a21,a22,a23},{a30,a31,a32,a33}} {}

    // ---- ファクトリ ----
    static FMat4 Identity()                                                           noexcept { return FMat4(); }
    static FMat4 Translation(FVec3 t)                                                  noexcept;
    static FMat4 Scale(FVec3 s)                                                        noexcept;
    static FMat4 RotationX(f32 rad)                                                   noexcept;
    static FMat4 RotationY(f32 rad)                                                   noexcept;
    static FMat4 RotationZ(f32 rad)                                                   noexcept;
    // 透視投影（左手 / 右手）
    static FMat4 PerspectiveFovLH(f32 fov_y_rad, f32 aspect, f32 z_near, f32 z_far)   noexcept;
    static FMat4 PerspectiveFovRH(f32 fov_y_rad, f32 aspect, f32 z_near, f32 z_far)   noexcept;
    // 正射影
    static FMat4 OrthoLH(f32 width, f32 height, f32 z_near, f32 z_far)                noexcept;
    // ビュー行列（注視点指定）
    static FMat4 LookAtLH(FVec3 eye, FVec3 at, FVec3 up)                                 noexcept;
    static FMat4 LookAtRH(FVec3 eye, FVec3 at, FVec3 up)                                 noexcept;
};

namespace mat4_detail {
// FMat4 ↔ XMMATRIX 変換
ACS_FORCEINLINE dxm::XMMATRIX Load(const FMat4& m) noexcept {
    return dxm::XMLoadFloat4x4A(reinterpret_cast<const dxm::XMFLOAT4X4A*>(&m));
}
ACS_FORCEINLINE FMat4 Store(dxm::FXMMATRIX x) noexcept {
    FMat4 r;
    dxm::XMStoreFloat4x4A(reinterpret_cast<dxm::XMFLOAT4X4A*>(&r), x);
    return r;
}
} // namespace mat4_detail

// ---- ファクトリ実装 (DirectXMath 委譲) ----
inline FMat4 FMat4::Translation(FVec3 t) noexcept {
    return mat4_detail::Store(dxm::XMMatrixTranslation(t.x, t.y, t.z));
}
inline FMat4 FMat4::Scale(FVec3 s) noexcept {
    return mat4_detail::Store(dxm::XMMatrixScaling(s.x, s.y, s.z));
}
inline FMat4 FMat4::RotationX(f32 r) noexcept { return mat4_detail::Store(dxm::XMMatrixRotationX(r)); }
inline FMat4 FMat4::RotationY(f32 r) noexcept { return mat4_detail::Store(dxm::XMMatrixRotationY(r)); }
inline FMat4 FMat4::RotationZ(f32 r) noexcept { return mat4_detail::Store(dxm::XMMatrixRotationZ(r)); }

inline FMat4 FMat4::PerspectiveFovLH(f32 fov, f32 aspect, f32 zn, f32 zf) noexcept {
    return mat4_detail::Store(dxm::XMMatrixPerspectiveFovLH(fov, aspect, zn, zf));
}
inline FMat4 FMat4::PerspectiveFovRH(f32 fov, f32 aspect, f32 zn, f32 zf) noexcept {
    return mat4_detail::Store(dxm::XMMatrixPerspectiveFovRH(fov, aspect, zn, zf));
}
inline FMat4 FMat4::OrthoLH(f32 w, f32 h, f32 zn, f32 zf) noexcept {
    return mat4_detail::Store(dxm::XMMatrixOrthographicLH(w, h, zn, zf));
}
inline FMat4 FMat4::LookAtLH(FVec3 eye, FVec3 at, FVec3 up) noexcept {
    return mat4_detail::Store(dxm::XMMatrixLookAtLH(
        vec3_detail::Load(eye), vec3_detail::Load(at), vec3_detail::Load(up)));
}
inline FMat4 FMat4::LookAtRH(FVec3 eye, FVec3 at, FVec3 up) noexcept {
    return mat4_detail::Store(dxm::XMMatrixLookAtRH(
        vec3_detail::Load(eye), vec3_detail::Load(at), vec3_detail::Load(up)));
}

// ---- 演算 ----
// 行列乗算（A * B = まず A、次に B を適用）
inline FMat4 operator*(const FMat4& a, const FMat4& b) noexcept {
    return mat4_detail::Store(dxm::XMMatrixMultiply(mat4_detail::Load(a), mat4_detail::Load(b)));
}
// 転置（HLSL 列優先側へアップロードする際に使用）
inline FMat4 Transpose(const FMat4& m) noexcept {
    return mat4_detail::Store(dxm::XMMatrixTranspose(mat4_detail::Load(m)));
}
// 逆行列（行列式は破棄）
inline FMat4 Inverse(const FMat4& m) noexcept {
    dxm::XMVECTOR det;
    return mat4_detail::Store(dxm::XMMatrixInverse(&det, mat4_detail::Load(m)));
}

// ---- ベクトル変換 ----
// 任意 4D ベクトルを行列で変換（点・方向・同次座標すべてに対応）
inline FVec4 Transform(FVec4 v, const FMat4& m) noexcept {
    return vec4_detail::Store(dxm::XMVector4Transform(vec4_detail::Load(v), mat4_detail::Load(m)));
}
// 点として変換（w=1 として、最後に w で割る）
inline FVec3 TransformPoint(FVec3 p, const FMat4& m) noexcept {
    return vec3_detail::Store(dxm::XMVector3TransformCoord(vec3_detail::Load(p), mat4_detail::Load(m)));
}
// 方向ベクトルとして変換（w=0、平行移動成分は無視）
inline FVec3 TransformVector(FVec3 v, const FMat4& m) noexcept {
    return vec3_detail::Store(dxm::XMVector3TransformNormal(vec3_detail::Load(v), mat4_detail::Load(m)));
}

} // namespace acs
