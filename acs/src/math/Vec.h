// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Math — FVec2 / FVec3 / FVec4 ベクトル型
// -----------------------------------------------------------------------------
// 格納レイアウトは DirectXMath の XMFLOAT* に一致:
//   FVec2: 8 バイト   (XMFLOAT2 と互換)
//   FVec3: 16 バイト  (FVec4 形状にパディング、SIMD フレンドリー)
//   FVec4: 16 バイト  (XMFLOAT4A と互換、整列必須)
//
// 演算は内部で XMVECTOR (= __m128) に Load → 計算 → Store。
// SSE2/SSE4.1/AVX/AVX2 のどれを使うかは DirectXMath が
// /arch:* フラグから自動選択する（モジュール全体に AVX2 を当てる場合は
// CMake の ACS_MATH_AVX2 オプションを ON）。
//
// 性能注意:
//   1 回ずつの演算では Load/Store のラウンドトリップが発生する。
//   タイトループでは FMathDispatch の TransformPoints などの
//   バッチ API を使うこと。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "math/Math.h"

#include <DirectXMath.h>

namespace acs {

/** DirectXMath 名前空間への短縮エイリアス。 */
namespace dxm = DirectX;

/**
 * 2 成分ベクトル (8 バイト、XMFLOAT2 互換)。
 *
 * @details
 * 小さすぎて SIMD のメリットが薄いため、内部はスカラ演算で実装する。
 */
struct FVec2 {
    /** x 成分。 */
    f32 x;

    /** y 成分。 */
    f32 y;

    /** 各成分を未初期化のまま構築する。 */
    constexpr FVec2() noexcept = default;

    /**
     * 全成分を同じ値で構築する。
     *
     * @param v 全成分に設定する値。
     */
    constexpr FVec2(f32 v) noexcept : x(v), y(v) {}

    /**
     * 各成分を指定して構築する。
     *
     * @param x_ x 成分。
     * @param y_ y 成分。
     */
    constexpr FVec2(f32 x_, f32 y_) noexcept : x(x_), y(y_) {}

    /**
     * 零ベクトルを返す。
     *
     * @return (0, 0)。
     */
    static constexpr FVec2 Zero() noexcept { return {0,0}; }

    /**
     * 全成分が 1 のベクトルを返す。
     *
     * @return (1, 1)。
     */
    static constexpr FVec2 One()  noexcept { return {1,1}; }

    /**
     * 成分ごとに加算する。
     *
     * @param o 加えるベクトル。
     * @return 加算後の自身への参照。
     */
    FVec2& operator+=(FVec2 o) noexcept { x+=o.x; y+=o.y; return *this; }

    /**
     * 成分ごとに減算する。
     *
     * @param o 引くベクトル。
     * @return 減算後の自身への参照。
     */
    FVec2& operator-=(FVec2 o) noexcept { x-=o.x; y-=o.y; return *this; }

    /**
     * スカラ倍する。
     *
     * @param s 乗ずるスカラ。
     * @return スケール後の自身への参照。
     */
    FVec2& operator*=(f32 s)  noexcept { x*=s; y*=s; return *this; }

    /**
     * スカラで除算する。
     *
     * @param s 除するスカラ (逆数を 1 回だけ計算して乗ずる)。
     * @return 除算後の自身への参照。
     */
    FVec2& operator/=(f32 s)  noexcept { const f32 r = 1.0f/s; x*=r; y*=r; return *this; }
};

/**
 * 2 つの FVec2 が全成分一致するかを返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return 全成分が等しければ true。
 */
inline bool operator==(FVec2 a, FVec2 b) noexcept { return a.x == b.x && a.y == b.y; }

/**
 * 2 つの FVec2 が一致しないかを返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return いずれかの成分が異なれば true。
 */
inline bool operator!=(FVec2 a, FVec2 b) noexcept { return !(a == b); }

/**
 * 成分ごとの和を返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a + b。
 */
inline FVec2 operator+(FVec2 a, FVec2 b) noexcept { return {a.x+b.x, a.y+b.y}; }

/**
 * 成分ごとの差を返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a - b。
 */
inline FVec2 operator-(FVec2 a, FVec2 b) noexcept { return {a.x-b.x, a.y-b.y}; }

/**
 * ベクトルのスカラ倍を返す。
 *
 * @param v ベクトル。
 * @param s スカラ。
 * @return v * s。
 */
inline FVec2 operator*(FVec2 v, f32 s)  noexcept { return {v.x*s, v.y*s}; }

/**
 * スカラとベクトルの積を返す。
 *
 * @param s スカラ。
 * @param v ベクトル。
 * @return s * v。
 */
inline FVec2 operator*(f32 s, FVec2 v)  noexcept { return v*s; }

/**
 * 符号反転したベクトルを返す。
 *
 * @param v 入力ベクトル。
 * @return -v。
 */
inline FVec2 operator-(FVec2 v)         noexcept { return {-v.x, -v.y}; }

/**
 * 内積を返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a・b (= a.x*b.x + a.y*b.y)。
 */
inline f32 Dot(FVec2 a, FVec2 b) noexcept { return a.x*b.x + a.y*b.y; }

/**
 * 長さの 2 乗を返す (平方根を避けたい比較用)。
 *
 * @param v 入力ベクトル。
 * @return |v|^2。
 */
inline f32 LengthSq(FVec2 v)    noexcept { return Dot(v, v); }

/**
 * 長さ (ノルム) を返す。
 *
 * @param v 入力ベクトル。
 * @return |v|。
 */
inline f32 Length(FVec2 v)      noexcept { return Sqrt(LengthSq(v)); }

/**
 * 正規化したベクトルを返す。
 *
 * @param v 入力ベクトル。
 * @return 長さ 1 のベクトル。長さ 0 のときは零ベクトル。
 */
inline FVec2 Normalize(FVec2 v)  noexcept { const f32 l = Length(v); return l > 0 ? v * (1.0f/l) : FVec2::Zero(); }

/**
 * 3 成分ベクトル (16 バイト、パッド付き、内部 SIMD)。
 *
 * @details
 * 4 番目の要素 m_Pad は常に 0。FVec4 と互換のレイアウトにすることで
 * XMLoadFloat4A が使え、ロード/ストアが高速になる。
 */
struct alignas(16) FVec3 {
    /** x 成分。 */
    f32 x;

    /** y 成分。 */
    f32 y;

    /** z 成分。 */
    f32 z;

    /** SIMD ロード整列用のパディング (常に 0)。 */
    f32 m_Pad;

    /** 零ベクトルとして構築する (パッドも 0)。 */
    constexpr FVec3() noexcept : x(0), y(0), z(0), m_Pad(0) {}

    /**
     * x/y/z を同じ値で構築する。
     *
     * @param v x/y/z に設定する値。
     */
    constexpr FVec3(f32 v) noexcept : x(v), y(v), z(v), m_Pad(0) {}

    /**
     * 各成分を指定して構築する。
     *
     * @param x_ x 成分。
     * @param y_ y 成分。
     * @param z_ z 成分。
     */
    constexpr FVec3(f32 x_, f32 y_, f32 z_) noexcept : x(x_), y(y_), z(z_), m_Pad(0) {}

    /**
     * 零ベクトルを返す。
     *
     * @return (0, 0, 0)。
     */
    static constexpr FVec3 Zero()    noexcept { return {0,0,0}; }

    /**
     * 全成分が 1 のベクトルを返す。
     *
     * @return (1, 1, 1)。
     */
    static constexpr FVec3 One()     noexcept { return {1,1,1}; }

    /**
     * X 軸の単位ベクトルを返す。
     *
     * @return (1, 0, 0)。
     */
    static constexpr FVec3 UnitX()   noexcept { return {1,0,0}; }

    /**
     * Y 軸の単位ベクトルを返す。
     *
     * @return (0, 1, 0)。
     */
    static constexpr FVec3 UnitY()   noexcept { return {0,1,0}; }

    /**
     * Z 軸の単位ベクトルを返す。
     *
     * @return (0, 0, 1)。
     */
    static constexpr FVec3 UnitZ()   noexcept { return {0,0,1}; }

    /**
     * 上方向 (+Y) の単位ベクトルを返す。
     *
     * @return (0, 1, 0)。
     */
    static constexpr FVec3 Up()      noexcept { return {0,1,0}; }

    /**
     * 前方向 (+Z、左手系) の単位ベクトルを返す。
     *
     * @return (0, 0, 1)。
     */
    static constexpr FVec3 Forward() noexcept { return {0,0,1}; }

    /**
     * 右方向 (+X) の単位ベクトルを返す。
     *
     * @return (1, 0, 0)。
     */
    static constexpr FVec3 Right()   noexcept { return {1,0,0}; }

    /**
     * 成分ごとに加算する。
     *
     * @param o 加えるベクトル。
     * @return 加算後の自身への参照。
     */
    FVec3& operator+=(FVec3 o) noexcept { x+=o.x; y+=o.y; z+=o.z; return *this; }

    /**
     * 成分ごとに減算する。
     *
     * @param o 引くベクトル。
     * @return 減算後の自身への参照。
     */
    FVec3& operator-=(FVec3 o) noexcept { x-=o.x; y-=o.y; z-=o.z; return *this; }

    /**
     * スカラ倍する。
     *
     * @param s 乗ずるスカラ。
     * @return スケール後の自身への参照。
     */
    FVec3& operator*=(f32 s)  noexcept { x*=s; y*=s; z*=s; return *this; }
};

/** FVec3 と XMVECTOR の相互変換ヘルパ (内部実装用)。 */
namespace vec3_detail {

/**
 * FVec3 を XMVECTOR にロードする (パッド付きなので Float4A をそのまま使える)。
 *
 * @param v 入力ベクトル。
 * @return ロードした XMVECTOR。
 */
ACS_FORCEINLINE dxm::XMVECTOR Load(const FVec3& v) noexcept {
    return dxm::XMLoadFloat4A(reinterpret_cast<const dxm::XMFLOAT4A*>(&v));
}

/**
 * XMVECTOR を FVec3 にストアする。
 *
 * @param x ストアする XMVECTOR。
 * @return 変換した FVec3 (パッドは 0 に戻す)。
 */
ACS_FORCEINLINE FVec3 Store(dxm::XMVECTOR x) noexcept {
    FVec3 r;
    dxm::XMStoreFloat4A(reinterpret_cast<dxm::XMFLOAT4A*>(&r), x);
    r.m_Pad = 0;  // ストア後にパッドを 0 に戻す（保守的）
    return r;
}
} // namespace vec3_detail

/**
 * 2 つの FVec3 が全成分一致するかを返す (パッドは比較しない)。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return x/y/z すべて等しければ true。
 */
inline bool operator==(FVec3 a, FVec3 b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/**
 * 2 つの FVec3 が一致しないかを返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return いずれかの成分が異なれば true。
 */
inline bool operator!=(FVec3 a, FVec3 b) noexcept { return !(a == b); }

/**
 * 成分ごとの和を返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a + b。
 */
inline FVec3 operator+(FVec3 a, FVec3 b) noexcept {
    return vec3_detail::Store(dxm::XMVectorAdd(vec3_detail::Load(a), vec3_detail::Load(b)));
}

/**
 * 成分ごとの差を返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a - b。
 */
inline FVec3 operator-(FVec3 a, FVec3 b) noexcept {
    return vec3_detail::Store(dxm::XMVectorSubtract(vec3_detail::Load(a), vec3_detail::Load(b)));
}

/**
 * ベクトルのスカラ倍を返す。
 *
 * @param v ベクトル。
 * @param s スカラ。
 * @return v * s。
 */
inline FVec3 operator*(FVec3 v, f32 s) noexcept {
    return vec3_detail::Store(dxm::XMVectorScale(vec3_detail::Load(v), s));
}

/**
 * スカラとベクトルの積を返す。
 *
 * @param s スカラ。
 * @param v ベクトル。
 * @return s * v。
 */
inline FVec3 operator*(f32 s, FVec3 v) noexcept { return v * s; }

/**
 * 符号反転したベクトルを返す。
 *
 * @param v 入力ベクトル。
 * @return -v。
 */
inline FVec3 operator-(FVec3 v)         noexcept {
    return vec3_detail::Store(dxm::XMVectorNegate(vec3_detail::Load(v)));
}

/**
 * 内積を返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a・b。
 */
inline f32 Dot(FVec3 a, FVec3 b) noexcept {
    return dxm::XMVectorGetX(dxm::XMVector3Dot(vec3_detail::Load(a), vec3_detail::Load(b)));
}

/**
 * 長さの 2 乗を返す (平方根を避けたい比較用)。
 *
 * @param v 入力ベクトル。
 * @return |v|^2。
 */
inline f32 LengthSq(FVec3 v) noexcept {
    return dxm::XMVectorGetX(dxm::XMVector3LengthSq(vec3_detail::Load(v)));
}

/**
 * 長さ (ノルム) を返す。
 *
 * @param v 入力ベクトル。
 * @return |v|。
 */
inline f32 Length(FVec3 v) noexcept {
    return dxm::XMVectorGetX(dxm::XMVector3Length(vec3_detail::Load(v)));
}

/**
 * 正規化したベクトルを返す。
 *
 * @param v 入力ベクトル。
 * @return 長さ 1 のベクトル。
 */
inline FVec3 Normalize(FVec3 v) noexcept {
    return vec3_detail::Store(dxm::XMVector3Normalize(vec3_detail::Load(v)));
}

/**
 * 外積を返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a × b (a と b に直交するベクトル)。
 */
inline FVec3 Cross(FVec3 a, FVec3 b) noexcept {
    return vec3_detail::Store(dxm::XMVector3Cross(vec3_detail::Load(a), vec3_detail::Load(b)));
}

/**
 * 2 ベクトルを線形補間する。
 *
 * @param a t=0 のときのベクトル。
 * @param b t=1 のときのベクトル。
 * @param t 補間係数。
 * @return a + (b - a) * t を成分ごとに適用したベクトル。
 */
inline FVec3 Lerp(FVec3 a, FVec3 b, f32 t) noexcept {
    return vec3_detail::Store(dxm::XMVectorLerp(vec3_detail::Load(a), vec3_detail::Load(b), t));
}

/**
 * 4 成分ベクトル (16 バイト整列、内部 SIMD、XMFLOAT4A 互換)。
 */
struct alignas(16) FVec4 {
    /** x 成分。 */
    f32 x;

    /** y 成分。 */
    f32 y;

    /** z 成分。 */
    f32 z;

    /** w 成分 (同次座標)。 */
    f32 w;

    /** 各成分を未初期化のまま構築する。 */
    constexpr FVec4() noexcept = default;

    /**
     * 全成分を同じ値で構築する。
     *
     * @param v 全成分に設定する値。
     */
    constexpr FVec4(f32 v) noexcept : x(v), y(v), z(v), w(v) {}

    /**
     * 各成分を指定して構築する。
     *
     * @param x_ x 成分。
     * @param y_ y 成分。
     * @param z_ z 成分。
     * @param w_ w 成分。
     */
    constexpr FVec4(f32 x_, f32 y_, f32 z_, f32 w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}

    /**
     * FVec3 と w から構築する。
     *
     * @param v xyz に展開する 3 成分ベクトル。
     * @param w_ w 成分。
     */
    constexpr FVec4(FVec3 v, f32 w_) noexcept : x(v.x), y(v.y), z(v.z), w(w_) {}

    /**
     * 零ベクトルを返す。
     *
     * @return (0, 0, 0, 0)。
     */
    static constexpr FVec4 Zero() noexcept { return {0,0,0,0}; }

    /**
     * 全成分が 1 のベクトルを返す。
     *
     * @return (1, 1, 1, 1)。
     */
    static constexpr FVec4 One()  noexcept { return {1,1,1,1}; }
};

/** FVec4 と XMVECTOR の相互変換ヘルパ (内部実装用)。 */
namespace vec4_detail {

/**
 * FVec4 を XMVECTOR にロードする。
 *
 * @param v 入力ベクトル。
 * @return ロードした XMVECTOR。
 */
ACS_FORCEINLINE dxm::XMVECTOR Load(const FVec4& v) noexcept {
    return dxm::XMLoadFloat4A(reinterpret_cast<const dxm::XMFLOAT4A*>(&v));
}

/**
 * XMVECTOR を FVec4 にストアする。
 *
 * @param x ストアする XMVECTOR。
 * @return 変換した FVec4。
 */
ACS_FORCEINLINE FVec4 Store(dxm::XMVECTOR x) noexcept {
    FVec4 r;
    dxm::XMStoreFloat4A(reinterpret_cast<dxm::XMFLOAT4A*>(&r), x);
    return r;
}
} // namespace vec4_detail

/**
 * 2 つの FVec4 が全成分一致するかを返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return 全成分が等しければ true。
 */
inline bool operator==(FVec4 a, FVec4 b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/**
 * 2 つの FVec4 が一致しないかを返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return いずれかの成分が異なれば true。
 */
inline bool operator!=(FVec4 a, FVec4 b) noexcept { return !(a == b); }

/**
 * 成分ごとの和を返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a + b。
 */
inline FVec4 operator+(FVec4 a, FVec4 b) noexcept {
    return vec4_detail::Store(dxm::XMVectorAdd(vec4_detail::Load(a), vec4_detail::Load(b)));
}

/**
 * 成分ごとの差を返す。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a - b。
 */
inline FVec4 operator-(FVec4 a, FVec4 b) noexcept {
    return vec4_detail::Store(dxm::XMVectorSubtract(vec4_detail::Load(a), vec4_detail::Load(b)));
}

/**
 * ベクトルのスカラ倍を返す。
 *
 * @param v ベクトル。
 * @param s スカラ。
 * @return v * s。
 */
inline FVec4 operator*(FVec4 v, f32 s) noexcept {
    return vec4_detail::Store(dxm::XMVectorScale(vec4_detail::Load(v), s));
}

/**
 * 内積を返す (全 4 成分)。
 *
 * @param a 左辺。
 * @param b 右辺。
 * @return a・b。
 */
inline f32 Dot(FVec4 a, FVec4 b) noexcept {
    return dxm::XMVectorGetX(dxm::XMVector4Dot(vec4_detail::Load(a), vec4_detail::Load(b)));
}

/**
 * 正規化したベクトルを返す (全 4 成分)。
 *
 * @param v 入力ベクトル。
 * @return 長さ 1 のベクトル。
 */
inline FVec4 Normalize(FVec4 v) noexcept {
    return vec4_detail::Store(dxm::XMVector4Normalize(vec4_detail::Load(v)));
}

} // namespace acs
