// ACS Math — Quaternion.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "math/Vec.h"
#include "math/Mat.h"

#include <DirectXMath.h>

namespace acs {

struct alignas(16) Quat {
    f32 x, y, z, w;

    constexpr Quat() noexcept : x(0), y(0), z(0), w(1) {}
    constexpr Quat(f32 x_, f32 y_, f32 z_, f32 w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}

    static Quat Identity() noexcept { return Quat(); }

    static Quat AxisAngle(Vec3 axis, f32 rad) noexcept;
    static Quat Euler(f32 pitch_rad, f32 yaw_rad, f32 roll_rad) noexcept;
    static Quat FromMatrix(const Mat4& m) noexcept;
};

namespace quat_detail {
ACS_FORCEINLINE dxm::XMVECTOR Load(const Quat& q) noexcept {
    return dxm::XMLoadFloat4A(reinterpret_cast<const dxm::XMFLOAT4A*>(&q));
}
ACS_FORCEINLINE Quat Store(dxm::XMVECTOR x) noexcept {
    Quat r;
    dxm::XMStoreFloat4A(reinterpret_cast<dxm::XMFLOAT4A*>(&r), x);
    return r;
}
} // namespace quat_detail

inline Quat Quat::AxisAngle(Vec3 axis, f32 rad) noexcept {
    return quat_detail::Store(dxm::XMQuaternionRotationAxis(vec3_detail::Load(axis), rad));
}
inline Quat Quat::Euler(f32 p, f32 y, f32 r) noexcept {
    return quat_detail::Store(dxm::XMQuaternionRotationRollPitchYaw(p, y, r));
}
inline Quat Quat::FromMatrix(const Mat4& m) noexcept {
    return quat_detail::Store(dxm::XMQuaternionRotationMatrix(mat4_detail::Load(m)));
}

inline Quat operator*(Quat a, Quat b) noexcept {
    return quat_detail::Store(dxm::XMQuaternionMultiply(quat_detail::Load(a), quat_detail::Load(b)));
}
inline Quat Conjugate(Quat q) noexcept {
    return quat_detail::Store(dxm::XMQuaternionConjugate(quat_detail::Load(q)));
}
inline Quat Inverse(Quat q) noexcept {
    return quat_detail::Store(dxm::XMQuaternionInverse(quat_detail::Load(q)));
}
inline Quat Normalize(Quat q) noexcept {
    return quat_detail::Store(dxm::XMQuaternionNormalize(quat_detail::Load(q)));
}
inline Quat Slerp(Quat a, Quat b, f32 t) noexcept {
    return quat_detail::Store(dxm::XMQuaternionSlerp(quat_detail::Load(a), quat_detail::Load(b), t));
}

inline Vec3 Rotate(Quat q, Vec3 v) noexcept {
    return vec3_detail::Store(dxm::XMVector3Rotate(vec3_detail::Load(v), quat_detail::Load(q)));
}

inline Mat4 ToMatrix(Quat q) noexcept {
    return mat4_detail::Store(dxm::XMMatrixRotationQuaternion(quat_detail::Load(q)));
}

} // namespace acs
