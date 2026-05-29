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

// 凸多角形（頂点は時計回り/反時計回りどちらでも可、凸であることが前提）。
// 物理で扱いやすいよう固定上限。スプライトの凸包コライダー等を載せる。
struct ConvexPoly2 {
    static constexpr u32 kMaxVerts = 16;
    FVec2 verts[kMaxVerts]{};
    u32   count = 0;

    void Clear() noexcept { count = 0; }
    void Add(FVec2 v) noexcept { if (count < kMaxVerts) verts[count++] = v; }
};

ACS_FORCEINLINE Aabb2 AabbOf(const ConvexPoly2& p) noexcept {
    if (p.count == 0) return Aabb2{};
    FVec2 mn = p.verts[0], mx = p.verts[0];
    for (u32 i = 1; i < p.count; ++i) {
        if (p.verts[i].x < mn.x) mn.x = p.verts[i].x;
        if (p.verts[i].y < mn.y) mn.y = p.verts[i].y;
        if (p.verts[i].x > mx.x) mx.x = p.verts[i].x;
        if (p.verts[i].y > mx.y) mx.y = p.verts[i].y;
    }
    return Aabb2::FromMinMax(mn, mx);
}

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

// ===== 凸多角形 =====
// 凸ポリゴンの内外判定 (全エッジで同じ側にあれば内側、巻き順は問わない)。
ACS_FORCEINLINE bool Contains(const ConvexPoly2& poly, FVec2 p) noexcept {
    if (poly.count < 3) return false;
    int sign = 0;
    for (u32 i = 0; i < poly.count; ++i) {
        const FVec2 a = poly.verts[i];
        const FVec2 b = poly.verts[(i + 1) % poly.count];
        const f32 cr = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (cr > 1e-6f)      { if (sign < 0) return false; sign = 1; }
        else if (cr < -1e-6f){ if (sign > 0) return false; sign = -1; }
    }
    return true;
}

namespace poly_detail {
ACS_FORCEINLINE void ProjectPoly(const ConvexPoly2& p, FVec2 axis, f32& mn, f32& mx) noexcept {
    mn = mx = p.verts[0].x * axis.x + p.verts[0].y * axis.y;
    for (u32 i = 1; i < p.count; ++i) {
        const f32 d = p.verts[i].x * axis.x + p.verts[i].y * axis.y;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
    }
}
ACS_FORCEINLINE bool AxisSeparates(const ConvexPoly2& a, const ConvexPoly2& b, FVec2 axis) noexcept {
    f32 amn, amx, bmn, bmx;
    ProjectPoly(a, axis, amn, amx);
    ProjectPoly(b, axis, bmn, bmx);
    return amx < bmn || bmx < amn;
}
} // namespace poly_detail

// SAT: 2 つの凸ポリゴンの重なり判定。
ACS_FORCEINLINE bool Intersect(const ConvexPoly2& a, const ConvexPoly2& b) noexcept {
    if (a.count < 3 || b.count < 3) return false;
    for (u32 i = 0; i < a.count; ++i) {
        const FVec2 e = a.verts[(i + 1) % a.count] - a.verts[i];
        if (poly_detail::AxisSeparates(a, b, FVec2{ -e.y, e.x })) return false;
    }
    for (u32 i = 0; i < b.count; ++i) {
        const FVec2 e = b.verts[(i + 1) % b.count] - b.verts[i];
        if (poly_detail::AxisSeparates(a, b, FVec2{ -e.y, e.x })) return false;
    }
    return true;
}

// 凸ポリゴン vs 円: 中心が内側、またはどれかのエッジに半径以内で接触なら重なる。
ACS_FORCEINLINE bool Intersect(const ConvexPoly2& poly, const Circle& c) noexcept {
    if (poly.count < 3) return false;
    if (Contains(poly, c.center)) return true;
    const f32 r2 = c.radius * c.radius;
    for (u32 i = 0; i < poly.count; ++i) {
        const FVec2 a = poly.verts[i];
        const FVec2 b = poly.verts[(i + 1) % poly.count];
        const FVec2 ab = b - a;
        const f32 len2 = ab.x * ab.x + ab.y * ab.y;
        f32 t = len2 > 1e-12f ? ((c.center.x - a.x) * ab.x + (c.center.y - a.y) * ab.y) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const f32 dx = c.center.x - (a.x + ab.x * t);
        const f32 dy = c.center.y - (a.y + ab.y * t);
        if (dx * dx + dy * dy <= r2) return true;
    }
    return false;
}
ACS_FORCEINLINE bool Intersect(const Circle& c, const ConvexPoly2& poly) noexcept { return Intersect(poly, c); }

// 凸ポリゴン vs AABB (AABB を 4 頂点ポリゴンに変換して SAT)。
ACS_FORCEINLINE ConvexPoly2 ToPoly(const Aabb2& box) noexcept {
    const FVec2 mn = box.Min(), mx = box.Max();
    ConvexPoly2 p;
    p.Add(FVec2{ mn.x, mn.y }); p.Add(FVec2{ mx.x, mn.y });
    p.Add(FVec2{ mx.x, mx.y }); p.Add(FVec2{ mn.x, mx.y });
    return p;
}
ACS_FORCEINLINE bool Intersect(const ConvexPoly2& poly, const Aabb2& box) noexcept {
    return Intersect(poly, ToPoly(box));
}
ACS_FORCEINLINE bool Intersect(const Aabb2& box, const ConvexPoly2& poly) noexcept { return Intersect(poly, box); }

ACS_FORCEINLINE FVec2 Centroid(const ConvexPoly2& p) noexcept {
    if (p.count == 0) return FVec2{ 0, 0 };
    FVec2 c{ 0, 0 };
    for (u32 i = 0; i < p.count; ++i) { c.x += p.verts[i].x; c.y += p.verts[i].y; }
    return FVec2{ c.x / static_cast<f32>(p.count), c.y / static_cast<f32>(p.count) };
}

// ===== 押し出しベクトル (SAT MTV)。push は「A を B から離す」最小移動 =====
ACS_FORCEINLINE bool Resolve(const ConvexPoly2& A, const ConvexPoly2& B, FVec2& push) noexcept {
    if (A.count < 3 || B.count < 3) return false;
    f32 min_overlap = 3.4028235e38f;
    FVec2 best_axis{ 0, 0 };
    for (int side = 0; side < 2; ++side) {
        const ConvexPoly2& P = (side == 0) ? A : B;
        for (u32 i = 0; i < P.count; ++i) {
            const FVec2 e = P.verts[(i + 1) % P.count] - P.verts[i];
            FVec2 axis = Normalize(FVec2{ -e.y, e.x });
            if (axis.x == 0.0f && axis.y == 0.0f) continue;
            f32 amn, amx, bmn, bmx;
            poly_detail::ProjectPoly(A, axis, amn, amx);
            poly_detail::ProjectPoly(B, axis, bmn, bmx);
            const f32 overlap = (amx < bmx ? amx : bmx) - (amn > bmn ? amn : bmn);
            if (overlap <= 0.0f) return false;       // 分離軸あり → 重ならない
            if (overlap < min_overlap) { min_overlap = overlap; best_axis = axis; }
        }
    }
    const FVec2 d = Centroid(A) - Centroid(B);
    if (best_axis.x * d.x + best_axis.y * d.y < 0.0f) best_axis = FVec2{ -best_axis.x, -best_axis.y };
    push = FVec2{ best_axis.x * min_overlap, best_axis.y * min_overlap };
    return true;
}

// 円を凸ポリゴンから押し出す MTV。
ACS_FORCEINLINE bool Resolve(const Circle& c, const ConvexPoly2& P, FVec2& push) noexcept {
    if (P.count < 3) return false;
    f32 min_overlap = 3.4028235e38f;
    FVec2 best_axis{ 0, 0 };
    auto test_axis = [&](FVec2 axis) -> bool {
        if (axis.x == 0.0f && axis.y == 0.0f) return true;
        f32 pmn, pmx;
        poly_detail::ProjectPoly(P, axis, pmn, pmx);
        const f32 cp = c.center.x * axis.x + c.center.y * axis.y;
        const f32 cmn = cp - c.radius, cmx = cp + c.radius;
        const f32 overlap = (pmx < cmx ? pmx : cmx) - (pmn > cmn ? pmn : cmn);
        if (overlap <= 0.0f) return false;
        if (overlap < min_overlap) { min_overlap = overlap; best_axis = axis; }
        return true;
    };
    for (u32 i = 0; i < P.count; ++i) {
        const FVec2 e = P.verts[(i + 1) % P.count] - P.verts[i];
        if (!test_axis(Normalize(FVec2{ -e.y, e.x }))) return false;
    }
    // 最近接頂点 → 円中心 の軸 (円の角衝突)
    u32 nearest = 0; f32 nd = 3.4028235e38f;
    for (u32 i = 0; i < P.count; ++i) {
        const f32 dq = (P.verts[i].x - c.center.x) * (P.verts[i].x - c.center.x) +
                       (P.verts[i].y - c.center.y) * (P.verts[i].y - c.center.y);
        if (dq < nd) { nd = dq; nearest = i; }
    }
    if (!test_axis(Normalize(c.center - P.verts[nearest]))) return false;

    const FVec2 d = c.center - Centroid(P);
    if (best_axis.x * d.x + best_axis.y * d.y < 0.0f) best_axis = FVec2{ -best_axis.x, -best_axis.y };
    push = FVec2{ best_axis.x * min_overlap, best_axis.y * min_overlap };  // 円を押し出す向き
    return true;
}

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

// レイ vs 凸ポリゴン: 各エッジ線分との交差のうち最近接を返す (エッジ法線つき)。
ACS_FORCEINLINE RayHit2 RaycastConvexPoly2(const Ray2& ray, const ConvexPoly2& poly,
                                           f32 t_max = 3.4028235e38f) noexcept {
    RayHit2 r{};
    if (poly.count < 2) return r;
    f32 best = t_max;
    for (u32 i = 0; i < poly.count; ++i) {
        const FVec2 a = poly.verts[i];
        const FVec2 b = poly.verts[(i + 1) % poly.count];
        const FVec2 e = b - a;
        const f32 denom = ray.direction.x * e.y - ray.direction.y * e.x;  // dir × e
        if (Abs(denom) < 1e-8f) continue;                                 // 平行
        const FVec2 ao = a - ray.origin;
        const f32 t = (ao.x * e.y - ao.y * e.x) / denom;
        const f32 u = (ao.x * ray.direction.y - ao.y * ray.direction.x) / denom;
        if (t >= 0.0f && t <= best && u >= 0.0f && u <= 1.0f) {
            best = t;
            r.hit = true;
            r.t = t;
            r.point = { ray.origin.x + ray.direction.x * t, ray.origin.y + ray.direction.y * t };
            FVec2 n = Normalize(FVec2{ e.y, -e.x });
            if (n.x * ray.direction.x + n.y * ray.direction.y > 0.0f) n = FVec2{ -n.x, -n.y };
            r.normal = n;
        }
    }
    return r;
}

} // namespace acs
