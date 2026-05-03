// =============================================================================
// ACS Math — Vec2 / Vec3 / Vec4 ベクトル型
// -----------------------------------------------------------------------------
// 格納レイアウトは DirectXMath の XMFLOAT* に一致:
//   Vec2: 8 バイト   (XMFLOAT2 と互換)
//   Vec3: 16 バイト  (Vec4 形状にパディング、SIMD フレンドリー)
//   Vec4: 16 バイト  (XMFLOAT4A と互換、整列必須)
//
// 演算は内部で XMVECTOR (= __m128) に Load → 計算 → Store。
// SSE2/SSE4.1/AVX/AVX2 のどれを使うかは DirectXMath が
// /arch:* フラグから自動選択する（モジュール全体に AVX2 を当てる場合は
// CMake の ACS_MATH_AVX2 オプションを ON）。
//
// 性能注意:
//   1 回ずつの演算では Load/Store のラウンドトリップが発生する。
//   タイトループでは MathDispatch::TransformPoints などの
//   バッチ API を使うこと。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "math/Math.h"

#include <DirectXMath.h>

namespace acs {

namespace dxm = DirectX;

// =============================================================================
// Vec2 — 8B、SIMD 不使用（小さすぎてメリット薄い）
// =============================================================================
struct Vec2 {
    f32 x, y;

    constexpr Vec2() noexcept = default;
    constexpr Vec2(f32 v) noexcept : x(v), y(v) {}
    constexpr Vec2(f32 x_, f32 y_) noexcept : x(x_), y(y_) {}

    static constexpr Vec2 Zero() noexcept { return {0,0}; }
    static constexpr Vec2 One()  noexcept { return {1,1}; }

    Vec2& operator+=(Vec2 o) noexcept { x+=o.x; y+=o.y; return *this; }
    Vec2& operator-=(Vec2 o) noexcept { x-=o.x; y-=o.y; return *this; }
    Vec2& operator*=(f32 s)  noexcept { x*=s; y*=s; return *this; }
    Vec2& operator/=(f32 s)  noexcept { f32 r = 1.0f/s; x*=r; y*=r; return *this; }
};

inline Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x+b.x, a.y+b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x-b.x, a.y-b.y}; }
inline Vec2 operator*(Vec2 v, f32 s)  noexcept { return {v.x*s, v.y*s}; }
inline Vec2 operator*(f32 s, Vec2 v)  noexcept { return v*s; }
inline Vec2 operator-(Vec2 v)         noexcept { return {-v.x, -v.y}; }

inline f32 Dot(Vec2 a, Vec2 b) noexcept { return a.x*b.x + a.y*b.y; }
inline f32 LengthSq(Vec2 v)    noexcept { return Dot(v, v); }
inline f32 Length(Vec2 v)      noexcept { return Sqrt(LengthSq(v)); }
inline Vec2 Normalize(Vec2 v)  noexcept { f32 l = Length(v); return l > 0 ? v * (1.0f/l) : Vec2::Zero(); }

// =============================================================================
// Vec3 — 16B (パッド付き)、内部 SIMD
// -----------------------------------------------------------------------------
// 4 番目の要素 _pad は常に 0。Vec4 と互換のあるレイアウトにすることで
// XMLoadFloat4A が使え、ロード/ストアが高速。
// =============================================================================
struct alignas(16) Vec3 {
    f32 x, y, z, _pad;

    constexpr Vec3() noexcept : x(0), y(0), z(0), _pad(0) {}
    constexpr Vec3(f32 v) noexcept : x(v), y(v), z(v), _pad(0) {}
    constexpr Vec3(f32 x_, f32 y_, f32 z_) noexcept : x(x_), y(y_), z(z_), _pad(0) {}

    // よく使う基底ベクトル
    static constexpr Vec3 Zero()    noexcept { return {0,0,0}; }
    static constexpr Vec3 One()     noexcept { return {1,1,1}; }
    static constexpr Vec3 UnitX()   noexcept { return {1,0,0}; }
    static constexpr Vec3 UnitY()   noexcept { return {0,1,0}; }
    static constexpr Vec3 UnitZ()   noexcept { return {0,0,1}; }
    static constexpr Vec3 Up()      noexcept { return {0,1,0}; }
    static constexpr Vec3 Forward() noexcept { return {0,0,1}; }
    static constexpr Vec3 Right()   noexcept { return {1,0,0}; }

    Vec3& operator+=(Vec3 o) noexcept { x+=o.x; y+=o.y; z+=o.z; return *this; }
    Vec3& operator-=(Vec3 o) noexcept { x-=o.x; y-=o.y; z-=o.z; return *this; }
    Vec3& operator*=(f32 s)  noexcept { x*=s; y*=s; z*=s; return *this; }
};

namespace vec3_detail {
// Vec3 ↔ XMVECTOR 変換（パッド付きなので Float4A をそのまま使える）
ACS_FORCEINLINE dxm::XMVECTOR Load(const Vec3& v) noexcept {
    return dxm::XMLoadFloat4A(reinterpret_cast<const dxm::XMFLOAT4A*>(&v));
}
ACS_FORCEINLINE Vec3 Store(dxm::XMVECTOR x) noexcept {
    Vec3 r;
    dxm::XMStoreFloat4A(reinterpret_cast<dxm::XMFLOAT4A*>(&r), x);
    r._pad = 0;  // ストア後にパッドを 0 に戻す（保守的）
    return r;
}
} // namespace vec3_detail

inline Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return vec3_detail::Store(dxm::XMVectorAdd(vec3_detail::Load(a), vec3_detail::Load(b)));
}
inline Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return vec3_detail::Store(dxm::XMVectorSubtract(vec3_detail::Load(a), vec3_detail::Load(b)));
}
inline Vec3 operator*(Vec3 v, f32 s) noexcept {
    return vec3_detail::Store(dxm::XMVectorScale(vec3_detail::Load(v), s));
}
inline Vec3 operator*(f32 s, Vec3 v) noexcept { return v * s; }
inline Vec3 operator-(Vec3 v)         noexcept {
    return vec3_detail::Store(dxm::XMVectorNegate(vec3_detail::Load(v)));
}

inline f32 Dot(Vec3 a, Vec3 b) noexcept {
    return dxm::XMVectorGetX(dxm::XMVector3Dot(vec3_detail::Load(a), vec3_detail::Load(b)));
}
inline f32 LengthSq(Vec3 v) noexcept {
    return dxm::XMVectorGetX(dxm::XMVector3LengthSq(vec3_detail::Load(v)));
}
inline f32 Length(Vec3 v) noexcept {
    return dxm::XMVectorGetX(dxm::XMVector3Length(vec3_detail::Load(v)));
}
inline Vec3 Normalize(Vec3 v) noexcept {
    return vec3_detail::Store(dxm::XMVector3Normalize(vec3_detail::Load(v)));
}
inline Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return vec3_detail::Store(dxm::XMVector3Cross(vec3_detail::Load(a), vec3_detail::Load(b)));
}
inline Vec3 Lerp(Vec3 a, Vec3 b, f32 t) noexcept {
    return vec3_detail::Store(dxm::XMVectorLerp(vec3_detail::Load(a), vec3_detail::Load(b), t));
}

// =============================================================================
// Vec4 — 16B 整列、SIMD
// =============================================================================
struct alignas(16) Vec4 {
    f32 x, y, z, w;

    constexpr Vec4() noexcept = default;
    constexpr Vec4(f32 v) noexcept : x(v), y(v), z(v), w(v) {}
    constexpr Vec4(f32 x_, f32 y_, f32 z_, f32 w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(Vec3 v, f32 w_) noexcept : x(v.x), y(v.y), z(v.z), w(w_) {}

    static constexpr Vec4 Zero() noexcept { return {0,0,0,0}; }
    static constexpr Vec4 One()  noexcept { return {1,1,1,1}; }
};

namespace vec4_detail {
ACS_FORCEINLINE dxm::XMVECTOR Load(const Vec4& v) noexcept {
    return dxm::XMLoadFloat4A(reinterpret_cast<const dxm::XMFLOAT4A*>(&v));
}
ACS_FORCEINLINE Vec4 Store(dxm::XMVECTOR x) noexcept {
    Vec4 r;
    dxm::XMStoreFloat4A(reinterpret_cast<dxm::XMFLOAT4A*>(&r), x);
    return r;
}
} // namespace vec4_detail

inline Vec4 operator+(Vec4 a, Vec4 b) noexcept {
    return vec4_detail::Store(dxm::XMVectorAdd(vec4_detail::Load(a), vec4_detail::Load(b)));
}
inline Vec4 operator-(Vec4 a, Vec4 b) noexcept {
    return vec4_detail::Store(dxm::XMVectorSubtract(vec4_detail::Load(a), vec4_detail::Load(b)));
}
inline Vec4 operator*(Vec4 v, f32 s) noexcept {
    return vec4_detail::Store(dxm::XMVectorScale(vec4_detail::Load(v), s));
}
inline f32 Dot(Vec4 a, Vec4 b) noexcept {
    return dxm::XMVectorGetX(dxm::XMVector4Dot(vec4_detail::Load(a), vec4_detail::Load(b)));
}
inline Vec4 Normalize(Vec4 v) noexcept {
    return vec4_detail::Store(dxm::XMVector4Normalize(vec4_detail::Load(v)));
}

} // namespace acs
