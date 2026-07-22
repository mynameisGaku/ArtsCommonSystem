// SPDX-License-Identifier: Apache-2.0
// 2D 衝突判定プリミティブ（AABB / 円 / 線分 / 点）
//
// ヘッダオンリー、ゲーム実装に直結する最小集合：
//   - 形状定義: FAabb2, FCircle
//   - 重なり判定: Intersect(A, B)
//   - 押し出しベクトル: Resolve(A, B)
//   - レイキャスト: RaycastAabb / RaycastCircle
//
// 使い方:
//   FAabb2 player{ {x, y}, {w, h} };
//   FAabb2 wall  { {0, 0}, {100, 8} };
//   if (Intersect(player, wall)) { /* 衝突 */ }
//
//   FCircle a{{px, py}, 16};
//   FCircle b{{ex, ey}, 12};
//   FVec2 push;
//   if (Resolve(a, b, push)) { player.center += push; }
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"

namespace acs {

/**
 * 軸並行境界ボックス (中心 + 半サイズで表す)。
 */
struct FAabb2 {
    /** ボックス中心。 */
    FVec2 center;

    /** 半サイズ (w/2, h/2)。 */
    FVec2 half_size;

    /** 中心・半サイズとも未初期化のまま構築する。 */
    constexpr FAabb2() noexcept = default;

    /**
     * 中心と半サイズを指定して構築する。
     *
     * @param c 中心。
     * @param hs 半サイズ。
     */
    constexpr FAabb2(FVec2 c, FVec2 hs) noexcept : center(c), half_size(hs) {}

    /**
     * 最小・最大座標から構築する。
     *
     * @param min 左上 (min_x, min_y)。
     * @param max 右下 (max_x, max_y)。
     * @return min〜max を覆う AABB。
     */
    static constexpr FAabb2 FromMinMax(FVec2 min, FVec2 max) noexcept {
        const FVec2 c{ (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f };
        const FVec2 hs{ (max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f };
        return { c, hs };
    }

    /**
     * 左上座標とサイズから構築する (スプライト系で便利)。
     *
     * @param tl 左上座標。
     * @param size 幅・高さ。
     * @return tl を左上に size の大きさを持つ AABB。
     */
    static constexpr FAabb2 FromTopLeftSize(FVec2 tl, FVec2 size) noexcept {
        const FVec2 c{ tl.x + size.x * 0.5f, tl.y + size.y * 0.5f };
        const FVec2 hs{ size.x * 0.5f, size.y * 0.5f };
        return { c, hs };
    }

    /**
     * 最小座標 (左上) を返す。
     *
     * @return (center - half_size)。
     */
    constexpr FVec2 Min() const noexcept { return { center.x - half_size.x, center.y - half_size.y }; }

    /**
     * 最大座標 (右下) を返す。
     *
     * @return (center + half_size)。
     */
    constexpr FVec2 Max() const noexcept { return { center.x + half_size.x, center.y + half_size.y }; }
};

/**
 * 円 (中心 + 半径)。
 */
struct FCircle {
    /** 円の中心。 */
    FVec2 center;

    /** 半径。 */
    f32  radius = 0.0f;
};

/**
 * 凸多角形コライダー (頂点数は固定上限まで)。
 *
 * @details
 * 巻き順は時計回り/反時計回りどちらでも可だが凸であることが前提。
 * スプライトの凸包コライダー等を載せる用途。
 */
struct FConvexPoly2 {
    /** 保持できる頂点数の上限。 */
    static constexpr u32 kMaxVerts = 16;

    /** 頂点配列 (有効なのは先頭 count 個)。 */
    FVec2 verts[kMaxVerts]{};

    /** 有効頂点数。 */
    u32   count = 0;

    /** 全頂点をクリアする (count を 0 に戻す)。 */
    void Clear() noexcept { count = 0; }

    /**
     * 頂点を末尾に追加する (上限超過時は無視)。
     *
     * @param v 追加する頂点。
     */
    void Add(FVec2 v) noexcept { if (count < kMaxVerts) verts[count++] = v; }
};

/**
 * 凸多角形を包む AABB を返す。
 *
 * @param p 入力凸多角形。
 * @return 全頂点を覆う AABB (頂点 0 個なら既定 AABB)。
 */
ACS_FORCEINLINE FAabb2 AabbOf(const FConvexPoly2& p) noexcept {
    if (p.count == 0) return FAabb2{};
    FVec2 mn = p.verts[0], mx = p.verts[0];
    for (u32 i = 1; i < p.count; ++i) {
        if (p.verts[i].x < mn.x) mn.x = p.verts[i].x;
        if (p.verts[i].y < mn.y) mn.y = p.verts[i].y;
        if (p.verts[i].x > mx.x) mx.x = p.verts[i].x;
        if (p.verts[i].y > mx.y) mx.y = p.verts[i].y;
    }
    return FAabb2::FromMinMax(mn, mx);
}

/**
 * レイ (始点 + 方向)。
 *
 * @details direction は必ずしも正規化されている必要はないが、t の解釈は
 * 方向ベクトルの長さに依存する。
 */
struct FRay2 {
    /** レイの始点。 */
    FVec2 origin;

    /** レイの方向 (正規化任意)。 */
    FVec2 direction;
};

/**
 * レイキャストの結果。
 */
struct FRayHit2 {
    /** 命中したか。 */
    bool hit  = false;

    /** 命中点の媒介変数 (origin + direction * t が命中点)。 */
    f32  t    = 0.0f;

    /** 命中点座標。 */
    FVec2 point;

    /** 命中面の外向き法線。 */
    FVec2 normal;
};

/**
 * 点が AABB に含まれるかを返す。
 *
 * @param a 対象 AABB。
 * @param p 判定する点。
 * @return p が a の内部 (境界含む) なら true。
 */
ACS_FORCEINLINE bool Contains(const FAabb2& a, FVec2 p) noexcept {
    return Abs(p.x - a.center.x) <= a.half_size.x &&
           Abs(p.y - a.center.y) <= a.half_size.y;
}

/**
 * 点が円に含まれるかを返す。
 *
 * @param c 対象の円。
 * @param p 判定する点。
 * @return p が c の内部 (境界含む) なら true。
 */
ACS_FORCEINLINE bool Contains(const FCircle& c, FVec2 p) noexcept {
    const f32 dx = p.x - c.center.x;
    const f32 dy = p.y - c.center.y;
    return dx*dx + dy*dy <= c.radius * c.radius;
}

/**
 * 2 つの AABB が重なるかを返す。
 *
 * @param a AABB その 1。
 * @param b AABB その 2。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FAabb2& a, const FAabb2& b) noexcept {
    return Abs(a.center.x - b.center.x) <= (a.half_size.x + b.half_size.x) &&
           Abs(a.center.y - b.center.y) <= (a.half_size.y + b.half_size.y);
}

/**
 * 2 つの円が重なるかを返す。
 *
 * @param a 円その 1。
 * @param b 円その 2。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FCircle& a, const FCircle& b) noexcept {
    const f32 dx = a.center.x - b.center.x;
    const f32 dy = a.center.y - b.center.y;
    const f32 r  = a.radius + b.radius;
    return dx*dx + dy*dy <= r * r;
}

/**
 * AABB と円が重なるかを返す。
 *
 * @details AABB 上の最近傍点が円内にあるかで判定する。
 * @param a 対象 AABB。
 * @param c 対象の円。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FAabb2& a, const FCircle& c) noexcept {
    // AABB 上の最近傍点が円の中に入っているか
    const f32 cx = c.center.x < a.Min().x ? a.Min().x : (c.center.x > a.Max().x ? a.Max().x : c.center.x);
    const f32 cy = c.center.y < a.Min().y ? a.Min().y : (c.center.y > a.Max().y ? a.Max().y : c.center.y);
    const f32 dx = c.center.x - cx;
    const f32 dy = c.center.y - cy;
    return dx*dx + dy*dy <= c.radius * c.radius;
}

/**
 * 円と AABB が重なるかを返す (引数順を入れ替えた利便オーバーロード)。
 *
 * @param c 対象の円。
 * @param a 対象 AABB。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FCircle& c, const FAabb2& a) noexcept { return Intersect(a, c); }

/**
 * 点が凸多角形の内部にあるかを返す。
 *
 * @details 全エッジで点が同じ側にあれば内側。巻き順は問わない。
 * @param poly 対象の凸多角形。
 * @param p 判定する点。
 * @return 内部 (境界含む) なら true。頂点数 3 未満は false。
 */
ACS_FORCEINLINE bool Contains(const FConvexPoly2& poly, FVec2 p) noexcept {
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

/** SAT 用の凸多角形射影ヘルパ (内部実装用)。 */
namespace poly_detail {

/**
 * 凸多角形を軸に射影して区間 [mn, mx] を求める。
 *
 * @param p 入力凸多角形。
 * @param axis 射影軸。
 * @param mn 射影区間の最小値 (出力)。
 * @param mx 射影区間の最大値 (出力)。
 */
ACS_FORCEINLINE void ProjectPoly(const FConvexPoly2& p, FVec2 axis, f32& mn, f32& mx) noexcept {
    mn = mx = p.verts[0].x * axis.x + p.verts[0].y * axis.y;
    for (u32 i = 1; i < p.count; ++i) {
        const f32 d = p.verts[i].x * axis.x + p.verts[i].y * axis.y;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
    }
}

/**
 * 指定軸が 2 つの凸多角形を分離するかを返す (SAT の 1 軸テスト)。
 *
 * @param a 凸多角形その 1。
 * @param b 凸多角形その 2。
 * @param axis テストする分離軸。
 * @return 射影区間が重ならない (分離軸である) なら true。
 */
ACS_FORCEINLINE bool AxisSeparates(const FConvexPoly2& a, const FConvexPoly2& b, FVec2 axis) noexcept {
    f32 amn, amx, bmn, bmx;
    ProjectPoly(a, axis, amn, amx);
    ProjectPoly(b, axis, bmn, bmx);
    return amx < bmn || bmx < amn;
}
} // namespace poly_detail

/**
 * 2 つの凸多角形が重なるかを返す (分離軸定理)。
 *
 * @param a 凸多角形その 1。
 * @param b 凸多角形その 2。
 * @return 重なっていれば true。いずれか頂点数 3 未満なら false。
 */
ACS_FORCEINLINE bool Intersect(const FConvexPoly2& a, const FConvexPoly2& b) noexcept {
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

/**
 * 凸多角形と円が重なるかを返す。
 *
 * @details 円中心が内側、またはいずれかのエッジに半径以内で接触すれば重なる。
 * @param poly 対象の凸多角形。
 * @param c 対象の円。
 * @return 重なっていれば true。頂点数 3 未満は false。
 */
ACS_FORCEINLINE bool Intersect(const FConvexPoly2& poly, const FCircle& c) noexcept {
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

/**
 * 円と凸多角形が重なるかを返す (引数順を入れ替えた利便オーバーロード)。
 *
 * @param c 対象の円。
 * @param poly 対象の凸多角形。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FCircle& c, const FConvexPoly2& poly) noexcept { return Intersect(poly, c); }

/**
 * AABB を 4 頂点の凸多角形に変換する。
 *
 * @param box 入力 AABB。
 * @return box と同じ矩形を表す凸多角形。
 */
ACS_FORCEINLINE FConvexPoly2 ToPoly(const FAabb2& box) noexcept {
    const FVec2 mn = box.Min(), mx = box.Max();
    FConvexPoly2 p;
    p.Add(FVec2{ mn.x, mn.y }); p.Add(FVec2{ mx.x, mn.y });
    p.Add(FVec2{ mx.x, mx.y }); p.Add(FVec2{ mn.x, mx.y });
    return p;
}

/**
 * 凸多角形と AABB が重なるかを返す (AABB を凸多角形化して SAT)。
 *
 * @param poly 対象の凸多角形。
 * @param box 対象 AABB。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FConvexPoly2& poly, const FAabb2& box) noexcept {
    return Intersect(poly, ToPoly(box));
}

/**
 * AABB と凸多角形が重なるかを返す (引数順を入れ替えた利便オーバーロード)。
 *
 * @param box 対象 AABB。
 * @param poly 対象の凸多角形。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FAabb2& box, const FConvexPoly2& poly) noexcept { return Intersect(poly, box); }

/**
 * 凸多角形の重心 (頂点平均) を返す。
 *
 * @param p 入力凸多角形。
 * @return 全頂点の平均座標 (頂点 0 個なら原点)。
 */
ACS_FORCEINLINE FVec2 Centroid(const FConvexPoly2& p) noexcept {
    if (p.count == 0) return FVec2{ 0, 0 };
    FVec2 c{ 0, 0 };
    for (u32 i = 0; i < p.count; ++i) { c.x += p.verts[i].x; c.y += p.verts[i].y; }
    return FVec2{ c.x / static_cast<f32>(p.count), c.y / static_cast<f32>(p.count) };
}

/**
 * 2 つの凸多角形の最小押し出しベクトル (MTV) を求める。
 *
 * @details
 * 分離軸定理で最小重なり量の軸を探し、A を B から離す向きの push を返す。
 * 両ポリゴンが退化していて有効な分離軸が見つからない場合は解決不能として false。
 * @param A 押し出す対象の凸多角形。
 * @param B 押し出しの基準となる凸多角形。
 * @param push A を B から離す最小移動ベクトル (出力)。
 * @return 重なっていて解決できたら true。
 */
ACS_FORCEINLINE bool Resolve(const FConvexPoly2& A, const FConvexPoly2& B, FVec2& push) noexcept {
    if (A.count < 3 || B.count < 3) return false;
    f32 min_overlap = 3.4028235e38f;
    FVec2 best_axis{ 0, 0 };
    for (int side = 0; side < 2; ++side) {
        const FConvexPoly2& P = (side == 0) ? A : B;
        for (u32 i = 0; i < P.count; ++i) {
            const FVec2 e = P.verts[(i + 1) % P.count] - P.verts[i];
            const FVec2 axis = Normalize(FVec2{ -e.y, e.x });
            if (axis.x == 0.0f && axis.y == 0.0f) continue;
            f32 amn, amx, bmn, bmx;
            poly_detail::ProjectPoly(A, axis, amn, amx);
            poly_detail::ProjectPoly(B, axis, bmn, bmx);
            const f32 overlap = (amx < bmx ? amx : bmx) - (amn > bmn ? amn : bmn);
            if (overlap <= 0.0f) return false;       // 分離軸あり → 重ならない
            if (overlap < min_overlap) { min_overlap = overlap; best_axis = axis; }
        }
    }
    // 有効な分離軸が 1 つも見つからなかった (両ポリゴンが退化していて全エッジ法線が
    // ゼロ等) 場合、best_axis={0,0}/min_overlap=∞ のまま。push={0,0} で true を返すと
    // 呼び出し側は「解決済みだが移動量ゼロ」と解釈してオブジェクトが重なったまま固着する。
    // この場合は解決不能として false を返す。
    if (best_axis.x == 0.0f && best_axis.y == 0.0f) return false;
    const FVec2 d = Centroid(A) - Centroid(B);
    if (best_axis.x * d.x + best_axis.y * d.y < 0.0f) best_axis = FVec2{ -best_axis.x, -best_axis.y };
    push = FVec2{ best_axis.x * min_overlap, best_axis.y * min_overlap };
    return true;
}

/**
 * 円を凸多角形から押し出す最小ベクトル (MTV) を求める。
 *
 * @details
 * 各エッジ法線に加え「最近接頂点 → 円中心」軸 (角衝突用) も候補にして
 * 最小重なり量の軸を探す。退化で有効軸が無ければ解決不能として false。
 * @param c 押し出す対象の円。
 * @param P 押し出しの基準となる凸多角形。
 * @param push 円を P から離す最小移動ベクトル (出力)。
 * @return 重なっていて解決できたら true。
 */
ACS_FORCEINLINE bool Resolve(const FCircle& c, const FConvexPoly2& P, FVec2& push) noexcept {
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

    // 有効な軸が無い (退化) 場合は push={0,0} の偽 true を避けて解決不能を返す。
    if (best_axis.x == 0.0f && best_axis.y == 0.0f) return false;
    const FVec2 d = c.center - Centroid(P);
    if (best_axis.x * d.x + best_axis.y * d.y < 0.0f) best_axis = FVec2{ -best_axis.x, -best_axis.y };
    push = FVec2{ best_axis.x * min_overlap, best_axis.y * min_overlap };  // 円を押し出す向き
    return true;
}

/**
 * 2 つの円の押し出しベクトル (A を B から離す最小ベクトル) を求める。
 *
 * @param a 押し出す対象の円。
 * @param b 押し出しの基準となる円。
 * @param push a を動かすべき方向 × 距離 (出力)。同心の場合は +X 方向に押す。
 * @return 衝突していたら true。
 */
ACS_FORCEINLINE bool Resolve(const FCircle& a, const FCircle& b, FVec2& push) noexcept {
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

/**
 * 2 つの AABB の押し出しベクトル (最小貫通軸へ押す) を求める。
 *
 * @param a 押し出す対象の AABB。
 * @param b 押し出しの基準となる AABB。
 * @param push a を動かすべき方向 × 距離 (貫通の浅い軸方向、出力)。
 * @return 衝突していたら true。
 */
ACS_FORCEINLINE bool Resolve(const FAabb2& a, const FAabb2& b, FVec2& push) noexcept {
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

/**
 * AABB と無限長レイの交差を求める (slab method)。
 *
 * @param ray 入力レイ。
 * @param a 対象 AABB。
 * @param t_max 探索する t の上限 (既定は実質無限大)。
 * @return 命中情報。命中時は t・point・命中軸の法線が入る。
 */
ACS_FORCEINLINE FRayHit2 RaycastAabb(const FRay2& ray, const FAabb2& a,
                                    f32 t_max = 3.4028235e38f) noexcept {
    FRayHit2 r{};
    const FVec2 mn = a.Min();
    const FVec2 mx = a.Max();
    const f32 inv_dx = ray.direction.x != 0.0f ? 1.0f / ray.direction.x : 1e30f;
    const f32 inv_dy = ray.direction.y != 0.0f ? 1.0f / ray.direction.y : 1e30f;

    const f32 t1 = (mn.x - ray.origin.x) * inv_dx;
    const f32 t2 = (mx.x - ray.origin.x) * inv_dx;
    const f32 t3 = (mn.y - ray.origin.y) * inv_dy;
    const f32 t4 = (mx.y - ray.origin.y) * inv_dy;

    const f32 tmin_x = t1 < t2 ? t1 : t2;
    const f32 tmax_x = t1 > t2 ? t1 : t2;
    const f32 tmin_y = t3 < t4 ? t3 : t4;
    const f32 tmax_y = t3 > t4 ? t3 : t4;

    const f32 tmin = tmin_x > tmin_y ? tmin_x : tmin_y;
    const f32 tmax = tmax_x < tmax_y ? tmax_x : tmax_y;

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

/**
 * 円とレイの交差を求める (二次方程式の最近接解)。
 *
 * @param ray 入力レイ。
 * @param c 対象の円。
 * @param t_max 探索する t の上限 (既定は実質無限大)。
 * @return 命中情報。半径 0 以下は常に非命中。
 */
ACS_FORCEINLINE FRayHit2 RaycastCircle(const FRay2& ray, const FCircle& c,
                                      f32 t_max = 3.4028235e38f) noexcept {
    FRayHit2 r{};
    if (c.radius <= 0.0f) return r;  // 半径 0 以下はヒット面を持たない (後段 1/radius の inf 法線を回避)
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

/**
 * レイと凸多角形の交差を求める。
 *
 * @details 各エッジ線分との交差のうち最近接 (最小 t) を返す。法線はエッジ法線。
 * @param ray 入力レイ。
 * @param poly 対象の凸多角形。
 * @param t_max 探索する t の上限 (既定は実質無限大)。
 * @return 命中情報。命中時はレイ側を向いたエッジ法線が入る。
 */
ACS_FORCEINLINE FRayHit2 RaycastConvexPoly2(const FRay2& ray, const FConvexPoly2& poly,
                                           f32 t_max = 3.4028235e38f) noexcept {
    FRayHit2 r{};
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

/**
 * 有向境界ボックス (回転できる矩形コライダー)。
 *
 * @details
 * 中心 + 半サイズ + 回転角 (ラジアン、反時計回り +X→+Y) で表す。幾何的には
 * 4 頂点の凸多角形なので、判定は ToPoly() で既存の凸ポリゴン SAT
 * (Intersect / Resolve / RaycastConvexPoly2) に委譲する。点内外判定だけは
 * OBB ローカル空間の軸並行比較で直接行う (高速・分岐少)。
 */
struct FObb2 {
    /** ボックス中心。 */
    FVec2 center;

    /** ローカル軸での半サイズ (w/2, h/2)。 */
    FVec2 half_size;

    /** 回転角 (ラジアン)。 */
    f32   rotation = 0.0f;

    /** 中心原点・半サイズ 0・回転 0 で構築する。 */
    constexpr FObb2() noexcept = default;

    /**
     * 中心・半サイズ・回転角を指定して構築する。
     *
     * @param c 中心。
     * @param hs ローカル軸での半サイズ。
     * @param rot 回転角 (ラジアン)。
     */
    FObb2(FVec2 c, FVec2 hs, f32 rot) noexcept : center(c), half_size(hs), rotation(rot) {}

    /**
     * 回転後のローカル X 軸 (単位ベクトル) を返す。
     *
     * @return (cos θ, sin θ)。
     */
    FVec2 AxisX() const noexcept { return FVec2{ Cos(rotation),  Sin(rotation) }; }

    /**
     * 回転後のローカル Y 軸 (単位ベクトル) を返す。
     *
     * @return (-sin θ, cos θ)。
     */
    FVec2 AxisY() const noexcept { return FVec2{ -Sin(rotation), Cos(rotation) }; }
};

/**
 * OBB を 4 頂点の凸多角形に変換する。
 *
 * @param o 入力 OBB。
 * @return 反時計回り (-X-Y, +X-Y, +X+Y, -X+Y) 順の凸多角形。
 */
ACS_FORCEINLINE FConvexPoly2 ToPoly(const FObb2& o) noexcept {
    const FVec2 ax = o.AxisX();
    const FVec2 ay = o.AxisY();
    const FVec2 hx{ ax.x * o.half_size.x, ax.y * o.half_size.x };
    const FVec2 hy{ ay.x * o.half_size.y, ay.y * o.half_size.y };
    FConvexPoly2 p;
    p.Add(FVec2{ o.center.x - hx.x - hy.x, o.center.y - hx.y - hy.y });
    p.Add(FVec2{ o.center.x + hx.x - hy.x, o.center.y + hx.y - hy.y });
    p.Add(FVec2{ o.center.x + hx.x + hy.x, o.center.y + hx.y + hy.y });
    p.Add(FVec2{ o.center.x - hx.x + hy.x, o.center.y - hx.y + hy.y });
    return p;
}

/**
 * OBB を包む AABB を返す。
 *
 * @param o 入力 OBB。
 * @return o の 4 頂点を覆う AABB。
 */
ACS_FORCEINLINE FAabb2 AabbOf(const FObb2& o) noexcept { return AabbOf(ToPoly(o)); }

/**
 * 点が OBB に含まれるかを返す。
 *
 * @details 点を OBB ローカル空間に射影して軸並行比較する (高速)。
 * @param o 対象 OBB。
 * @param p 判定する点。
 * @return p が o の内部 (境界含む) なら true。
 */
ACS_FORCEINLINE bool Contains(const FObb2& o, FVec2 p) noexcept {
    const FVec2 d{ p.x - o.center.x, p.y - o.center.y };
    const FVec2 ax = o.AxisX(), ay = o.AxisY();
    const f32 lx = d.x * ax.x + d.y * ax.y;     // ローカル X 射影
    const f32 ly = d.x * ay.x + d.y * ay.y;     // ローカル Y 射影
    return Abs(lx) <= o.half_size.x && Abs(ly) <= o.half_size.y;
}

/**
 * 2 つの OBB が重なるかを返す (凸ポリ SAT へ委譲)。
 *
 * @param a OBB その 1。
 * @param b OBB その 2。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FObb2& a, const FObb2& b) noexcept { return Intersect(ToPoly(a), ToPoly(b)); }

/**
 * OBB と AABB が重なるかを返す (凸ポリ SAT へ委譲)。
 *
 * @param o 対象 OBB。
 * @param b 対象 AABB。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FObb2& o, const FAabb2& b) noexcept { return Intersect(ToPoly(o), b); }

/**
 * AABB と OBB が重なるかを返す (引数順を入れ替えた利便オーバーロード)。
 *
 * @param b 対象 AABB。
 * @param o 対象 OBB。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FAabb2& b, const FObb2& o) noexcept { return Intersect(ToPoly(o), b); }

/**
 * OBB と円が重なるかを返す (凸ポリ SAT へ委譲)。
 *
 * @param o 対象 OBB。
 * @param c 対象の円。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FObb2& o, const FCircle& c) noexcept { return Intersect(ToPoly(o), c); }

/**
 * 円と OBB が重なるかを返す (引数順を入れ替えた利便オーバーロード)。
 *
 * @param c 対象の円。
 * @param o 対象 OBB。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FCircle& c, const FObb2& o) noexcept { return Intersect(ToPoly(o), c); }

/**
 * OBB と凸多角形が重なるかを返す (凸ポリ SAT へ委譲)。
 *
 * @param o 対象 OBB。
 * @param p 対象の凸多角形。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FObb2& o, const FConvexPoly2& p) noexcept { return Intersect(ToPoly(o), p); }

/**
 * 凸多角形と OBB が重なるかを返す (引数順を入れ替えた利便オーバーロード)。
 *
 * @param p 対象の凸多角形。
 * @param o 対象 OBB。
 * @return 重なっていれば true。
 */
ACS_FORCEINLINE bool Intersect(const FConvexPoly2& p, const FObb2& o) noexcept { return Intersect(p, ToPoly(o)); }

/**
 * 2 つの OBB の押し出しベクトル (MTV) を求める (凸ポリ Resolve へ委譲)。
 *
 * @param A 押し出す対象の OBB。
 * @param B 押し出しの基準となる OBB。
 * @param push A を B から離す最小移動ベクトル (出力)。
 * @return 衝突していて解決できたら true。
 */
ACS_FORCEINLINE bool Resolve(const FObb2& A, const FObb2& B, FVec2& push) noexcept { return Resolve(ToPoly(A), ToPoly(B), push); }

/**
 * OBB を AABB から押し出す MTV を求める (凸ポリ Resolve へ委譲)。
 *
 * @param A 押し出す対象の OBB。
 * @param B 押し出しの基準となる AABB。
 * @param push A を B から離す最小移動ベクトル (出力)。
 * @return 衝突していて解決できたら true。
 */
ACS_FORCEINLINE bool Resolve(const FObb2& A, const FAabb2& B, FVec2& push) noexcept { return Resolve(ToPoly(A), ToPoly(B), push); }

/**
 * OBB を凸多角形から押し出す MTV を求める (凸ポリ Resolve へ委譲)。
 *
 * @param A 押し出す対象の OBB。
 * @param B 押し出しの基準となる凸多角形。
 * @param push A を B から離す最小移動ベクトル (出力)。
 * @return 衝突していて解決できたら true。
 */
ACS_FORCEINLINE bool Resolve(const FObb2& A, const FConvexPoly2& B, FVec2& push) noexcept { return Resolve(ToPoly(A), B, push); }

/**
 * 凸多角形を OBB から押し出す MTV を求める (凸ポリ Resolve へ委譲)。
 *
 * @param A 押し出す対象の凸多角形。
 * @param B 押し出しの基準となる OBB。
 * @param push A を B から離す最小移動ベクトル (出力)。
 * @return 衝突していて解決できたら true。
 */
ACS_FORCEINLINE bool Resolve(const FConvexPoly2& A, const FObb2& B, FVec2& push) noexcept { return Resolve(A, ToPoly(B), push); }

/**
 * 円を OBB から押し出す MTV を求める (凸ポリ Resolve へ委譲)。
 *
 * @param c 押し出す対象の円。
 * @param o 押し出しの基準となる OBB。
 * @param push 円を o から離す最小移動ベクトル (出力)。
 * @return 衝突していて解決できたら true。
 */
ACS_FORCEINLINE bool Resolve(const FCircle& c, const FObb2& o, FVec2& push) noexcept { return Resolve(c, ToPoly(o), push); }

/**
 * レイと OBB の交差を求める (凸ポリ raycast へ委譲)。
 *
 * @param ray 入力レイ。
 * @param o 対象 OBB。
 * @param t_max 探索する t の上限 (既定は実質無限大)。
 * @return 命中情報。
 */
ACS_FORCEINLINE FRayHit2 RaycastObb2(const FRay2& ray, const FObb2& o,
                                    f32 t_max = 3.4028235e38f) noexcept {
    return RaycastConvexPoly2(ray, ToPoly(o), t_max);
}

} // namespace acs
