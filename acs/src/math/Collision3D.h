// 3D 衝突判定プリミティブ（AABB / 球 / 平面 / レイ）
//
// 使い方:
//   Aabb3 box = Aabb3::FromCenterExtents({0,0,0}, {1,1,1});
//   Sphere s{ {3,0,0}, 0.5f };
//   if (Intersect(box, s)) { /* 重なっている */ }
//
//   Ray3 ray{ camera.Eye(), forward };
//   RayHit3 h = RaycastAabb(ray, box);
//   if (h.hit) { /* h.point, h.normal, h.t */ }
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"

namespace acs {

// 軸並行境界ボックス（中心 + 半サイズ）
struct Aabb3 {
    Vec3 center;
    Vec3 half_size;

    constexpr Aabb3() noexcept = default;
    constexpr Aabb3(Vec3 c, Vec3 hs) noexcept : center(c), half_size(hs) {}

    static constexpr Aabb3 FromMinMax(Vec3 min, Vec3 max) noexcept {
        Vec3 c{ (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f };
        Vec3 hs{ (max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f, (max.z - min.z) * 0.5f };
        return { c, hs };
    }
    static constexpr Aabb3 FromCenterExtents(Vec3 center, Vec3 extents) noexcept {
        return { center, extents };
    }
    constexpr Vec3 Min() const noexcept { return { center.x - half_size.x, center.y - half_size.y, center.z - half_size.z }; }
    constexpr Vec3 Max() const noexcept { return { center.x + half_size.x, center.y + half_size.y, center.z + half_size.z }; }
};

// 球
struct Sphere {
    Vec3 center;
    f32  radius = 0.0f;
};

// 平面（法線・正規化必須、d は原点からの距離）
// 平面式: dot(normal, p) + d = 0
struct Plane {
    Vec3 normal;
    f32  d = 0.0f;

    static Plane FromPointNormal(Vec3 p, Vec3 n) noexcept {
        Plane pl;
        pl.normal = n;
        pl.d = -(n.x * p.x + n.y * p.y + n.z * p.z);
        return pl;
    }
};

struct Ray3 {
    Vec3 origin;
    Vec3 direction;
};

struct RayHit3 {
    bool hit = false;
    f32  t   = 0.0f;
    Vec3 point;
    Vec3 normal;
};

// ===== 含有 =====
ACS_FORCEINLINE bool Contains(const Aabb3& a, Vec3 p) noexcept {
    return Abs(p.x - a.center.x) <= a.half_size.x &&
           Abs(p.y - a.center.y) <= a.half_size.y &&
           Abs(p.z - a.center.z) <= a.half_size.z;
}
ACS_FORCEINLINE bool Contains(const Sphere& s, Vec3 p) noexcept {
    const f32 dx = p.x - s.center.x;
    const f32 dy = p.y - s.center.y;
    const f32 dz = p.z - s.center.z;
    return dx*dx + dy*dy + dz*dz <= s.radius * s.radius;
}

// ===== 重なり判定 =====
ACS_FORCEINLINE bool Intersect(const Aabb3& a, const Aabb3& b) noexcept {
    return Abs(a.center.x - b.center.x) <= (a.half_size.x + b.half_size.x) &&
           Abs(a.center.y - b.center.y) <= (a.half_size.y + b.half_size.y) &&
           Abs(a.center.z - b.center.z) <= (a.half_size.z + b.half_size.z);
}
ACS_FORCEINLINE bool Intersect(const Sphere& a, const Sphere& b) noexcept {
    const f32 dx = a.center.x - b.center.x;
    const f32 dy = a.center.y - b.center.y;
    const f32 dz = a.center.z - b.center.z;
    const f32 r  = a.radius + b.radius;
    return dx*dx + dy*dy + dz*dz <= r * r;
}
ACS_FORCEINLINE bool Intersect(const Aabb3& a, const Sphere& s) noexcept {
    const Vec3 mn = a.Min(), mx = a.Max();
    const f32 cx = s.center.x < mn.x ? mn.x : (s.center.x > mx.x ? mx.x : s.center.x);
    const f32 cy = s.center.y < mn.y ? mn.y : (s.center.y > mx.y ? mx.y : s.center.y);
    const f32 cz = s.center.z < mn.z ? mn.z : (s.center.z > mx.z ? mx.z : s.center.z);
    const f32 dx = s.center.x - cx;
    const f32 dy = s.center.y - cy;
    const f32 dz = s.center.z - cz;
    return dx*dx + dy*dy + dz*dz <= s.radius * s.radius;
}
ACS_FORCEINLINE bool Intersect(const Sphere& s, const Aabb3& a) noexcept { return Intersect(a, s); }

// ===== 押し出し（弾性衝突向け）=====
ACS_FORCEINLINE bool Resolve(const Sphere& a, const Sphere& b, Vec3& push) noexcept {
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

// ===== レイキャスト =====
ACS_FORCEINLINE RayHit3 RaycastAabb(const Ray3& ray, const Aabb3& a,
                                    f32 t_max = 3.4028235e38f) noexcept {
    RayHit3 r{};
    const Vec3 mn = a.Min(), mx = a.Max();
    const f32 inv_dx = ray.direction.x != 0.0f ? 1.0f / ray.direction.x : 1e30f;
    const f32 inv_dy = ray.direction.y != 0.0f ? 1.0f / ray.direction.y : 1e30f;
    const f32 inv_dz = ray.direction.z != 0.0f ? 1.0f / ray.direction.z : 1e30f;

    f32 t1 = (mn.x - ray.origin.x) * inv_dx;
    f32 t2 = (mx.x - ray.origin.x) * inv_dx;
    f32 t3 = (mn.y - ray.origin.y) * inv_dy;
    f32 t4 = (mx.y - ray.origin.y) * inv_dy;
    f32 t5 = (mn.z - ray.origin.z) * inv_dz;
    f32 t6 = (mx.z - ray.origin.z) * inv_dz;

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

ACS_FORCEINLINE RayHit3 RaycastSphere(const Ray3& ray, const Sphere& s,
                                      f32 t_max = 3.4028235e38f) noexcept {
    RayHit3 r{};
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

ACS_FORCEINLINE RayHit3 RaycastPlane(const Ray3& ray, const Plane& p,
                                     f32 t_max = 3.4028235e38f) noexcept {
    RayHit3 r{};
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
