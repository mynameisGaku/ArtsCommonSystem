// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — Mat4 4x4 行列
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

struct alignas(16) Mat4 {
    f32 m[4][4];

    // デフォルトは単位行列
    constexpr Mat4() noexcept
        : m{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}} {}

    constexpr Mat4(f32 a00, f32 a01, f32 a02, f32 a03,
                   f32 a10, f32 a11, f32 a12, f32 a13,
                   f32 a20, f32 a21, f32 a22, f32 a23,
                   f32 a30, f32 a31, f32 a32, f32 a33) noexcept
        : m{{a00,a01,a02,a03},{a10,a11,a12,a13},{a20,a21,a22,a23},{a30,a31,a32,a33}} {}

    // ---- ファクトリ ----
    static Mat4 Identity()                                                           noexcept { return Mat4(); }
    static Mat4 Translation(Vec3 t)                                                  noexcept;
    static Mat4 Scale(Vec3 s)                                                        noexcept;
    static Mat4 RotationX(f32 rad)                                                   noexcept;
    static Mat4 RotationY(f32 rad)                                                   noexcept;
    static Mat4 RotationZ(f32 rad)                                                   noexcept;
    // 透視投影（左手 / 右手）
    static Mat4 PerspectiveFovLH(f32 fov_y_rad, f32 aspect, f32 z_near, f32 z_far)   noexcept;
    static Mat4 PerspectiveFovRH(f32 fov_y_rad, f32 aspect, f32 z_near, f32 z_far)   noexcept;
    // 正射影
    static Mat4 OrthoLH(f32 width, f32 height, f32 z_near, f32 z_far)                noexcept;
    // ビュー行列（注視点指定）
    static Mat4 LookAtLH(Vec3 eye, Vec3 at, Vec3 up)                                 noexcept;
    static Mat4 LookAtRH(Vec3 eye, Vec3 at, Vec3 up)                                 noexcept;
};

namespace mat4_detail {
// Mat4 ↔ XMMATRIX 変換
ACS_FORCEINLINE dxm::XMMATRIX Load(const Mat4& m) noexcept {
    return dxm::XMLoadFloat4x4A(reinterpret_cast<const dxm::XMFLOAT4X4A*>(&m));
}
ACS_FORCEINLINE Mat4 Store(dxm::FXMMATRIX x) noexcept {
    Mat4 r;
    dxm::XMStoreFloat4x4A(reinterpret_cast<dxm::XMFLOAT4X4A*>(&r), x);
    return r;
}
} // namespace mat4_detail

// ---- ファクトリ実装 (DirectXMath 委譲) ----
inline Mat4 Mat4::Translation(Vec3 t) noexcept {
    return mat4_detail::Store(dxm::XMMatrixTranslation(t.x, t.y, t.z));
}
inline Mat4 Mat4::Scale(Vec3 s) noexcept {
    return mat4_detail::Store(dxm::XMMatrixScaling(s.x, s.y, s.z));
}
inline Mat4 Mat4::RotationX(f32 r) noexcept { return mat4_detail::Store(dxm::XMMatrixRotationX(r)); }
inline Mat4 Mat4::RotationY(f32 r) noexcept { return mat4_detail::Store(dxm::XMMatrixRotationY(r)); }
inline Mat4 Mat4::RotationZ(f32 r) noexcept { return mat4_detail::Store(dxm::XMMatrixRotationZ(r)); }

inline Mat4 Mat4::PerspectiveFovLH(f32 fov, f32 aspect, f32 zn, f32 zf) noexcept {
    return mat4_detail::Store(dxm::XMMatrixPerspectiveFovLH(fov, aspect, zn, zf));
}
inline Mat4 Mat4::PerspectiveFovRH(f32 fov, f32 aspect, f32 zn, f32 zf) noexcept {
    return mat4_detail::Store(dxm::XMMatrixPerspectiveFovRH(fov, aspect, zn, zf));
}
inline Mat4 Mat4::OrthoLH(f32 w, f32 h, f32 zn, f32 zf) noexcept {
    return mat4_detail::Store(dxm::XMMatrixOrthographicLH(w, h, zn, zf));
}
inline Mat4 Mat4::LookAtLH(Vec3 eye, Vec3 at, Vec3 up) noexcept {
    return mat4_detail::Store(dxm::XMMatrixLookAtLH(
        vec3_detail::Load(eye), vec3_detail::Load(at), vec3_detail::Load(up)));
}
inline Mat4 Mat4::LookAtRH(Vec3 eye, Vec3 at, Vec3 up) noexcept {
    return mat4_detail::Store(dxm::XMMatrixLookAtRH(
        vec3_detail::Load(eye), vec3_detail::Load(at), vec3_detail::Load(up)));
}

// ---- 演算 ----
// 行列乗算（A * B = まず A、次に B を適用）
inline Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    return mat4_detail::Store(dxm::XMMatrixMultiply(mat4_detail::Load(a), mat4_detail::Load(b)));
}
// 転置（HLSL 列優先側へアップロードする際に使用）
inline Mat4 Transpose(const Mat4& m) noexcept {
    return mat4_detail::Store(dxm::XMMatrixTranspose(mat4_detail::Load(m)));
}
// 逆行列（行列式は破棄）
inline Mat4 Inverse(const Mat4& m) noexcept {
    dxm::XMVECTOR det;
    return mat4_detail::Store(dxm::XMMatrixInverse(&det, mat4_detail::Load(m)));
}

// ---- ベクトル変換 ----
// 任意 4D ベクトルを行列で変換（点・方向・同次座標すべてに対応）
inline Vec4 Transform(Vec4 v, const Mat4& m) noexcept {
    return vec4_detail::Store(dxm::XMVector4Transform(vec4_detail::Load(v), mat4_detail::Load(m)));
}
// 点として変換（w=1 として、最後に w で割る）
inline Vec3 TransformPoint(Vec3 p, const Mat4& m) noexcept {
    return vec3_detail::Store(dxm::XMVector3TransformCoord(vec3_detail::Load(p), mat4_detail::Load(m)));
}
// 方向ベクトルとして変換（w=0、平行移動成分は無視）
inline Vec3 TransformVector(Vec3 v, const Mat4& m) noexcept {
    return vec3_detail::Store(dxm::XMVector3TransformNormal(vec3_detail::Load(v), mat4_detail::Load(m)));
}

} // namespace acs
