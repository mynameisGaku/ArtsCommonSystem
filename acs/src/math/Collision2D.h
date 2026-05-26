// SPDX-License-Identifier: Apache-2.0
// 2D 衝突判定プリミティブ（AABB / 円 / 線分 / 点）
//
// ヘッダオンリー、ゲーム実装に直結する最小集合：
//   - 形状定義: Aabb2, Circle
//   - 重なり判定: Intersect(A, B)
//   - 押し出しベクトル: Resolve(A, B)
//   - レイキャスト: RaycastAabb / RaycastCircle
//
// 使い方:
//   Aabb2 player{ {x, y}, {w, h} };
//   Aabb2 wall  { {0, 0}, {100, 8} };
//   if (Intersect(player, wall)) { /* 衝突 */ }
//
//   Circle a{{px, py}, 16};
//   Circle b{{ex, ey}, 12};
//   FVec2 push;
//   if (Resolve(a, b, push)) { player.center += push; }
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"

namespace acs {

// 軸並行境界ボックス（中心 + 半サイズ）
struct Aabb2 {
    FVec2 center;
    FVec2 half_size;     // (w/2, h/2)

    constexpr Aabb2() noexcept = default;
    constexpr Aabb2(FVec2 c, FVec2 hs) noexcept : center(c), half_size(hs) {}

    // 左上 (min_x, min_y) と右下 (max_x, max_y) からの構築
    static constexpr Aabb2 FromMinMax(FVec2 min, FVec2 max) noexcept {
        FVec2 c{ (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f };
        FVec2 hs{ (max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f };
        return { c, hs };
    }
    // 左上 + サイズからの構築（スプライト系で便利）
    static constexpr Aabb2 FromTopLeftSize(FVec2 tl, FVec2 size) noexcept {
        FVec2 c{ tl.x + size.x * 0.5f, tl.y + size.y * 0.5f };
        FVec2 hs{ size.x * 0.5f, size.y * 0.5f };
        return { c, hs };
    }

    constexpr FVec2 Min() const noexcept { return { center.x - half_size.x, center.y - half_size.y }; }
    constexpr FVec2 Max() const noexcept { return { center.x + half_size.x, center.y + half_size.y }; }
};

// 円
struct Circle {
    FVec2 center;
    f32  radius = 0.0f;
};

// レイ（始点 + 方向、必ずしも正規化されてなくて良いが、t 解釈は方向長さ依存）
struct Ray2 {
    FVec2 origin;
    FVec2 direction;
};

// レイキャスト結果
struct RayHit2 {
    bool hit  = false;
    f32  t    = 0.0f;       // origin + direction * t が衝突点
    FVec2 point;
    FVec2 normal;            // 衝突点の外向き法線（命中体表面）
};

// ===== 点 vs 形状 =====
ACS_FORCEINLINE bool Contains(const Aabb2& a, FVec2 p) noexcept {
    return Abs(p.x - a.center.x) <= a.half_size.x &&
           Abs(p.y - a.center.y) <= a.half_size.y;
}
ACS_FORCEINLINE bool Contains(const Circle& c, FVec2 p) noexcept {
    const f32 dx = p.x - c.center.x;
    const f32 dy = p.y - c.center.y;
    return dx*dx + dy*dy <= c.radius * c.radius;
}

// ===== 重なり判定 =====
ACS_FORCEINLINE bool Intersect(const Aabb2& a, const Aabb2& b) noexcept {
    return Abs(a.center.x - b.center.x) <= (a.half_size.x + b.half_size.x) &&
           Abs(a.center.y - b.center.y) <= (a.half_size.y + b.half_size.y);
}
ACS_FORCEINLINE bool Intersect(const Circle& a, const Circle& b) noexcept {
    const f32 dx = a.center.x - b.center.x;
    const f32 dy = a.center.y - b.center.y;
    const f32 r  = a.radius + b.radius;
    return dx*dx + dy*dy <= r * r;
}
ACS_FORCEINLINE bool Intersect(const Aabb2& a, const Circle& c) noexcept {
    // AABB 上の最近傍点が円の中に入っているか
    const f32 cx = c.center.x < a.Min().x ? a.Min().x : (c.center.x > a.Max().x ? a.Max().x : c.center.x);
    const f32 cy = c.center.y < a.Min().y ? a.Min().y : (c.center.y > a.Max().y ? a.Max().y : c.center.y);
    const f32 dx = c.center.x - cx;
    const f32 dy = c.center.y - cy;
    return dx*dx + dy*dy <= c.radius * c.radius;
}
ACS_FORCEINLINE bool Intersect(const Circle& c, const Aabb2& a) noexcept { return Intersect(a, c); }

// ===== 押し出しベクトル（A を B から離す最小ベクトル）=====
// 戻り値: 衝突していたら true、push に A を動かすべき方向 × 距離が入る。
ACS_FORCEINLINE bool Resolve(const Circle& a, const Circle& b, FVec2& push) noexcept {
    const f32 dx = a.center.x - b.center.x;
    const f32 dy = a.center.y - b.center.y;
    const f32 d2 = dx*dx + dy*dy;
    const f32 r  = a.radius + b.radius;
    if (d2 >= r * r) return false;
    if (d2 < 1e-8f) {
        // 同心。任意方向（+X）に押し出す
        push = { r, 0 };
        return true;
    }
    const f32 d = Sqrt(d2);
    const f32 overlap = r - d;
    push = { (dx / d) * overlap, (dy / d) * overlap };
    return true;
}

ACS_FORCEINLINE bool Resolve(const Aabb2& a, const Aabb2& b, FVec2& push) noexcept {
    const f32 dx = a.center.x - b.center.x;
    const f32 dy = a.center.y - b.center.y;
    const f32 px = (a.half_size.x + b.half_size.x) - Abs(dx);
    const f32 py = (a.half_size.y + b.half_size.y) - Abs(dy);
    if (px <= 0 || py <= 0) return false;
    // 浅い軸に押し出す（最小貫通）
    if (px < py) push = { dx < 0 ? -px : px, 0 };
    else         push = { 0, dy < 0 ? -py : py };
    return true;
}

// ===== レイキャスト（slab method）=====
// AABB と無限長レイの交差。t は方向に対する係数。t が範囲内なら hit。
ACS_FORCEINLINE RayHit2 RaycastAabb(const Ray2& ray, const Aabb2& a,
                                    f32 t_max = 3.4028235e38f) noexcept {
    RayHit2 r{};
    const FVec2 mn = a.Min();
    const FVec2 mx = a.Max();
    const f32 inv_dx = ray.direction.x != 0.0f ? 1.0f / ray.direction.x : 1e30f;
    const f32 inv_dy = ray.direction.y != 0.0f ? 1.0f / ray.direction.y : 1e30f;

    f32 t1 = (mn.x - ray.origin.x) * inv_dx;
    f32 t2 = (mx.x - ray.origin.x) * inv_dx;
    f32 t3 = (mn.y - ray.origin.y) * inv_dy;
    f32 t4 = (mx.y - ray.origin.y) * inv_dy;

    f32 tmin_x = t1 < t2 ? t1 : t2;
    f32 tmax_x = t1 > t2 ? t1 : t2;
    f32 tmin_y = t3 < t4 ? t3 : t4;
    f32 tmax_y = t3 > t4 ? t3 : t4;

    f32 tmin = tmin_x > tmin_y ? tmin_x : tmin_y;
    f32 tmax = tmax_x < tmax_y ? tmax_x : tmax_y;

    if (tmax < 0 || tmin > tmax || tmin > t_max) return r;
    r.hit = true;
    r.t = tmin < 0 ? 0 : tmin;
    r.point = { ray.origin.x + ray.direction.x * r.t,
                ray.origin.y + ray.direction.y * r.t };
    // 法線: hit した軸を判定
    if (tmin_x > tmin_y) r.normal = { ray.direction.x < 0 ? 1.0f : -1.0f, 0 };
    else                 r.normal = { 0, ray.direction.y < 0 ? 1.0f : -1.0f };
    return r;
}

ACS_FORCEINLINE RayHit2 RaycastCircle(const Ray2& ray, const Circle& c,
                                      f32 t_max = 3.4028235e38f) noexcept {
    RayHit2 r{};
    const f32 ox = ray.origin.x - c.center.x;
    const f32 oy = ray.origin.y - c.center.y;
    const f32 dx = ray.direction.x;
    const f32 dy = ray.direction.y;
    const f32 a = dx*dx + dy*dy;
    if (a < 1e-12f) return r;
    const f32 b = 2.0f * (ox * dx + oy * dy);
    const f32 cc = ox*ox + oy*oy - c.radius * c.radius;
    const f32 disc = b * b - 4.0f * a * cc;
    if (disc < 0) return r;
    const f32 sd = Sqrt(disc);
    const f32 t = (-b - sd) / (2.0f * a);
    if (t < 0 || t > t_max) return r;
    r.hit = true;
    r.t = t;
    r.point = { ray.origin.x + dx * t, ray.origin.y + dy * t };
    const f32 nx = (r.point.x - c.center.x);
    const f32 ny = (r.point.y - c.center.y);
    const f32 inv_r = 1.0f / c.radius;
    r.normal = { nx * inv_r, ny * inv_r };
    return r;
}

} // namespace acs
