// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — CRigidWorld2D 実装。詳細はヘッダ参照。
// 線形インパルスベースの接触ソルバ + Baumgarte 位置補正。
// =============================================================================
#include "gameframework/RigidWorld2D.h"

#include <cmath>   // std::sqrt

namespace acs::game {

namespace {

inline f32   VDot(FVec2 a, FVec2 b) noexcept { return a.x * b.x + a.y * b.y; }
inline f32   VLen(FVec2 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }
inline FVec2 VAdd(FVec2 a, FVec2 b) noexcept { return FVec2{ a.x + b.x, a.y + b.y }; }
inline FVec2 VSub(FVec2 a, FVec2 b) noexcept { return FVec2{ a.x - b.x, a.y - b.y }; }
inline FVec2 VScale(FVec2 v, f32 s) noexcept { return FVec2{ v.x * s, v.y * s }; }
inline f32   Clampf(f32 v, f32 lo, f32 hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
inline f32   Maxf(f32 a, f32 b) noexcept { return a > b ? a : b; }
inline f32   Minf(f32 a, f32 b) noexcept { return a < b ? a : b; }
// 2D 外積: スカラ角速度 w と腕 r → 接点速度 (w × r);  腕 r と力 f → スカラトルク (r × f)。
inline FVec2 CrossWR(f32 w, FVec2 r) noexcept { return FVec2{ -w * r.y, w * r.x }; }
inline f32   CrossRF(FVec2 r, FVec2 f) noexcept { return r.x * f.y - r.y * f.x; }

/** 接触マニフォルド: 法線 n (a→b 向き)・めり込み深さ pen・接触点 (最大 2)。hit=false で接触なし。
 *  箱-箱は面接触なので 2 点 (静止が安定して回転で暴れない)、曲面 (円) は 1 点。 */
struct FManifold {
    FVec2 n{0.0f, 0.0f};
    f32   pen = 0.0f;
    FVec2 points[2]{};
    u32   count = 0u;
    bool  hit   = false;
};

/** 円-円 (法線は a→b、1 点)。 */
FManifold CircleCircle(const FRigidBodyState& a, const FRigidBodyState& b) noexcept {
    const FVec2 d    = VSub(b.pos, a.pos);
    const f32   dist = VLen(d);
    const f32   sum  = a.radius + b.radius;
    if (dist >= sum) return {};
    const FVec2 n = (dist > 1e-6f) ? VScale(d, 1.0f / dist) : FVec2{ 0.0f, 1.0f };
    FManifold m; m.n = n; m.pen = sum - dist; m.count = 1u; m.hit = true;
    m.points[0] = FVec2{ a.pos.x + n.x * a.radius, a.pos.y + n.y * a.radius };  // a 表面上の接点
    return m;
}

// ベクトルを角度 (cos c, sin s) で回す。
inline FVec2 Rotate(FVec2 v, f32 c, f32 s) noexcept { return FVec2{ v.x * c - v.y * s, v.x * s + v.y * c }; }

/** 有向凸ポリゴン (箱は 4 頂点、任意凸は最大 kMaxPolyVerts)。v=頂点 (world), n=各辺の外向き法線。 */
struct FPoly {
    FVec2 v[kMaxPolyVerts];
    FVec2 n[kMaxPolyVerts];
    u32   count = 0u;
};

/** ポリゴンの外向き辺法線を計算する (CCW 前提なので (e.y,-e.x))。 */
void ComputePolyNormals(FPoly& p) noexcept {
    for (u32 i = 0; i < p.count; ++i) {
        const FVec2 e = VSub(p.v[(i + 1) % p.count], p.v[i]);
        FVec2 nn{ e.y, -e.x };
        const f32 l = VLen(nn);
        p.n[i] = (l > 1e-9f) ? VScale(nn, 1.0f / l) : FVec2{ 0.0f, 0.0f };
    }
}

/** 有向ボックス (OBB) を world ポリゴンに展開する (angle で回転)。 */
FPoly MakeBoxPoly(const FRigidBodyState& b) noexcept {
    const f32 c = std::cos(b.angle), s = std::sin(b.angle);
    const FVec2 hx = Rotate(FVec2{ b.half.x, 0.0f }, c, s);   // 回転後ローカル x 軸 × half.x
    const FVec2 hy = Rotate(FVec2{ 0.0f, b.half.y }, c, s);   // 回転後ローカル y 軸 × half.y
    FPoly p; p.count = 4u;
    p.v[0] = VSub(VSub(b.pos, hx), hy);   // (-x,-y)
    p.v[1] = VSub(VAdd(b.pos, hx), hy);   // (+x,-y)
    p.v[2] = VAdd(VAdd(b.pos, hx), hy);   // (+x,+y)
    p.v[3] = VAdd(VSub(b.pos, hx), hy);   // (-x,+y)
    ComputePolyNormals(p);
    return p;
}

/** ポリゴンボディ (b.poly ローカル頂点) を world ポリゴンに展開する。 */
FPoly MakePolyBody(const FRigidBodyState& b) noexcept {
    const f32 c = std::cos(b.angle), s = std::sin(b.angle);
    FPoly p; p.count = b.poly_count;
    for (u32 i = 0; i < b.poly_count; ++i) p.v[i] = VAdd(b.pos, Rotate(b.poly[i], c, s));
    ComputePolyNormals(p);
    return p;
}

/** ボディ (箱 or ポリゴン) を world 凸ポリゴンに変換する。 */
FPoly BodyToPoly(const FRigidBodyState& b) noexcept {
    return (b.poly_count > 0u) ? MakePolyBody(b) : MakeBoxPoly(b);
}

/** 線分 in[2] を半平面 dot(n,p) <= offset で切る (Sutherland-Hodgman)。残った点を out へ、点数を返す。 */
u32 ClipSeg(FVec2 out[2], const FVec2 in[2], FVec2 n, f32 offset) noexcept {
    u32 num = 0u;
    const f32 d0 = VDot(n, in[0]) - offset;
    const f32 d1 = VDot(n, in[1]) - offset;
    if (d0 <= 0.0f)               out[num++] = in[0];
    if (d1 <= 0.0f && num < 2u)   out[num++] = in[1];
    if (d0 * d1 < 0.0f && num < 2u) {
        const f32 t = d0 / (d0 - d1);
        out[num++] = VAdd(in[0], VScale(VSub(in[1], in[0]), t));
    }
    return num;
}

/** A の各面に対する B の最大分離量 (>0 で分離 = 衝突なし)。最良面 index を返す。 */
f32 MaxSeparation(const FPoly& A, const FPoly& B, u32& bestFace) noexcept {
    f32 best = -1e30f; u32 bi = 0u;
    for (u32 i = 0; i < A.count; ++i) {
        const FVec2 nrm = A.n[i];
        f32 minDot = 1e30f;                       // -nrm 方向の B の支持点 (最もめり込む頂点)
        for (u32 j = 0; j < B.count; ++j) { const f32 d = VDot(nrm, B.v[j]); if (d < minDot) minDot = d; }
        const f32 sep = minDot - VDot(nrm, A.v[i]);
        if (sep > best) { best = sep; bi = i; }
    }
    bestFace = bi; return best;
}

/** 有向ボックス同士 (SAT + 面クリッピング、最大 2 接触点)。法線は a→b。 */
FManifold PolyPoly(const FPoly& A, const FPoly& B) noexcept {
    u32 faceA; const f32 sepA = MaxSeparation(A, B, faceA);
    if (sepA > 0.0f) return {};
    u32 faceB; const f32 sepB = MaxSeparation(B, A, faceB);
    if (sepB > 0.0f) return {};

    // 参照面 = 分離が大きい (めり込みが浅い) 側。A を優先するバイアスで安定化。
    const FPoly* ref; const FPoly* inc; u32 refFace; bool flip;
    if (sepB > sepA + 0.001f) { ref = &B; inc = &A; refFace = faceB; flip = true;  }
    else                      { ref = &A; inc = &B; refFace = faceA; flip = false; }

    const FVec2 refNormal = ref->n[refFace];
    // 入射面 = inc のうち refNormal に最も反平行な面。
    u32 incFace = 0u; f32 minDot = 1e30f;
    for (u32 i = 0; i < inc->count; ++i) { const f32 d = VDot(refNormal, inc->n[i]); if (d < minDot) { minDot = d; incFace = i; } }
    FVec2 seg[2] = { inc->v[incFace], inc->v[(incFace + 1) % inc->count] };

    const FVec2 r1 = ref->v[refFace];
    const FVec2 r2 = ref->v[(refFace + 1) % ref->count];
    const f32   tl = Maxf(VLen(VSub(r2, r1)), 1e-9f);
    const FVec2 tang = VScale(VSub(r2, r1), 1.0f / tl);

    // 参照面の両側の側面で入射線分をクリップする (面の幅にそろえる)。
    FVec2 tmp[2];
    if (ClipSeg(tmp, seg, VScale(tang, -1.0f), -VDot(tang, r1)) < 2u) return {};
    seg[0] = tmp[0]; seg[1] = tmp[1];
    if (ClipSeg(tmp, seg, tang, VDot(tang, r2)) < 2u) return {};
    seg[0] = tmp[0]; seg[1] = tmp[1];

    FManifold m; m.hit = true; m.n = flip ? VScale(refNormal, -1.0f) : refNormal; m.count = 0u;
    const f32 refC = VDot(refNormal, r1);
    f32 maxPen = 0.0f;
    for (u32 k = 0; k < 2u; ++k) {                 // 参照面より内側 (めり込んでいる) 点だけ採用。
        const f32 sep = VDot(refNormal, seg[k]) - refC;
        if (sep <= 0.0f) { m.points[m.count++] = seg[k]; if (-sep > maxPen) maxPen = -sep; }
    }
    if (m.count == 0u) return {};
    m.pen = maxPen;
    return m;
}

/** 円 vs 凸ポリゴン (法線は poly→circle = 円を押し出す向き、1 点)。 */
FManifold CircleVsConvex(const FRigidBodyState& circle, const FPoly& poly) noexcept {
    const FVec2 cc = circle.pos;
    // 各辺の分離量 dot(n_i, c - v_i) の最大 = SAT。
    f32 maxSep = -1e30f; u32 bestEdge = 0u;
    for (u32 i = 0; i < poly.count; ++i) {
        const f32 sep = VDot(poly.n[i], VSub(cc, poly.v[i]));
        if (sep > maxSep) { maxSep = sep; bestEdge = i; }
    }
    if (maxSep > circle.radius) return {};                  // 分離
    FManifold m; m.hit = true; m.count = 1u;
    if (maxSep < 1e-6f) {                                   // 中心がポリゴン内 → bestEdge 法線で押し出す
        m.n = poly.n[bestEdge];                            // poly→circle (外向き)
        m.pen = circle.radius - maxSep;
        m.points[0] = VSub(cc, VScale(m.n, circle.radius));
        return m;
    }
    // 中心は外: bestEdge 上の最近点。
    const FVec2 v1 = poly.v[bestEdge];
    const FVec2 v2 = poly.v[(bestEdge + 1) % poly.count];
    const FVec2 e  = VSub(v2, v1);
    const f32   t  = Clampf(VDot(VSub(cc, v1), e) / Maxf(VDot(e, e), 1e-9f), 0.0f, 1.0f);
    const FVec2 closest = VAdd(v1, VScale(e, t));
    const FVec2 d  = VSub(cc, closest);
    const f32   dist = VLen(d);
    if (dist >= circle.radius) return {};
    m.n = (dist > 1e-6f) ? VScale(d, 1.0f / dist) : poly.n[bestEdge];   // poly→circle
    m.pen = circle.radius - dist;
    m.points[0] = closest;
    return m;
}

/** ペア (a,b) の接触マニフォルド (法線は a→b)。円 / 凸ポリゴン (箱含む) の全組合せ。 */
FManifold Collide(const FRigidBodyState& a, const FRigidBodyState& b) noexcept {
    if (a.is_circle && b.is_circle) return CircleCircle(a, b);
    if (a.is_circle && !b.is_circle) {                 // 円 a vs 凸 b: poly→circle を反転して circle→poly (=a→b)
        FManifold m = CircleVsConvex(a, BodyToPoly(b));
        m.n = VScale(m.n, -1.0f);
        return m;
    }
    if (!a.is_circle && b.is_circle)                   // 凸 a vs 円 b: poly→circle (=a→b)
        return CircleVsConvex(b, BodyToPoly(a));
    return PolyPoly(BodyToPoly(a), BodyToPoly(b));      // 凸-凸 (OBB / ポリゴン)
}

/** 1 接触点ぶんの制約 (逐次インパルス + 累積。Gauss-Seidel で反復解く)。 */
struct FContact {
    u32   a = 0u, b = 0u;
    FVec2 n{0.0f, 0.0f};
    FVec2 ra{0.0f, 0.0f}, rb{0.0f, 0.0f};
    f32   mass_n = 0.0f, mass_t = 0.0f;
    f32   rest_bias = 0.0f;       // 反発の目標分離速度 (>=0)
    f32   pn = 0.0f, pt = 0.0f;   // 累積インパルス (法線/接線)
};

// ----- CCD (連続衝突判定 / トンネリング防止) -----

/** CCD スウィープに使うボディの代表半径 (円=半径、ボックス=外接円、ポリゴン=最遠頂点)。
 *  ボックス/ポリゴンは外接円で保守近似するので、接触手前で止まる (= 決してすり抜けない)。 */
f32 CcdRadius(const FRigidBodyState& b) noexcept {
    if (b.is_circle) return b.radius;
    if (b.poly_count > 0u) {
        f32 mx = 0.0f;
        for (u32 i = 0; i < b.poly_count; ++i) { const f32 d = VLen(b.poly[i]); if (d > mx) mx = d; }
        return mx;
    }
    return VLen(b.half);
}

/** 動的円 (中心 p0・半径 r) が変位 disp で動くとき、静的ボディ s に最初に当たる割合 toi∈[0,1] と
 *  接触法線 n (s から動的側への外向き単位) を返す。対象は静的な円 / 軸並行ボックスのみ。
 *  既に重なり・回転静的・凸ポリゴン静的・非交差は false (離散判定にフォールバック)。 */
bool SweptCircleVsStatic(FVec2 p0, FVec2 disp, f32 r,
                         const FRigidBodyState& s, f32& toi, FVec2& n) noexcept {
    if (s.is_circle) {
        // ray P(t)=p0+disp*t と 円 (中心 s.pos, 半径 R=s.radius+r) の最初の交点 (Minkowski)。
        const f32   R  = s.radius + r;
        const FVec2 m  = VSub(p0, s.pos);
        const f32   c2 = VDot(m, m) - R * R;
        if (c2 < 0.0f) return false;                    // 既に重なり → 離散にまかせる
        const f32 a = VDot(disp, disp);
        if (a < 1e-12f) return false;                   // 動いていない
        const f32 b = 2.0f * VDot(m, disp);
        const f32 disc = b * b - 4.0f * a * c2;
        if (disc < 0.0f) return false;
        const f32 t = (-b - std::sqrt(disc)) / (2.0f * a);
        if (t < 0.0f || t > 1.0f) return false;
        const FVec2 hit = VAdd(p0, VScale(disp, t));
        FVec2 nn = VSub(hit, s.pos);
        const f32 nl = VLen(nn);
        n = (nl > 1e-6f) ? VScale(nn, 1.0f / nl)
                         : VScale(disp, -1.0f / Maxf(VLen(disp), 1e-6f));
        toi = t;
        return true;
    }
    if (s.poly_count > 0u) return false;                // 凸ポリゴン静的は CCD 対象外
    if (s.angle > 1e-3f || s.angle < -1e-3f) return false;   // 回転静的は対象外 (離散で解く)

    // 軸並行ボックスを r だけ膨張させ (Minkowski)、中心レイの slab 法でエントリ tmin を求める。
    const f32 minx = s.pos.x - s.half.x - r, maxx = s.pos.x + s.half.x + r;
    const f32 miny = s.pos.y - s.half.y - r, maxy = s.pos.y + s.half.y + r;
    if (p0.x > minx && p0.x < maxx && p0.y > miny && p0.y < maxy) return false;   // 始点が内側 → 重なり扱い
    f32   tmin = -1e30f, tmax = 1e30f;
    FVec2 nrm{ 0.0f, 0.0f };
    if (disp.x > -1e-9f && disp.x < 1e-9f) { if (p0.x < minx || p0.x > maxx) return false; }
    else {
        const f32 inv = 1.0f / disp.x;
        f32 t1 = (minx - p0.x) * inv, t2 = (maxx - p0.x) * inv;
        FVec2 face{ inv < 0.0f ? 1.0f : -1.0f, 0.0f };
        if (t1 > t2) { const f32 tt = t1; t1 = t2; t2 = tt; face = FVec2{ -face.x, 0.0f }; }
        if (t1 > tmin) { tmin = t1; nrm = face; }
        if (t2 < tmax) tmax = t2;
    }
    if (disp.y > -1e-9f && disp.y < 1e-9f) { if (p0.y < miny || p0.y > maxy) return false; }
    else {
        const f32 inv = 1.0f / disp.y;
        f32 t1 = (miny - p0.y) * inv, t2 = (maxy - p0.y) * inv;
        FVec2 face{ 0.0f, inv < 0.0f ? 1.0f : -1.0f };
        if (t1 > t2) { const f32 tt = t1; t1 = t2; t2 = tt; face = FVec2{ 0.0f, -face.y }; }
        if (t1 > tmin) { tmin = t1; nrm = face; }
        if (t2 < tmax) tmax = t2;
    }
    if (tmax < tmin) return false;                      // miss
    if (tmin < 0.0f || tmin > 1.0f) return false;       // 接触は変位 [0,1] 内のみ
    toi = tmin; n = nrm;
    return true;
}

} // namespace

u32 CRigidWorld2D::AllocBody(const FRigidBodyState& state) noexcept {
    ++m_ActiveCount;
    if (m_FreeList.Num() > 0) {                       // 削除済み slot を再利用
        const u32 idx = m_FreeList[m_FreeList.Num() - 1];
        m_FreeList.Pop();
        m_Bodies[idx] = state;
        m_Bodies[idx].active = true;
        return idx;
    }
    m_Bodies.Add(state);
    return static_cast<u32>(m_Bodies.Num() - 1);
}

void CRigidWorld2D::RemoveBody(u32 index) noexcept {
    if (index >= m_Bodies.Num() || !m_Bodies[index].active) return;
    m_Bodies[index].active   = false;     // tombstone
    m_Bodies[index].inv_mass = 0.0f;      // 念のため (万一処理されても不動)
    m_FreeList.Add(index);
    if (m_ActiveCount > 0) --m_ActiveCount;
}

u32 CRigidWorld2D::AddCircle(FVec2 pos, f32 radius, f32 mass, f32 restitution, f32 friction) noexcept {
    FRigidBodyState b;
    b.pos = pos; b.radius = radius; b.is_circle = true;
    b.inv_mass = (mass > 0.0f) ? (1.0f / mass) : 0.0f;
    // 円の慣性 I = 0.5·m·r² → inv_inertia。
    b.inv_inertia = (mass > 0.0f && radius > 0.0f) ? 1.0f / (0.5f * mass * radius * radius) : 0.0f;
    b.restitution = Clampf(restitution, 0.0f, 1.0f);
    b.friction = Maxf(friction, 0.0f);
    return AllocBody(b);
}

u32 CRigidWorld2D::AddDynamicAabb(FVec2 pos, FVec2 half, f32 mass, f32 restitution, f32 friction) noexcept {
    FRigidBodyState b;
    b.pos = pos; b.half = half; b.is_circle = false;
    b.inv_mass = (mass > 0.0f) ? (1.0f / mass) : 0.0f;
    // 矩形の慣性 I = m·(W²+H²)/12 = m·(hx²+hy²)/3 → inv_inertia。
    const f32 denom = mass * (half.x * half.x + half.y * half.y);
    b.inv_inertia = (mass > 0.0f && denom > 0.0f) ? 3.0f / denom : 0.0f;
    b.restitution = Clampf(restitution, 0.0f, 1.0f);
    b.friction = Maxf(friction, 0.0f);
    return AllocBody(b);
}

u32 CRigidWorld2D::AddStaticAabb(FVec2 center, FVec2 half, f32 restitution, f32 friction) noexcept {
    FRigidBodyState b;
    b.pos = center; b.half = half; b.is_circle = false;
    b.inv_mass = 0.0f;   // 静的
    b.restitution = Clampf(restitution, 0.0f, 1.0f);
    b.friction = Maxf(friction, 0.0f);
    return AllocBody(b);
}

u32 CRigidWorld2D::AddPolygon(FVec2 pos, const FVec2* local_verts, u32 count, f32 mass,
                              f32 restitution, f32 friction) noexcept {
    FRigidBodyState b;
    b.pos = pos; b.is_circle = false;
    b.restitution = Clampf(restitution, 0.0f, 1.0f);
    b.friction = Maxf(friction, 0.0f);
    if (count > kMaxPolyVerts) count = kMaxPolyVerts;
    if (local_verts == nullptr || count < 3u) {            // 退化 → 小さな箱で代用
        b.half = FVec2{ 0.5f, 0.5f };
        b.inv_mass = (mass > 0.0f) ? 1.0f / mass : 0.0f;
        return AllocBody(b);
    }
    // 符号付き面積で CCW に揃える (外向き法線の符号が一致するように)。
    f32 area2 = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        const FVec2& p0 = local_verts[i];
        const FVec2& p1 = local_verts[(i + 1) % count];
        area2 += p0.x * p1.y - p1.x * p0.y;
    }
    b.poly_count = static_cast<u8>(count);
    for (u32 i = 0; i < count; ++i)
        b.poly[i] = (area2 < 0.0f) ? local_verts[count - 1 - i] : local_verts[i];   // CW なら反転

    b.inv_mass = (mass > 0.0f) ? 1.0f / mass : 0.0f;
    if (mass > 0.0f) {
        // ポリゴン慣性 (body 原点まわり = 回転ピボット)。I = m·Σcr(dot 和)/(6·Σcr)。
        f32 num = 0.0f, den = 0.0f;
        for (u32 i = 0; i < count; ++i) {
            const FVec2 p0 = b.poly[i], p1 = b.poly[(i + 1) % count];
            const f32 cr = p0.x * p1.y - p1.x * p0.y;
            num += cr * (VDot(p0, p0) + VDot(p0, p1) + VDot(p1, p1));
            den += cr;
        }
        const f32 I = (den > 1e-6f) ? mass * num / (6.0f * den) : 0.0f;
        b.inv_inertia = (I > 1e-6f) ? 1.0f / I : 0.0f;
    }
    return AllocBody(b);
}

u32 CRigidWorld2D::AddSensorCircle(FVec2 pos, f32 radius) noexcept {
    FRigidBodyState b;
    b.pos = pos; b.radius = radius; b.is_circle = true;
    b.inv_mass = 0.0f; b.is_sensor = true;   // 静的・素通り
    return AllocBody(b);
}

u32 CRigidWorld2D::AddSensorAabb(FVec2 center, FVec2 half) noexcept {
    FRigidBodyState b;
    b.pos = center; b.half = half; b.is_circle = false;
    b.inv_mass = 0.0f; b.is_sensor = true;
    return AllocBody(b);
}

void CRigidWorld2D::Step(f32 dt, FVec2 gravity) noexcept {
    if (dt <= 0.0f) return;

    // --- 1. 速度積分 (重力 + 減衰)。動的ボディのみ。 ---
    for (u32 i = 0; i < m_Bodies.Num(); ++i) {
        FRigidBodyState& bd = m_Bodies[i];
        if (!bd.active || bd.inv_mass <= 0.0f) continue;
        bd.vel.x += gravity.x * dt;
        bd.vel.y += gravity.y * dt;
        if (bd.lin_damp > 0.0f) { const f32 d = 1.0f / (1.0f + bd.lin_damp * dt); bd.vel.x *= d; bd.vel.y *= d; }
        if (bd.ang_damp > 0.0f) bd.ang_vel *= 1.0f / (1.0f + bd.ang_damp * dt);
    }

    // --- 2. 接触生成 (全ペア。箱-箱は 2 点)。反発バイアスは初期接近速度から決める。 ---
    // 低速接触では反発を 0 にして静止ジッタを防ぐ。閾値は重力×dt にスケールさせ、
    // SI スケール (g=10) でもピクセルスケール (g=900) でも妥当に効くようにする。
    const f32 restThresh = VLen(gravity) * dt * 2.0f;
    TArray<FContact> contacts;
    for (u32 i = 0; i < m_Bodies.Num(); ++i) {
        FRigidBodyState& A = m_Bodies[i];
        if (!A.active || A.is_sensor) continue;
        for (u32 j = i + 1; j < m_Bodies.Num(); ++j) {
            FRigidBodyState& B = m_Bodies[j];
            if (!B.active || B.is_sensor) continue;
            if (A.inv_mass + B.inv_mass <= 0.0f) continue;   // 両方静的 → 衝突解決しない
            const FManifold m = Collide(A, B);
            if (!m.hit) continue;
            const f32 e = Minf(A.restitution, B.restitution);
            const FVec2 t{ -m.n.y, m.n.x };
            for (u32 k = 0; k < m.count; ++k) {
                FContact c;
                c.a = i; c.b = j; c.n = m.n;
                c.ra = VSub(m.points[k], A.pos);
                c.rb = VSub(m.points[k], B.pos);
                const f32 rnA = CrossRF(c.ra, m.n), rnB = CrossRF(c.rb, m.n);
                const f32 kn = A.inv_mass + B.inv_mass + A.inv_inertia * rnA * rnA + B.inv_inertia * rnB * rnB;
                c.mass_n = (kn > 0.0f) ? 1.0f / kn : 0.0f;
                const f32 rtA = CrossRF(c.ra, t), rtB = CrossRF(c.rb, t);
                const f32 kt = A.inv_mass + B.inv_mass + A.inv_inertia * rtA * rtA + B.inv_inertia * rtB * rtB;
                c.mass_t = (kt > 0.0f) ? 1.0f / kt : 0.0f;
                const FVec2 vA = VAdd(A.vel, CrossWR(A.ang_vel, c.ra));
                const FVec2 vB = VAdd(B.vel, CrossWR(B.ang_vel, c.rb));
                const f32   vn0 = VDot(VSub(vB, vA), m.n);
                c.rest_bias = (vn0 < -restThresh) ? -e * vn0 : 0.0f;   // 速い接近のみ跳ね返す
                contacts.Add(c);
            }
        }
    }

    // --- 3. 速度反復 (逐次インパルス、累積)。法線→接線 (摩擦) を Gauss-Seidel で。 ---
    for (u32 it = 0; it < kVelIters; ++it) {
        for (u32 ci = 0; ci < contacts.Num(); ++ci) {
            FContact& c = contacts[ci];
            FRigidBodyState& A = m_Bodies[c.a];
            FRigidBodyState& B = m_Bodies[c.b];
            const FVec2 t{ -c.n.y, c.n.x };

            // 法線インパルス (累積を 0 でクランプ → 引っ張らない)。
            FVec2 vA = VAdd(A.vel, CrossWR(A.ang_vel, c.ra));
            FVec2 vB = VAdd(B.vel, CrossWR(B.ang_vel, c.rb));
            f32 vn = VDot(VSub(vB, vA), c.n);
            f32 dPn = c.mass_n * (c.rest_bias - vn);
            const f32 pn0 = c.pn;
            c.pn = Maxf(pn0 + dPn, 0.0f);
            dPn = c.pn - pn0;
            const FVec2 Pn = VScale(c.n, dPn);
            A.vel = VSub(A.vel, VScale(Pn, A.inv_mass));
            A.ang_vel -= A.inv_inertia * CrossRF(c.ra, Pn);
            B.vel = VAdd(B.vel, VScale(Pn, B.inv_mass));
            B.ang_vel += B.inv_inertia * CrossRF(c.rb, Pn);

            // 接線インパルス (摩擦)。|累積接線| <= μ·|累積法線| で Coulomb 円錐に拘束。
            vA = VAdd(A.vel, CrossWR(A.ang_vel, c.ra));
            vB = VAdd(B.vel, CrossWR(B.ang_vel, c.rb));
            const f32 vt = VDot(VSub(vB, vA), t);
            f32 dPt = c.mass_t * (-vt);
            const f32 mu = std::sqrt(A.friction * B.friction);
            const f32 maxPt = mu * c.pn;
            const f32 pt0 = c.pt;
            c.pt = Clampf(pt0 + dPt, -maxPt, maxPt);
            dPt = c.pt - pt0;
            const FVec2 Pt = VScale(t, dPt);
            A.vel = VSub(A.vel, VScale(Pt, A.inv_mass));
            A.ang_vel -= A.inv_inertia * CrossRF(c.ra, Pt);
            B.vel = VAdd(B.vel, VScale(Pt, B.inv_mass));
            B.ang_vel += B.inv_inertia * CrossRF(c.rb, Pt);
        }
    }

    // --- 4. 位置積分 (CCD 有効ボディは静的形状へのスウィープで TOI クランプ)。 ---
    for (u32 i = 0; i < m_Bodies.Num(); ++i) {
        FRigidBodyState& bd = m_Bodies[i];
        if (!bd.active || bd.inv_mass <= 0.0f) continue;
        const FVec2 disp{ bd.vel.x * dt, bd.vel.y * dt };

        if (bd.ccd && !bd.is_sensor) {
            const f32 cr = CcdRadius(bd);
            // 1 ステップの移動量が自半径を超える「高速」時のみスウィープ (低速は離散で十分・安価)。
            if (VDot(disp, disp) > cr * cr) {
                f32 bestToi = 1e30f; FVec2 bestN{ 0.0f, 0.0f }; bool blocked = false;
                for (u32 j = 0; j < m_Bodies.Num(); ++j) {
                    if (j == i) continue;
                    const FRigidBodyState& s = m_Bodies[j];
                    if (!s.active || s.is_sensor || s.inv_mass > 0.0f) continue;   // 静的のみ
                    f32 toi; FVec2 nn;
                    if (SweptCircleVsStatic(bd.pos, disp, cr, s, toi, nn) && toi < bestToi) {
                        bestToi = toi; bestN = nn; blocked = true;
                    }
                }
                if (blocked) {
                    const f32 safe = Maxf(bestToi - 0.001f, 0.0f);   // 接触手前 (薄皮) で止める
                    bd.pos.x += disp.x * safe;
                    bd.pos.y += disp.y * safe;
                    bd.angle += bd.ang_vel * dt;
                    const f32 vn = VDot(bd.vel, bestN);              // 面へ食い込む速度成分を除去
                    if (vn < 0.0f) bd.vel = VSub(bd.vel, VScale(bestN, vn));  // 接線は残し滑りを許容
                    continue;
                }
            }
        }

        bd.pos.x += disp.x;
        bd.pos.y += disp.y;
        bd.angle += bd.ang_vel * dt;
    }

    // --- 5. 位置補正 (NGS): めり込みを位置のみで除去する。速度を変えないのでエネルギーを
    //         注入せず、Baumgarte の跳ね/ジッタが出ない。接触を再検出して押し出す。 ---
    constexpr f32 kSlop = 0.01f, kPosBeta = 0.2f;
    for (u32 it = 0; it < kPosIters; ++it) {
        for (u32 i = 0; i < m_Bodies.Num(); ++i) {
            FRigidBodyState& A = m_Bodies[i];
            if (!A.active || A.is_sensor) continue;
            for (u32 j = i + 1; j < m_Bodies.Num(); ++j) {
                FRigidBodyState& B = m_Bodies[j];
                if (!B.active || B.is_sensor) continue;
                const f32 invSum = A.inv_mass + B.inv_mass;
                if (invSum <= 0.0f) continue;
                const FManifold m = Collide(A, B);
                if (!m.hit) continue;
                const f32 corr = Maxf(m.pen - kSlop, 0.0f) * kPosBeta / invSum;
                A.pos = VSub(A.pos, VScale(m.n, corr * A.inv_mass));
                B.pos = VAdd(B.pos, VScale(m.n, corr * B.inv_mass));
            }
        }
    }
}

// ----- 空間クエリ -----
namespace {

/** 点がボディ内か。 */
bool PointInBody(const FRigidBodyState& b, FVec2 p) noexcept {
    if (b.is_circle) {
        const FVec2 d = VSub(p, b.pos);
        return VDot(d, d) <= b.radius * b.radius;
    }
    return p.x >= b.pos.x - b.half.x && p.x <= b.pos.x + b.half.x
        && p.y >= b.pos.y - b.half.y && p.y <= b.pos.y + b.half.y;
}

/** レイ (o + t·d, d 正規化済) vs 円。ヒットで t/n を返す。 */
bool RayCircle(FVec2 o, FVec2 d, const FRigidBodyState& b, f32& t, FVec2& n) noexcept {
    const FVec2 m = VSub(o, b.pos);
    const f32 bdot = VDot(m, d);
    const f32 c    = VDot(m, m) - b.radius * b.radius;
    if (c > 0.0f && bdot > 0.0f) return false;          // 外側 + 逆向き
    const f32 disc = bdot * bdot - c;
    if (disc < 0.0f) return false;
    f32 tt = -bdot - std::sqrt(disc);
    if (tt < 0.0f) tt = 0.0f;                           // 始点が内側
    t = tt;
    const FVec2 hp{ o.x + d.x * tt, o.y + d.y * tt };
    FVec2 nn = VSub(hp, b.pos);
    const f32 nl = VLen(nn);
    n = (nl > 1e-6f) ? VScale(nn, 1.0f / nl) : FVec2{ 0.0f, -1.0f };
    return true;
}

/** レイ vs AABB (slab 法)。ヒットで t/n を返す。 */
bool RayAabb(FVec2 o, FVec2 d, const FRigidBodyState& b, f32& tOut, FVec2& n) noexcept {
    const f32 minx = b.pos.x - b.half.x, maxx = b.pos.x + b.half.x;
    const f32 miny = b.pos.y - b.half.y, maxy = b.pos.y + b.half.y;
    f32 tmin = -1e30f, tmax = 1e30f;
    FVec2 nmin{ 0.0f, 0.0f };

    if (d.x > -1e-9f && d.x < 1e-9f) { if (o.x < minx || o.x > maxx) return false; }   // X 軸に平行
    else {
        const f32 inv = 1.0f / d.x;
        f32 t1 = (minx - o.x) * inv, t2 = (maxx - o.x) * inv;
        FVec2 face{ inv < 0.0f ? 1.0f : -1.0f, 0.0f };
        if (t1 > t2) { const f32 tmp = t1; t1 = t2; t2 = tmp; face = FVec2{ -face.x, 0.0f }; }
        if (t1 > tmin) { tmin = t1; nmin = face; }
        if (t2 < tmax) tmax = t2;
    }
    if (d.y > -1e-9f && d.y < 1e-9f) { if (o.y < miny || o.y > maxy) return false; }   // Y 軸に平行
    else {
        const f32 inv = 1.0f / d.y;
        f32 t1 = (miny - o.y) * inv, t2 = (maxy - o.y) * inv;
        FVec2 face{ 0.0f, inv < 0.0f ? 1.0f : -1.0f };
        if (t1 > t2) { const f32 tmp = t1; t1 = t2; t2 = tmp; face = FVec2{ 0.0f, -face.y }; }
        if (t1 > tmin) { tmin = t1; nmin = face; }
        if (t2 < tmax) tmax = t2;
    }
    if (tmax < tmin || tmax < 0.0f) return false;       // miss / 背後
    tOut = (tmin >= 0.0f) ? tmin : 0.0f;
    n    = (tmin >= 0.0f) ? nmin : FVec2{ 0.0f, 0.0f };  // 始点が内側なら法線は不定 (0)
    return true;
}

} // namespace

i32 CRigidWorld2D::QueryPoint(FVec2 p) const noexcept {
    for (u32 i = 0; i < m_Bodies.Num(); ++i) {
        if (m_Bodies[i].active && PointInBody(m_Bodies[i], p)) return static_cast<i32>(i);
    }
    return -1;
}

FRayHit2D CRigidWorld2D::Raycast(FVec2 origin, FVec2 dir, f32 max_dist) const noexcept {
    FRayHit2D best;
    const f32 dl = VLen(dir);
    if (dl < 1e-9f) return best;
    const FVec2 d = VScale(dir, 1.0f / dl);
    f32 bestT = max_dist;
    for (u32 i = 0; i < m_Bodies.Num(); ++i) {
        const FRigidBodyState& b = m_Bodies[i];
        if (!b.active) continue;
        f32 t = 0.0f; FVec2 nrm{ 0.0f, 0.0f };
        const bool hit = b.is_circle ? RayCircle(origin, d, b, t, nrm)
                                     : RayAabb(origin, d, b, t, nrm);
        if (hit && t >= 0.0f && t <= bestT) {
            bestT = t;
            best.hit = true; best.distance = t; best.body = i; best.normal = nrm;
            best.point = FVec2{ origin.x + d.x * t, origin.y + d.y * t };
        }
    }
    return best;
}

u32 CRigidWorld2D::OverlapCircle(FVec2 center, f32 radius, u32* out_indices, u32 cap) const noexcept {
    u32 n = 0u;
    for (u32 i = 0; i < m_Bodies.Num(); ++i) {
        const FRigidBodyState& b = m_Bodies[i];
        if (!b.active) continue;
        bool overlap;
        if (b.is_circle) {
            const FVec2 d = VSub(b.pos, center);
            const f32 rr = radius + b.radius;
            overlap = VDot(d, d) <= rr * rr;
        } else {
            const f32 cx = Clampf(center.x, b.pos.x - b.half.x, b.pos.x + b.half.x);
            const f32 cy = Clampf(center.y, b.pos.y - b.half.y, b.pos.y + b.half.y);
            const FVec2 d = VSub(FVec2{ cx, cy }, center);
            overlap = VDot(d, d) <= radius * radius;
        }
        if (overlap) { if (out_indices != nullptr && n < cap) out_indices[n] = i; ++n; }
    }
    return n;
}

u32 CRigidWorld2D::OverlapBody(u32 index, u32* out_indices, u32 cap) const noexcept {
    if (index >= m_Bodies.Num() || !m_Bodies[index].active) return 0u;
    const FRigidBodyState& s = m_Bodies[index];
    u32 n = 0u;
    for (u32 i = 0; i < m_Bodies.Num(); ++i) {
        if (i == index) continue;
        const FRigidBodyState& b = m_Bodies[i];
        if (!b.active) continue;
        if (Collide(s, b).hit) {   // マニフォルドが当たれば重なり
            if (out_indices != nullptr && n < cap) out_indices[n] = i;
            ++n;
        }
    }
    return n;
}

} // namespace acs::game
