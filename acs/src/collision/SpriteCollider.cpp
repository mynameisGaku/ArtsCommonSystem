// SPDX-License-Identifier: Apache-2.0
// 2D スプライトコライダー実装。
#include "collision/SpriteCollider.h"
#include "container/Array.h"
#include "foundation/Error.h"

namespace acs {

namespace {

/** 引数不正 (null/空画像) を表すサブエラーコード。 */
constexpr u16 kSubSpriteColInvalidArg = 411;

/** 不透明境界画素が少なすぎることを表すサブエラーコード。 */
constexpr u16 kSubSpriteColEmpty      = 412;

/**
 * アルファ判定つきの読み取り専用 RGBA8 画像ビュー。
 *
 * @details しきい値 thr 以上のアルファを「不透明」とみなす。範囲外座標は背景扱い。
 */
struct FAlphaImage {
    /** RGBA8 tightly-packed 画素列の先頭 (所有しない)。 */
    const u8* rgba;

    /** 画像の幅 (px)。 */
    u32 w;

    /** 画像の高さ (px)。 */
    u32 h;

    /** 「不透明」とみなすアルファのしきい値。 */
    u8  thr;

    /**
     * 指定画素が不透明かを返す。
     *
     * @param x 画素 X 座標。
     * @param y 画素 Y 座標。
     * @return 範囲内かつ alpha >= thr なら true、範囲外は false (背景扱い)。
     */
    bool Opaque(int x, int y) const noexcept {
        if (x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(h)) return false;
        return rgba[(static_cast<usize>(y) * w + x) * 4u + 3u] >= thr;
    }
};

/**
 * 2D 外積 (a→b) × (a→c) を返す。
 *
 * @details 符号で c が線分 a→b の左右どちらにあるかを示す (Jarvis march の turn 判定用)。
 * @param a 基点。
 * @param b a からの 1 本目の方向の終点。
 * @param c 左右を判定する点。
 * @return 外積のスカラー値 (正で左、負で右、0 で共線)。
 */
ACS_FORCEINLINE f32 Cross2(FVec2 a, FVec2 b, FVec2 c) noexcept {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/**
 * 2 点間の距離の二乗を返す。
 *
 * @param a 点 1。
 * @param b 点 2。
 * @return a と b の距離の二乗。
 */
ACS_FORCEINLINE f32 DistSq(FVec2 a, FVec2 b) noexcept {
    const f32 dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

/**
 * 線分 (a,b) と点 p の垂直距離を返す (Douglas-Peucker 用)。
 *
 * @details 線分が縮退している場合は端点 a からの距離を返す。
 * @param p 距離を測る点。
 * @param a 線分の始点。
 * @param b 線分の終点。
 * @return 点 p から線分 (a,b) への垂直距離。
 */
f32 PerpDist(FVec2 p, FVec2 a, FVec2 b) noexcept {
    const f32 dx = b.x - a.x, dy = b.y - a.y;
    const f32 len2 = dx * dx + dy * dy;
    if (len2 < 1e-12f) return Length(FVec2{ p.x - a.x, p.y - a.y });
    const f32 cross = (p.x - a.x) * dy - (p.y - a.y) * dx;
    return Abs(cross) / Sqrt(len2);
}

} // namespace

/** 凸包・輪郭・境界を破棄して空状態に戻す。 */
void CSpriteCollider::Clear() noexcept {
    m_HullCount    = 0;
    m_OutlineCount = 0;
    m_Bounds       = FAabb2{};
}

/** RGBA8 画像のアルファから凸包・輪郭・AABB を構築する。 */
TResult<void> CSpriteCollider::BuildFromAlpha(const u8* rgba, u32 width, u32 height,
                                              u8 alpha_threshold, f32 simplify_epsilon) noexcept {
    Clear();
    if (!rgba || width == 0 || height == 0) {
        return ACS_ERR(Generic, kSubSpriteColInvalidArg, "CSpriteCollider: null/empty image");
    }
    const FAlphaImage img{ rgba, width, height, alpha_threshold };

    // 1. 境界画素 (不透明 かつ 4 近傍に背景/外がある) を収集
    TArray<FVec2> boundary;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const int xi = static_cast<int>(x), yi = static_cast<int>(y);
            if (!img.Opaque(xi, yi)) continue;
            if (!img.Opaque(xi - 1, yi) || !img.Opaque(xi + 1, yi) ||
                !img.Opaque(xi, yi - 1) || !img.Opaque(xi, yi + 1)) {
                boundary.Add(FVec2{ static_cast<f32>(x), static_cast<f32>(y) });
            }
        }
    }
    if (boundary.Num() < 3) {
        return ACS_ERR(Generic, kSubSpriteColEmpty, "CSpriteCollider: too few opaque boundary pixels");
    }
    const u32 bn = static_cast<u32>(boundary.Num());

    // 2. 凸包 (Jarvis march、順序非依存で堅牢)
    u32 start = 0;
    for (u32 i = 1; i < bn; ++i) {
        if (boundary[i].x < boundary[start].x ||
            (boundary[i].x == boundary[start].x && boundary[i].y < boundary[start].y)) {
            start = i;
        }
    }
    u32 cur = start;
    u32 guard = 0;
    do {
        if (m_HullCount < kMaxVertices) m_Hull[m_HullCount++] = boundary[cur];
        u32 next = (cur == 0) ? 1u : 0u;
        for (u32 i = 0; i < bn; ++i) {
            if (i == cur) continue;
            const f32 o = Cross2(boundary[cur], boundary[next], boundary[i]);
            if (next == cur || o < 0.0f ||
                (o == 0.0f && DistSq(boundary[cur], boundary[i]) >
                              DistSq(boundary[cur], boundary[next]))) {
                next = i;
            }
        }
        cur = next;
        if (++guard > bn + 2) break;          // 安全網
    } while (cur != start && m_HullCount < kMaxVertices);

    // 3. 輪郭トレース (Moore 近傍、時計回り) → Douglas-Peucker 簡略化
    static const int OFF[8][2] = {
        {1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}  // E,SE,S,SW,W,NW,N,NE
    };
    // 開始 = scan 順で最初の不透明画素
    int sx = -1, sy = -1;
    for (u32 y = 0; y < height && sx < 0; ++y) {
        for (u32 x = 0; x < width; ++x) {
            if (img.Opaque(static_cast<int>(x), static_cast<int>(y))) {
                sx = static_cast<int>(x); sy = static_cast<int>(y); break;
            }
        }
    }
    TArray<FVec2> raw;
    if (sx >= 0) {
        int px = sx, py = sy;     // 現在の境界画素
        int bx = sx - 1, by = sy; // backtrack (背景側)
        const u32 max_iter = width * height * 4u + 8u;
        u32 iter = 0;
        raw.Add(FVec2{ static_cast<f32>(px), static_cast<f32>(py) });
        while (iter++ < max_iter) {
            // backtrack 方向を求める
            int d = 4;  // 既定 W
            for (int k = 0; k < 8; ++k) {
                if (px + OFF[k][0] == bx && py + OFF[k][1] == by) { d = k; break; }
            }
            bool found = false;
            for (int k = 1; k <= 8; ++k) {
                const int dd = (d + k) & 7;
                const int cx = px + OFF[dd][0];
                const int cy = py + OFF[dd][1];
                if (img.Opaque(cx, cy)) {
                    bx = px + OFF[(dd + 7) & 7][0];   // 直前に調べた背景セルが次の backtrack
                    by = py + OFF[(dd + 7) & 7][1];
                    px = cx; py = cy;
                    found = true;
                    break;
                }
            }
            if (!found) break;                         // 孤立画素
            if (px == sx && py == sy) break;           // 開始に戻った
            raw.Add(FVec2{ static_cast<f32>(px), static_cast<f32>(py) });
        }
    }

    // Douglas-Peucker (反復、keep フラグ方式)
    if (raw.Num() >= 2) {
        const u32 rn = static_cast<u32>(raw.Num());
        TArray<u8> keep;
        keep.SetNum(rn);
        for (u32 i = 0; i < rn; ++i) keep[i] = 0;
        keep[0] = 1; keep[rn - 1] = 1;
        struct FSeg { u32 lo, hi; };
        TArray<FSeg> stack;
        stack.Add(FSeg{ 0, rn - 1 });
        while (stack.Num() > 0) {
            const FSeg s = stack[stack.Num() - 1];
            stack.Pop();
            if (s.hi <= s.lo + 1) continue;
            f32 max_d = 0.0f; u32 max_i = s.lo;
            for (u32 i = s.lo + 1; i < s.hi; ++i) {
                const f32 dd = PerpDist(raw[i], raw[s.lo], raw[s.hi]);
                if (dd > max_d) { max_d = dd; max_i = i; }
            }
            if (max_d > simplify_epsilon) {
                keep[max_i] = 1;
                stack.Add(FSeg{ s.lo, max_i });
                stack.Add(FSeg{ max_i, s.hi });
            }
        }
        for (u32 i = 0; i < rn && m_OutlineCount < kMaxVertices; ++i) {
            if (keep[i]) m_Outline[m_OutlineCount++] = raw[i];
        }
    }
    // 輪郭が取れなければ凸包で代用。
    if (m_OutlineCount < 3) {
        m_OutlineCount = 0;
        for (u32 i = 0; i < m_HullCount && i < kMaxVertices; ++i) m_Outline[m_OutlineCount++] = m_Hull[i];
    }

    // 4. AABB (凸包から)
    FVec2 mn{ boundary[0].x, boundary[0].y };
    FVec2 mx = mn;
    for (u32 i = 1; i < bn; ++i) {
        if (boundary[i].x < mn.x) mn.x = boundary[i].x;
        if (boundary[i].y < mn.y) mn.y = boundary[i].y;
        if (boundary[i].x > mx.x) mx.x = boundary[i].x;
        if (boundary[i].y > mx.y) mx.y = boundary[i].y;
    }
    m_Bounds = FAabb2::FromMinMax(mn, mx);
    return Ok();
}

/** 凸包を ConvexPoly2 (物理用) に変換する。 */
FConvexPoly2 CSpriteCollider::HullPolygon() const noexcept {
    FConvexPoly2 poly;
    const u32 n = m_HullCount;
    if (n == 0) return poly;
    if (n <= FConvexPoly2::kMaxVerts) {
        for (u32 i = 0; i < n; ++i) poly.Add(m_Hull[i]);
    } else {
        for (u32 i = 0; i < FConvexPoly2::kMaxVerts; ++i) {
            poly.Add(m_Hull[(i * n) / FConvexPoly2::kMaxVerts]);
        }
    }
    return poly;
}

/** 点が簡略化済み輪郭ポリゴンの内側かを判定する (ray-crossing、凹形状対応)。 */
bool CSpriteCollider::ContainsPoint(FVec2 p) const noexcept {
    const u32 n = m_OutlineCount;
    if (n < 3) return false;
    bool inside = false;
    for (u32 i = 0, j = n - 1; i < n; j = i++) {
        const FVec2 a = m_Outline[i];
        const FVec2 b = m_Outline[j];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
            inside = !inside;
        }
    }
    return inside;
}

} // namespace acs
