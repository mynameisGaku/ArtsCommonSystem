// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"

namespace acs {

/**
 * 軸並行境界ボックス (中心 + 半サイズで表す)。
 */
struct FAabb3 {
    /** ボックス中心。 */
    FVec3 center;

    /** 半サイズ (各軸の半分の幅)。 */
    FVec3 half_size;

    /** 中心・半サイズとも未初期化のまま構築する。 */
    constexpr FAabb3() noexcept = default;

    /**
     * 中心と半サイズを指定して構築する。
     *
     * @param c 中心。
     * @param hs 半サイズ。
     */
    constexpr FAabb3(FVec3 c, FVec3 hs) noexcept : center(c), half_size(hs) {}

    /**
     * 最小・最大座標から構築する。
     *
     * @param min 最小座標。
     * @param max 最大座標。
     * @return min〜max を覆う AABB。
     */
    static constexpr FAabb3 FromMinMax(FVec3 min, FVec3 max) noexcept {
        const FVec3 c{ (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f };
        const FVec3 hs{ (max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f, (max.z - min.z) * 0.5f };
        return { c, hs };
    }

    /**
     * 中心とエクステント (半サイズ) から構築する。
     *
     * @param center 中心。
     * @param extents 半サイズ。
     * @return 指定の中心・半サイズを持つ AABB。
     */
    static constexpr FAabb3 FromCenterExtents(FVec3 center, FVec3 extents) noexcept {
        return { center, extents };
    }

    /**
     * 最小座標を返す。
     *
     * @return (center - half_size)。
     */
    constexpr FVec3 Min() const noexcept { return { center.x - half_size.x, center.y - half_size.y, center.z - half_size.z }; }

    /**
     * 最大座標を返す。
     *
     * @return (center + half_size)。
     */
    constexpr FVec3 Max() const noexcept { return { center.x + half_size.x, center.y + half_size.y, center.z + half_size.z }; }
};

/**
 * 球 (中心 + 半径)。
 */
struct FSphere {
    /** 球の中心。 */
    FVec3 center;

    /** 半径。 */
    f32  radius = 0.0f;
};

/**
 * 平面 (法線 + 原点からの符号付き距離)。
 *
 * @details
 * 平面式は dot(normal, p) + d = 0。normal は正規化されている前提。
 */
struct FPlane {
    /** 平面の法線 (正規化必須)。 */
    FVec3 normal;

    /** 平面式の定数項 d (原点からの符号付き距離)。 */
    f32  d = 0.0f;

    /**
     * 平面上の点と法線から平面を構築する。
     *
     * @param p 平面上の 1 点。
     * @param n 平面の法線 (正規化を想定)。
     * @return p を通り n を法線とする平面。
     */
    static FPlane FromPointNormal(FVec3 p, FVec3 n) noexcept {
        FPlane pl;
        pl.normal = n;
        pl.d = -(n.x * p.x + n.y * p.y + n.z * p.z);
        return pl;
    }
};

/**
 * レイ (始点 + 方向)。
 */
struct FRay3 {
    /** レイの始点。 */
    FVec3 origin;

    /** レイの方向。 */
    FVec3 direction;
};

/**
 * レイキャストの結果。
 */
struct FRayHit3 {
    /** 命中したか。 */
    bool hit = false;

    /** 命中点の媒介変数 (origin + direction * t が命中点)。 */
    f32  t   = 0.0f;

    /** 命中点座標。 */
    FVec3 point;

    /** 命中面の法線。 */
    FVec3 normal;
};

/**
 * 点が AABB に含まれるかを返す。
 *
 * @param a 対象 AABB。
 * @param p 判定する点。
 * @return p が a の内部 (境界含む) なら true。
 */
ACS_FORCEINLINE bool Contains(const FAabb3& a, FVec3 p) noexcept {
    return Abs(p.x - a.center.x) <= a.half_size.x &&
           Abs(p.y - a.center.y) <= a.half_size.y &&
           Abs(p.z - a.center.z) <= a.half_size.z;
}

/**
 * 点が球に含まれるかを返す。
 *
 * @param s 対象の球。
 * @param p 判定する点。
 * @return p が s の内部 (境界含む) なら true。
 */
ACS_FORCEINLINE bool Contains(const FSphere& s, FVec3 p) noexcept {
    const f32 dx = p.x - s.center.x;
    const f32 dy = p.y - s.center.y;
    const f32 dz = p.z - s.center.z;
    return dx*dx + dy*dy + dz*dz <= s.radius * s.radius;
}

/**
 * 2 つの AABB が重なるかを返す。
 *
 * @param a AABB その 1。
 * @param b AABB その 2。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FAabb3& a, const FAabb3& b) noexcept {
    return Abs(a.center.x - b.center.x) <= (a.half_size.x + b.half_size.x) &&
           Abs(a.center.y - b.center.y) <= (a.half_size.y + b.half_size.y) &&
           Abs(a.center.z - b.center.z) <= (a.half_size.z + b.half_size.z);
}

/**
 * 2 つの球が重なるかを返す。
 *
 * @param a 球その 1。
 * @param b 球その 2。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FSphere& a, const FSphere& b) noexcept {
    const f32 dx = a.center.x - b.center.x;
    const f32 dy = a.center.y - b.center.y;
    const f32 dz = a.center.z - b.center.z;
    const f32 r  = a.radius + b.radius;
    return dx*dx + dy*dy + dz*dz <= r * r;
}

/**
 * AABB と球が重なるかを返す。
 *
 * @details AABB 上の最近傍点が球内にあるかで判定する。
 * @param a 対象 AABB。
 * @param s 対象の球。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FAabb3& a, const FSphere& s) noexcept {
    const FVec3 mn = a.Min(), mx = a.Max();
    const f32 cx = s.center.x < mn.x ? mn.x : (s.center.x > mx.x ? mx.x : s.center.x);
    const f32 cy = s.center.y < mn.y ? mn.y : (s.center.y > mx.y ? mx.y : s.center.y);
    const f32 cz = s.center.z < mn.z ? mn.z : (s.center.z > mx.z ? mx.z : s.center.z);
    const f32 dx = s.center.x - cx;
    const f32 dy = s.center.y - cy;
    const f32 dz = s.center.z - cz;
    return dx*dx + dy*dy + dz*dz <= s.radius * s.radius;
}

/**
 * 球と AABB が重なるかを返す (引数順を入れ替えた利便オーバーロード)。
 *
 * @param s 対象の球。
 * @param a 対象 AABB。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FSphere& s, const FAabb3& a) noexcept { return Intersect(a, s); }

/**
 * 2 つの球の押し出しベクトル (a を b から離す最小ベクトル) を求める。
 *
 * @param a 押し出す対象の球。
 * @param b 押し出しの基準となる球。
 * @param push a を動かすべき方向 × 距離 (出力)。同心の場合は +X 方向に押す。
 * @return 衝突していたら true。
 */
ACS_FORCEINLINE bool Resolve(const FSphere& a, const FSphere& b, FVec3& push) noexcept {
    const f32 dx = a.center.x - b.center.x;
    const f32 dy = a.center.y - b.center.y;
    const f32 dz = a.center.z - b.center.z;
    const f32 d2 = dx*dx + dy*dy + dz*dz;
    const f32 r  = a.radius + b.radius;
    if (d2 >= r * r) return false;
    if (d2 < 1e-8f) { push = { r, 0, 0 }; return true; }
    const f32 d = Sqrt(d2);
    const f32 overlap = r - d;
    push = { (dx / d) * overlap, (dy / d) * overlap, (dz / d) * overlap };
    return true;
}

/**
 * AABB と無限長レイの交差を求める (slab method)。
 *
 * @param ray 入力レイ。
 * @param a 対象 AABB。
 * @param t_max 探索する t の上限 (既定は実質無限大)。
 * @return 命中情報。命中時は t・point・命中軸の法線が入る。
 */
ACS_FORCEINLINE FRayHit3 RaycastAabb(const FRay3& ray, const FAabb3& a,
                                    f32 t_max = 3.4028235e38f) noexcept {
    FRayHit3 r{};
    const FVec3 mn = a.Min(), mx = a.Max();
    const f32 inv_dx = ray.direction.x != 0.0f ? 1.0f / ray.direction.x : 1e30f;
    const f32 inv_dy = ray.direction.y != 0.0f ? 1.0f / ray.direction.y : 1e30f;
    const f32 inv_dz = ray.direction.z != 0.0f ? 1.0f / ray.direction.z : 1e30f;

    const f32 t1 = (mn.x - ray.origin.x) * inv_dx;
    const f32 t2 = (mx.x - ray.origin.x) * inv_dx;
    const f32 t3 = (mn.y - ray.origin.y) * inv_dy;
    const f32 t4 = (mx.y - ray.origin.y) * inv_dy;
    const f32 t5 = (mn.z - ray.origin.z) * inv_dz;
    const f32 t6 = (mx.z - ray.origin.z) * inv_dz;

    const f32 tmin_x = t1 < t2 ? t1 : t2;
    const f32 tmax_x = t1 > t2 ? t1 : t2;
    const f32 tmin_y = t3 < t4 ? t3 : t4;
    const f32 tmax_y = t3 > t4 ? t3 : t4;
    const f32 tmin_z = t5 < t6 ? t5 : t6;
    const f32 tmax_z = t5 > t6 ? t5 : t6;

    f32 tmin = tmin_x > tmin_y ? tmin_x : tmin_y;
    if (tmin_z > tmin) tmin = tmin_z;
    f32 tmax = tmax_x < tmax_y ? tmax_x : tmax_y;
    if (tmax_z < tmax) tmax = tmax_z;

    if (tmax < 0 || tmin > tmax || tmin > t_max) return r;
    r.hit = true;
    r.t = tmin < 0 ? 0 : tmin;
    r.point = { ray.origin.x + ray.direction.x * r.t,
                ray.origin.y + ray.direction.y * r.t,
                ray.origin.z + ray.direction.z * r.t };
    // 法線: hit した軸を判定
    if (tmin == tmin_x)      r.normal = { ray.direction.x < 0 ? 1.0f : -1.0f, 0, 0 };
    else if (tmin == tmin_y) r.normal = { 0, ray.direction.y < 0 ? 1.0f : -1.0f, 0 };
    else                     r.normal = { 0, 0, ray.direction.z < 0 ? 1.0f : -1.0f };
    return r;
}

/**
 * 球とレイの交差を求める (二次方程式の最近接解)。
 *
 * @param ray 入力レイ。
 * @param s 対象の球。
 * @param t_max 探索する t の上限 (既定は実質無限大)。
 * @return 命中情報。命中時は外向き法線が入る。
 */
ACS_FORCEINLINE FRayHit3 RaycastSphere(const FRay3& ray, const FSphere& s,
                                      f32 t_max = 3.4028235e38f) noexcept {
    FRayHit3 r{};
    const f32 ox = ray.origin.x - s.center.x;
    const f32 oy = ray.origin.y - s.center.y;
    const f32 oz = ray.origin.z - s.center.z;
    const f32 dx = ray.direction.x;
    const f32 dy = ray.direction.y;
    const f32 dz = ray.direction.z;
    const f32 a = dx*dx + dy*dy + dz*dz;
    if (a < 1e-12f) return r;
    const f32 b = 2.0f * (ox*dx + oy*dy + oz*dz);
    const f32 c = ox*ox + oy*oy + oz*oz - s.radius * s.radius;
    const f32 disc = b*b - 4.0f*a*c;
    if (disc < 0) return r;
    const f32 sd = Sqrt(disc);
    const f32 t = (-b - sd) / (2.0f * a);
    if (t < 0 || t > t_max) return r;
    r.hit = true;
    r.t = t;
    r.point = { ray.origin.x + dx*t, ray.origin.y + dy*t, ray.origin.z + dz*t };
    const f32 inv_r = 1.0f / s.radius;
    r.normal = { (r.point.x - s.center.x) * inv_r,
                 (r.point.y - s.center.y) * inv_r,
                 (r.point.z - s.center.z) * inv_r };
    return r;
}

/**
 * 三角形 (両面) とレイの交差を求める (Möller–Trumbore)。
 *
 * @details 法線は常にレイと逆向き (レイ側を向く面法線) を返す。
 * @param ray 入力レイ。
 * @param v0 三角形頂点 0。
 * @param v1 三角形頂点 1。
 * @param v2 三角形頂点 2。
 * @param t_max 探索する t の上限 (既定は実質無限大)。
 * @return 命中情報。
 */
ACS_FORCEINLINE FRayHit3 RaycastTriangle(const FRay3& ray, FVec3 v0, FVec3 v1, FVec3 v2,
                                        f32 t_max = 3.4028235e38f) noexcept {
    FRayHit3 r{};
    const FVec3 e1 = v1 - v0;
    const FVec3 e2 = v2 - v0;
    const FVec3 pv = Cross(ray.direction, e2);
    const f32   det = Dot(e1, pv);
    if (det > -1e-8f && det < 1e-8f) return r;       // レイと三角形が平行
    const f32 inv_det = 1.0f / det;
    const FVec3 tv = ray.origin - v0;
    const f32 u = Dot(tv, pv) * inv_det;
    if (u < 0.0f || u > 1.0f) return r;
    const FVec3 qv = Cross(tv, e1);
    const f32 v = Dot(ray.direction, qv) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return r;
    const f32 t = Dot(e2, qv) * inv_det;
    if (t < 1e-6f || t > t_max) return r;
    r.hit   = true;
    r.t     = t;
    r.point = { ray.origin.x + ray.direction.x * t,
                ray.origin.y + ray.direction.y * t,
                ray.origin.z + ray.direction.z * t };
    FVec3 n = Normalize(Cross(e1, e2));
    if (Dot(n, ray.direction) > 0.0f) n = -n;        // レイ側を向く面法線
    r.normal = n;
    return r;
}

/**
 * 平面とレイの交差を求める。
 *
 * @param ray 入力レイ。
 * @param p 対象の平面。
 * @param t_max 探索する t の上限 (既定は実質無限大)。
 * @return 命中情報。法線は平面の法線をそのまま返す。レイが平面と平行なら非命中。
 */
ACS_FORCEINLINE FRayHit3 RaycastPlane(const FRay3& ray, const FPlane& p,
                                     f32 t_max = 3.4028235e38f) noexcept {
    FRayHit3 r{};
    const f32 nd = p.normal.x * ray.direction.x + p.normal.y * ray.direction.y + p.normal.z * ray.direction.z;
    if (Abs(nd) < 1e-8f) return r;  // 平行
    const f32 no = p.normal.x * ray.origin.x + p.normal.y * ray.origin.y + p.normal.z * ray.origin.z;
    const f32 t = -(no + p.d) / nd;
    if (t < 0 || t > t_max) return r;
    r.hit = true;
    r.t = t;
    r.point = { ray.origin.x + ray.direction.x * t,
                ray.origin.y + ray.direction.y * t,
                ray.origin.z + ray.direction.z * t };
    r.normal = p.normal;
    return r;
}

} // namespace acs
