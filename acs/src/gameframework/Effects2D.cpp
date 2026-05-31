// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Effects2D.h"
#include "gameframework/Node2D.h"
#include "gameframework/RenderContext.h"
#include "render/SpriteBatch.h"
#include "math/Math.h"

namespace acs::game {

namespace {
// Catmull-Rom 補間: p1,p2 間を t で。p0/p3 は隣接制御点 (端は端点を複製)。
inline FVec2 CatmullRom(FVec2 p0, FVec2 p1, FVec2 p2, FVec2 p3, f32 t) noexcept {
    const f32 t2 = t * t, t3 = t2 * t;
    return FVec2{
        0.5f * ((2.0f*p1.x) + (-p0.x+p2.x)*t + (2.0f*p0.x-5.0f*p1.x+4.0f*p2.x-p3.x)*t2 + (-p0.x+3.0f*p1.x-3.0f*p2.x+p3.x)*t3),
        0.5f * ((2.0f*p1.y) + (-p0.y+p2.y)*t + (2.0f*p0.y-5.0f*p1.y+4.0f*p2.y-p3.y)*t2 + (-p0.y+3.0f*p1.y-3.0f*p2.y+p3.y)*t3)
    };
}
// 点 p から線分 a-b への最短距離。
inline f32 PointSegDist(FVec2 p, FVec2 a, FVec2 b) noexcept {
    const f32 dx = b.x - a.x, dy = b.y - a.y;
    const f32 len2 = dx*dx + dy*dy;
    f32 t = len2 > 1e-8f ? ((p.x-a.x)*dx + (p.y-a.y)*dy) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const f32 cx = a.x + dx*t, cy = a.y + dy*t;
    const f32 ex = p.x-cx, ey = p.y-cy;
    return Sqrt(ex*ex + ey*ey);
}
} // namespace

// ===========================================================================
// FWater2DComponent
// ===========================================================================

// ---- 形状ビルダー共通ヘルパ ----
void FWater2DComponent::PushVert(FVec2 v, f32 edge_dist) noexcept {
    if (m_VCount < kMaxVerts) {
        m_EdgeDist[m_VCount] = edge_dist;   // -1 = builder 未設定 (Finish で境界距離 fallback)
        m_Vert[m_VCount++] = v;
    }
}

void FWater2DComponent::PushTri(u32 a, u32 b, u32 c) noexcept {
    if (m_TCount < kMaxTris && a < m_VCount && b < m_VCount && c < m_VCount) {
        m_Tri[m_TCount * 3 + 0] = static_cast<u16>(a);
        m_Tri[m_TCount * 3 + 1] = static_cast<u16>(b);
        m_Tri[m_TCount * 3 + 2] = static_cast<u16>(c);
        ++m_TCount;
    }
}

void FWater2DComponent::Finish() noexcept {
    if (m_VCount == 0) return;
    m_MinX = m_MaxX = m_Vert[0].x;
    m_MinY = m_MaxY = m_Vert[0].y;
    for (u32 i = 1; i < m_VCount; ++i) {
        if (m_Vert[i].x < m_MinX) m_MinX = m_Vert[i].x;
        if (m_Vert[i].x > m_MaxX) m_MaxX = m_Vert[i].x;
        if (m_Vert[i].y < m_MinY) m_MinY = m_Vert[i].y;
        if (m_Vert[i].y > m_MaxY) m_MaxY = m_Vert[i].y;
    }
    // 「水面らしさ」: bbox 上端 (= min Y、画面上) から band 内を 1→0。
    // 上端の頂点ほど波で揺れ、底の頂点は動かない (どの形状でも自然な水面に)。
    const f32 band = (m_MaxY - m_MinY) * 0.4f + 1e-4f;
    for (u32 i = 0; i < m_VCount; ++i) {
        f32 w = 1.0f - (m_Vert[i].y - m_MinY) / band;
        m_Weight[i] = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    }
    BuildBoundary();
    // builder が edge-dist を埋めていない頂点があれば、境界距離で補完。
    bool unset = false;
    for (u32 i = 0; i < m_VCount; ++i) if (m_EdgeDist[i] < 0.0f) { unset = true; break; }
    if (unset) ComputeEdgeDistFallback();
}

void FWater2DComponent::ComputeEdgeDistFallback() noexcept {
    if (m_BoundaryCount < 2) {
        for (u32 i = 0; i < m_VCount; ++i) if (m_EdgeDist[i] < 0.0f) m_EdgeDist[i] = 0.5f;
        return;
    }
    f32 maxd = 1e-4f;
    f32 dtmp[kMaxVerts];
    for (u32 i = 0; i < m_VCount; ++i) {
        f32 best = 1e9f;
        for (u32 k = 0; k < m_BoundaryCount; ++k) {
            const u32 k2 = (k + 1) % m_BoundaryCount;
            const f32 d = PointSegDist(m_Vert[i], m_Vert[m_Boundary[k]], m_Vert[m_Boundary[k2]]);
            if (d < best) best = d;
        }
        dtmp[i] = best;
        if (best > maxd) maxd = best;
    }
    for (u32 i = 0; i < m_VCount; ++i)
        if (m_EdgeDist[i] < 0.0f) m_EdgeDist[i] = Saturate(dtmp[i] / maxd);
}

// edge-incidence で外周ループを抽出: 1 つの三角形にしか使われていないエッジ =
// 境界エッジ。それらを順に繋いでリング化し、各境界頂点の外向き法線を求める。
// (weight ヒューリスティックと違い、どの形状でも正しく外周を取れる)
void FWater2DComponent::BuildBoundary() noexcept {
    m_BoundaryCount = 0;
    if (m_TCount < 1) return;

    // 1) 無向エッジの出現回数を数える。
    u16 ea[kMaxTris * 3]; u16 eb[kMaxTris * 3]; u8 ec[kMaxTris * 3];
    u32 ne = 0;
    for (u32 t = 0; t < m_TCount; ++t) {
        const u16 tri[3] = { m_Tri[t*3], m_Tri[t*3+1], m_Tri[t*3+2] };
        for (u32 j = 0; j < 3; ++j) {
            u16 a = tri[j], b = tri[(j + 1) % 3];
            if (a > b) { const u16 s = a; a = b; b = s; }
            u32 found = ne;
            for (u32 i = 0; i < ne; ++i) { if (ea[i] == a && eb[i] == b) { found = i; break; } }
            if (found < ne) { ++ec[found]; }
            else if (ne < kMaxTris * 3) { ea[ne] = a; eb[ne] = b; ec[ne] = 1; ++ne; }
        }
    }
    // 2) count==1 のエッジを集めて順序付きリングに繋ぐ。
    u16 ba[kMaxVerts * 2]; u16 bb[kMaxVerts * 2]; u32 nb = 0;
    for (u32 i = 0; i < ne; ++i)
        if (ec[i] == 1 && nb < kMaxVerts * 2) { ba[nb] = ea[i]; bb[nb] = eb[i]; ++nb; }
    if (nb < 3) return;

    u8 used[kMaxVerts * 2] = {};
    const u16 startV = ba[0];
    used[0] = 1;
    m_Boundary[m_BoundaryCount++] = ba[0];
    u16 nextV = bb[0];
    while (nextV != startV && m_BoundaryCount < kMaxVerts) {
        m_Boundary[m_BoundaryCount++] = nextV;
        bool found = false;
        for (u32 i = 0; i < nb; ++i) {
            if (used[i]) continue;
            if (ba[i] == nextV) { used[i] = 1; nextV = bb[i]; found = true; break; }
            if (bb[i] == nextV) { used[i] = 1; nextV = ba[i]; found = true; break; }
        }
        if (!found) break;
    }

    // 3) 外向き単位法線 (隣接 2 エッジの垂線平均、bbox 中心から外を向くよう符号調整)。
    const FVec2 ctr{ (m_MinX + m_MaxX) * 0.5f, (m_MinY + m_MaxY) * 0.5f };
    for (u32 k = 0; k < m_BoundaryCount; ++k) {
        const FVec2 v  = m_Vert[m_Boundary[k]];
        const FVec2 vp = m_Vert[m_Boundary[(k + m_BoundaryCount - 1) % m_BoundaryCount]];
        const FVec2 vn = m_Vert[m_Boundary[(k + 1) % m_BoundaryCount]];
        FVec2 n{ -((v.y - vp.y) + (vn.y - v.y)), ((v.x - vp.x) + (vn.x - v.x)) };
        const f32 len = Sqrt(n.x * n.x + n.y * n.y);
        if (len > 1e-5f) { n.x /= len; n.y /= len; } else { n = FVec2{ 0.0f, -1.0f }; }
        const FVec2 d{ v.x - ctr.x, v.y - ctr.y };
        if (n.x * d.x + n.y * d.y < 0.0f) { n.x = -n.x; n.y = -n.y; }
        m_BoundaryNormal[k] = n;
    }
}

FVec4 FWater2DComponent::DepthColorAt(u32 i) const noexcept {
    // SideView: 上端(weight=1)=浅, 下=深。 TopDown: 縁(edge=0)=浅, 中心(edge=1)=深。
    const f32 dt = (m_Style == EWaterStyle::TopDown) ? m_EdgeDist[i] : (1.0f - m_Weight[i]);
    return FVec4{
        m_ShallowColor.x + (m_DeepColor.x - m_ShallowColor.x) * dt,
        m_ShallowColor.y + (m_DeepColor.y - m_ShallowColor.y) * dt,
        m_ShallowColor.z + (m_DeepColor.z - m_ShallowColor.z) * dt,
        m_ShallowAlpha + (m_DeepAlpha - m_ShallowAlpha) * dt
    };
}

void FWater2DComponent::SetRect(FVec2 center, FVec2 half, u32 top_segments) noexcept {
    m_VCount = 0; m_TCount = 0;
    const f32 w = half.x * 2.0f, h = half.y * 2.0f;
    // 内部頂点を持つ規則格子。cols×rows セル。これで深さ/反射/コースティクスが
    // 滑らかに補間され、扇メッシュの直線スメアが消える。
    u32 cols = m_ResA, rows = m_ResB;
    if (cols == 0 || rows == 0) {
        const f32 cell = ((w > h ? w : h) / 14.0f) + 1e-4f;
        cols = static_cast<u32>(w / cell + 0.5f);
        rows = static_cast<u32>(h / cell + 0.5f);
        if (top_segments > 1 && top_segments < 64 && top_segments > cols) cols = top_segments; // 互換: 細かい上辺指定を尊重
    }
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    while ((cols + 1) * (rows + 1) > kMaxVerts || 2 * cols * rows > kMaxTris) {
        if (cols >= rows && cols > 1) --cols; else if (rows > 1) --rows; else break;
    }
    const f32 lx = center.x - half.x, ty = center.y - half.y;   // 画面上 = min Y
    for (u32 r = 0; r <= rows; ++r) {
        for (u32 c = 0; c <= cols; ++c) {
            const f32 fx = static_cast<f32>(c) / static_cast<f32>(cols);
            const f32 fy = static_cast<f32>(r) / static_cast<f32>(rows);
            f32 ed = fx; if (1.0f - fx < ed) ed = 1.0f - fx;
            if (fy < ed) ed = fy; if (1.0f - fy < ed) ed = 1.0f - fy;
            ed *= 2.0f; if (ed > 1.0f) ed = 1.0f;     // 0 縁 .. 1 中心
            PushVert(FVec2{ lx + w * fx, ty + h * fy }, ed);
        }
    }
    const u32 stride = cols + 1;
    for (u32 r = 0; r < rows; ++r)
        for (u32 c = 0; c < cols; ++c) {
            const u32 i0 = r * stride + c, i1 = i0 + 1, i2 = i0 + stride, i3 = i2 + 1;
            PushTri(i0, i1, i3); PushTri(i0, i3, i2);
        }
    Finish();
}

// 同心リングメッシュ (ellipse / polygon 共通)。中心 + R リング × S サンプル。
// outline(i) が i 番目の外周点を返す。edge_dist = 1-リング比 (中心 1、外周 0)。
void FWater2DComponent::SetEllipse(FVec2 center, f32 rx, f32 ry, u32 segments) noexcept {
    m_VCount = 0; m_TCount = 0;
    u32 S = m_ResA ? m_ResA : segments;
    u32 R = m_ResB ? m_ResB : 4;
    if (S < 3) S = 3;
    if (R < 1) R = 1;
    while (S * R + 1 > kMaxVerts || 2 * S * R > kMaxTris) {
        if (S > 6 && S >= R * 5) --S; else if (R > 1) --R; else break;
    }
    const u32 cIdx = m_VCount; PushVert(center, 1.0f);       // 中心 = 最深
    for (u32 k = 1; k <= R; ++k) {
        const f32 t = static_cast<f32>(k) / static_cast<f32>(R);
        const f32 ed = 1.0f - t;                              // 縁(t=1)→0
        for (u32 i = 0; i < S; ++i) {
            const f32 a = 6.2831853f * (static_cast<f32>(i) / static_cast<f32>(S));
            PushVert(FVec2{ center.x + Cos(a) * rx * t, center.y + Sin(a) * ry * t }, ed);
        }
    }
    const u32 ring0 = cIdx + 1;
    for (u32 i = 0; i < S; ++i) PushTri(cIdx, ring0 + i, ring0 + ((i + 1) % S));
    for (u32 k = 1; k < R; ++k) {
        const u32 a0 = cIdx + 1 + (k - 1) * S, b0 = cIdx + 1 + k * S;
        for (u32 i = 0; i < S; ++i) {
            const u32 i2 = (i + 1) % S;
            PushTri(a0 + i, b0 + i, b0 + i2);
            PushTri(a0 + i, b0 + i2, a0 + i2);
        }
    }
    Finish();
}

void FWater2DComponent::SetPolygon(const FVec2* pts, u32 count) noexcept {
    m_VCount = 0; m_TCount = 0;
    if (pts == nullptr || count < 3) return;
    FVec2 ctr{ 0.0f, 0.0f };
    for (u32 i = 0; i < count; ++i) { ctr.x += pts[i].x; ctr.y += pts[i].y; }
    ctr.x /= static_cast<f32>(count); ctr.y /= static_cast<f32>(count);
    u32 R = m_ResB ? m_ResB : 4;
    if (R < 1) R = 1;
    // 外周点数 × リング数が頂点上限に収まるよう、必要なら外周を間引く。
    u32 stride = 1;
    while ((count / stride) * R + 1 > kMaxVerts && (count / stride) > 3) ++stride;
    while (R > 1 && (count / stride) * R + 1 > kMaxVerts) --R;
    const u32 S = count / stride;
    const u32 cIdx = m_VCount; PushVert(ctr, 1.0f);
    for (u32 k = 1; k <= R; ++k) {
        const f32 t = static_cast<f32>(k) / static_cast<f32>(R);
        const f32 ed = 1.0f - t;
        for (u32 i = 0; i < S; ++i) {
            const FVec2 p = pts[i * stride];
            PushVert(FVec2{ ctr.x + (p.x - ctr.x) * t, ctr.y + (p.y - ctr.y) * t }, ed);
        }
    }
    const u32 ring0 = cIdx + 1;
    for (u32 i = 0; i < S; ++i) PushTri(cIdx, ring0 + i, ring0 + ((i + 1) % S));
    for (u32 k = 1; k < R; ++k) {
        const u32 a0 = cIdx + 1 + (k - 1) * S, b0 = cIdx + 1 + k * S;
        for (u32 i = 0; i < S; ++i) {
            const u32 i2 = (i + 1) % S;
            PushTri(a0 + i, b0 + i, b0 + i2);
            PushTri(a0 + i, b0 + i2, a0 + i2);
        }
    }
    Finish();
}

void FWater2DComponent::SetRiver(const FVec2* path, u32 count, f32 width) noexcept {
    m_VCount = 0; m_TCount = 0;
    if (path == nullptr || count < 2) return;
    const u32 maxPts = kMaxVerts / 3u;
    if (count > maxPts) count = maxPts;
    const f32 hw = width * 0.5f;
    // 3 行リボン: 左岸 / 中央(最深) / 右岸。中央行が内部頂点でコースティクス/深度を担う。
    for (u32 i = 0; i < count; ++i) {
        FVec2 dir;
        if (i == 0)            dir = FVec2{ path[1].x - path[0].x, path[1].y - path[0].y };
        else if (i == count-1) dir = FVec2{ path[i].x - path[i-1].x, path[i].y - path[i-1].y };
        else                   dir = FVec2{ path[i+1].x - path[i-1].x, path[i+1].y - path[i-1].y };
        const f32 len = Sqrt(dir.x * dir.x + dir.y * dir.y);
        const FVec2 n = len > 1e-5f ? FVec2{ -dir.y / len, dir.x / len } : FVec2{ 0.0f, 1.0f };
        PushVert(FVec2{ path[i].x + n.x * hw, path[i].y + n.y * hw }, 0.0f);  // 左岸 edge=0
        PushVert(FVec2{ path[i].x,            path[i].y            }, 1.0f);  // 中央 edge=1
        PushVert(FVec2{ path[i].x - n.x * hw, path[i].y - n.y * hw }, 0.0f);  // 右岸 edge=0
    }
    for (u32 i = 0; i + 1 < count; ++i) {
        const u32 l0 = 3*i, c0 = 3*i+1, r0 = 3*i+2, l1 = 3*i+3, c1 = 3*i+4, r1 = 3*i+5;
        PushTri(l0, c0, l1); PushTri(c0, c1, l1);   // 左半分
        PushTri(c0, r0, c1); PushTri(r0, r1, c1);   // 右半分
    }
    Finish();
}

void FWater2DComponent::SetSplineRiver(const FVec2* control, u32 count, f32 width,
                                       u32 samples_per_seg) noexcept {
    if (control == nullptr || count < 2) return;
    if (samples_per_seg < 1) samples_per_seg = 1;
    FVec2 dense[kMaxVerts / 3u];
    const u32 cap = kMaxVerts / 3u;
    u32 n = 0;
    for (u32 i = 0; i + 1 < count && n < cap; ++i) {
        const FVec2 p0 = control[i == 0 ? 0 : i - 1];
        const FVec2 p1 = control[i];
        const FVec2 p2 = control[i + 1];
        const FVec2 p3 = control[i + 2 < count ? i + 2 : count - 1];
        for (u32 s = 0; s < samples_per_seg && n < cap; ++s)
            dense[n++] = CatmullRom(p0, p1, p2, p3, static_cast<f32>(s) / static_cast<f32>(samples_per_seg));
    }
    if (n < cap) dense[n++] = control[count - 1];        // 終端を含める
    SetRiver(dense, n, width);
}

void FWater2DComponent::SetSplineRegion(const FVec2* control, u32 count,
                                        u32 samples_per_seg) noexcept {
    if (control == nullptr || count < 3) return;
    if (samples_per_seg < 1) samples_per_seg = 1;
    FVec2 dense[kMaxVerts - 1u];
    const u32 cap = kMaxVerts - 1u;
    u32 n = 0;
    for (u32 i = 0; i < count && n < cap; ++i) {         // 閉ループ (添字を wrap)
        const FVec2 p0 = control[(i + count - 1) % count];
        const FVec2 p1 = control[i];
        const FVec2 p2 = control[(i + 1) % count];
        const FVec2 p3 = control[(i + 2) % count];
        for (u32 s = 0; s < samples_per_seg && n < cap; ++s)
            dense[n++] = CatmullRom(p0, p1, p2, p3, static_cast<f32>(s) / static_cast<f32>(samples_per_seg));
    }
    SetPolygon(dense, n);
}

void FWater2DComponent::Disturb(FVec2 world_point, f32 strength) noexcept {
    u32 slot = kMaxRipples;
    f32 oldest = -1.0f;
    for (u32 i = 0; i < kMaxRipples; ++i) {
        if (!m_Ripples[i].active) { slot = i; break; }
        if (m_Ripples[i].time > oldest) { oldest = m_Ripples[i].time; slot = i; }
    }
    if (slot >= kMaxRipples) slot = 0;
    m_Ripples[slot] = Ripple{ world_point, strength, strength, 0.0f, m_RippleSpeed, true };
}

void FWater2DComponent::Disturb(f32 world_x, f32 strength) noexcept {
    Disturb(FVec2{ world_x, SurfaceY() }, strength);   // 後方互換: 水面上の点に輪を立てる
}

bool FWater2DComponent::ContainsPoint(FVec2 world) const noexcept {
    const FVec2 o = Owner().World().position;
    return world.x >= o.x + m_MinX && world.x <= o.x + m_MaxX &&
           world.y >= o.y + m_MinY && world.y <= o.y + m_MaxY;
}

bool FWater2DComponent::ContainsX(f32 world_x) const noexcept {
    const f32 ox = Owner().World().position.x;
    return world_x >= ox + m_MinX && world_x <= ox + m_MaxX;
}

f32 FWater2DComponent::SurfaceY() const noexcept {
    return Owner().World().position.y + m_MinY;   // bbox 上端
}

void FWater2DComponent::SetFlow(FVec2 dir, f32 speed) noexcept {
    const f32 len = Sqrt(dir.x * dir.x + dir.y * dir.y);
    m_FlowDir   = len > 1e-5f ? FVec2{ dir.x / len, dir.y / len } : FVec2{ 1.0f, 0.0f };
    m_FlowSpeed = speed;
}

f32 FWater2DComponent::WaveRadialAt(FVec2 world) const noexcept {
    f32 off;
    if (m_Style == EWaterStyle::TopDown) {
        // 見下ろし: 緩い 2 軸スウェル (方向性のないさざ波)。
        off = m_Amp * 0.5f * (Sin(world.x * 1.3f + m_Time * m_FlowSpeed * 0.5f)
                            + Sin(world.y * 1.1f - m_Time * m_FlowSpeed * 0.4f));
    } else {
        // 横視点: 流れ方向への射影を位相にした進行波。
        const f32 c = world.x * m_FlowDir.x + world.y * m_FlowDir.y;
        off = m_Amp        * Sin(c * 3.0f - m_Time * m_FlowSpeed)
            + m_Amp * 0.4f * Sin(c * 7.0f + m_Time * m_FlowSpeed * 1.3f);
    }
    return off + RippleAt(world);
}

f32 FWater2DComponent::RippleAt(FVec2 world) const noexcept {
    // 触れた点を中心に半径 speed*time の輪が広がる (2D 放射状)。
    const f32 ring = m_RippleWavelength;
    const f32 k    = 6.2831853f / ring;
    const f32 sig2 = 2.0f * (ring * 0.5f) * (ring * 0.5f) + 1e-4f;
    f32 off = 0.0f;
    for (u32 i = 0; i < kMaxRipples; ++i) {
        if (!m_Ripples[i].active) continue;
        const f32 dx = world.x - m_Ripples[i].center.x;
        const f32 dy = world.y - m_Ripples[i].center.y;
        const f32 d  = Sqrt(dx*dx + dy*dy);
        const f32 r  = m_Ripples[i].speed * m_Ripples[i].time;   // 輪の現半径
        const f32 rw = d - r;                                    // 輪 front への符号付き距離
        const f32 env = Exp(-m_Ripples[i].time * 2.0f) * Exp(-(rw*rw) / sig2);
        off += m_Ripples[i].amp0 * Cos(rw * k - m_Ripples[i].time * 9.0f) * env;
    }
    return off;
}

bool FWater2DComponent::AnyRippleActive() const noexcept {
    for (u32 i = 0; i < kMaxRipples; ++i) if (m_Ripples[i].active) return true;
    return false;
}

f32 FWater2DComponent::CausticAt(FVec2 world) const noexcept {
    // 等方的な光の網目: 3 方向 (0°,60°,120°) の等周波スクロール波の |sin| 和。
    // 全 |sin| が同時に 0 に近い格子点 (六角格子) で明るく光り、網目状の「脈」になる。
    // 各方向の位相を別速度でスクロールさせて、網目がゆっくり蠢く。
    const f32 s = m_CausticsScale, v = m_CausticsSpeed, t = m_Time;
    const f32 px = world.x * s, py = world.y * s;
    const f32 a1 = px;
    const f32 a2 = px * 0.5f    + py * 0.8660254f;
    const f32 a3 = px * (-0.5f) + py * 0.8660254f;
    const f32 r = Abs(Sin(a1 + t * v))
                + Abs(Sin(a2 + t * v * 0.9f))
                + Abs(Sin(a3 - t * v * 0.8f));
    f32 cell = Saturate(1.0f - r * 0.55f);   // 細めの脈
    // 別スケールの第 2 層で網目を不規則化 (一様な格子に見せない)。
    const f32 w2 = Abs(Sin((px - py) * 0.6f + t * v * 1.3f));
    cell *= 0.7f + 0.3f * Saturate(1.0f - w2 * 0.9f);
    return cell * cell * (1.6f);   // 鋭く + 明るく
}

void FWater2DComponent::OnUpdate(f32 dt) noexcept {
    m_Time += dt;
    for (u32 i = 0; i < kMaxRipples; ++i) {
        if (!m_Ripples[i].active) continue;
        m_Ripples[i].time += dt;
        m_Ripples[i].amp = m_Ripples[i].amp0 * Exp(-m_Ripples[i].time * 2.0f);
        if (m_Ripples[i].time > 3.0f || m_Ripples[i].amp < 0.002f) m_Ripples[i].active = false;
    }
}

void FWater2DComponent::OnDraw(RenderContext& rc) noexcept {
    if (!rc.HasSprites() || m_TCount == 0) return;
    FSpriteBatch& sb = rc.Sprites();
    const FVec2 o = Owner().World().position;

    const bool topDown = (m_Style == EWaterStyle::TopDown);

    // 波で変位した頂点 world 位置。SideView は上端を上下に揺らす。TopDown は面なので変位無し。
    FVec2 disp[kMaxVerts];
    for (u32 i = 0; i < m_VCount; ++i) {
        const f32 wx = o.x + m_Vert[i].x;
        const f32 wy0 = o.y + m_Vert[i].y;
        const f32 dy = topDown ? 0.0f : (WaveRadialAt(FVec2{ wx, wy0 }) * m_Weight[i]);
        disp[i] = FVec2{ wx, wy0 - dy };
    }

    // 平面反射 (SideView のみ)。細グリッド + 深部フェード/カリングで直線スメアを解消。
    if (!topDown && m_ReflectEnabled && rc.HasReflection()) {
        IRhiTexture& refl = rc.Reflection();
        const FVec2 vc = rc.ViewCenter();
        const f32   vs = rc.ViewScale();
        const f32   hw = static_cast<f32>(rc.Width())  * 0.5f;
        const f32   hh = static_cast<f32>(rc.Height()) * 0.5f;
        const f32   invW = rc.Width()  > 0 ? 1.0f / static_cast<f32>(rc.Width())  : 0.0f;
        const f32   invH = rc.Height() > 0 ? 1.0f / static_cast<f32>(rc.Height()) : 0.0f;
        const f32   surfaceY = o.y + m_MinY;
        FVec2 ruv[kMaxVerts];
        for (u32 i = 0; i < m_VCount; ++i) {
            const f32 mx  = disp[i].x;
            const f32 my  = 2.0f * surfaceY - disp[i].y;                 // 水面で鏡像
            f32 wob = WaveRadialAt(disp[i]) * m_ReflectDistort * vs;      // 波で UV を揺らす
            if (wob >  6.0f) wob =  6.0f;                                 // UV 暴れ防止 (±6px)
            if (wob < -6.0f) wob = -6.0f;
            const f32 sx  = (mx - vc.x) * vs + hw + wob;
            const f32 sy  = (my - vc.y) * vs + hh;
            ruv[i] = FVec2{ sx * invW, sy * invH };
        }
        for (u32 t = 0; t < m_TCount; ++t) {
            const u16 a = m_Tri[t*3], b = m_Tri[t*3+1], c = m_Tri[t*3+2];
            const f32 wavg = (m_Weight[a] + m_Weight[b] + m_Weight[c]) * (1.0f / 3.0f);
            if (m_ReflectFade > 0.0f && wavg < 0.04f) continue;          // 深部三角形はカリング
            const f32 fa = m_ReflectAlpha * (1.0f - m_ReflectFade + m_ReflectFade * wavg);
            const FVec4 rtint{ m_ReflectTint.x, m_ReflectTint.y, m_ReflectTint.z, fa };
            sb.DrawTriangleSub(refl, disp[a].x, disp[a].y, disp[b].x, disp[b].y, disp[c].x, disp[c].y,
                               ruv[a].x, ruv[a].y, ruv[b].x, ruv[b].y, ruv[c].x, ruv[c].y, rtint);
        }
    }

    // 本体: 深さ勾配を頂点カラーで (style 依存: SideView=上下, TopDown=縁→中心)。
    for (u32 t = 0; t < m_TCount; ++t) {
        const u16 a = m_Tri[t*3], b = m_Tri[t*3+1], c = m_Tri[t*3+2];
        sb.DrawTriangleVC(disp[a].x, disp[a].y, disp[b].x, disp[b].y, disp[c].x, disp[c].y,
                          DepthColorAt(a), DepthColorAt(b), DepthColorAt(c));
    }

    if (m_BoundaryCount >= 3) {
        const f32 autoScale = (m_MaxX - m_MinX) + (m_MaxY - m_MinY);

        // 縁取り: 境界から内側へフェードする細い帯 (水際を引き締める)。
        if (m_RimEnabled) {
            const f32 rimW = m_RimWidth > 0.0f ? m_RimWidth : autoScale * 0.008f;
            const FVec4 cEdge{ m_RimColor.x, m_RimColor.y, m_RimColor.z, m_RimAlpha };
            const FVec4 cIn  { m_RimColor.x, m_RimColor.y, m_RimColor.z, 0.0f };
            for (u32 k = 0; k < m_BoundaryCount; ++k) {
                const u32 k2 = (k + 1) % m_BoundaryCount;
                const FVec2 p0 = disp[m_Boundary[k]],  p1 = disp[m_Boundary[k2]];
                const FVec2 n0 = m_BoundaryNormal[k],  n1 = m_BoundaryNormal[k2];
                const FVec2 i0{ p0.x - n0.x * rimW, p0.y - n0.y * rimW };
                const FVec2 i1{ p1.x - n1.x * rimW, p1.y - n1.y * rimW };
                sb.DrawTriangleVC(p0.x, p0.y, p1.x, p1.y, i1.x, i1.y, cEdge, cEdge, cIn);
                sb.DrawTriangleVC(p0.x, p0.y, i1.x, i1.y, i0.x, i0.y, cEdge, cIn, cIn);
            }
        }
        // 全周の白泡: 境界から外側へフェード + 弧長に沿って打ち寄せる波 (lapping)。
        if (m_FoamEnabled) {
            const f32 foamW = m_FoamWidth > 0.0f ? m_FoamWidth : autoScale * 0.02f;
            const f32 kf = 6.2831853f * 3.0f;   // 周回あたり 3 波
            for (u32 k = 0; k < m_BoundaryCount; ++k) {
                const u32 k2 = (k + 1) % m_BoundaryCount;
                const f32 param = static_cast<f32>(k) / static_cast<f32>(m_BoundaryCount);
                const f32 lap = Saturate(0.35f + 0.65f * Sin(param * kf - m_Time * m_FoamSpeed));
                const f32 wob = foamW * (0.7f + 0.5f * lap
                                + 0.15f * Sin(m_Time * m_FoamSpeed * 1.7f + param * 11.0f));
                const FVec2 p0 = disp[m_Boundary[k]],  p1 = disp[m_Boundary[k2]];
                const FVec2 n0 = m_BoundaryNormal[k],  n1 = m_BoundaryNormal[k2];
                const FVec2 q0{ p0.x + n0.x * wob, p0.y + n0.y * wob };
                const FVec2 q1{ p1.x + n1.x * wob, p1.y + n1.y * wob };
                const FVec4 cIn { m_FoamColor.x, m_FoamColor.y, m_FoamColor.z, m_FoamAlpha * lap };
                const FVec4 cOut{ m_FoamColor.x, m_FoamColor.y, m_FoamColor.z, 0.0f };
                sb.DrawTriangleVC(p0.x, p0.y, p1.x, p1.y, q1.x, q1.y, cIn, cIn, cOut);
                sb.DrawTriangleVC(p0.x, p0.y, q1.x, q1.y, q0.x, q0.y, cIn, cOut, cOut);
            }
        }
    }

    // 加算パス: 空色 (TopDown 反射代替) + コースティクス + リップル輪 + きらめき。
    if (m_SkyAlpha > 0.0f || m_CausticsOn || m_GlintsOn || AnyRippleActive()) {
        sb.SetBlendMode(EBlendMode::Additive);
        DrawCaustics(sb, disp);
        if (m_GlintsOn) DrawGlints(sb, disp);
        sb.SetBlendMode(EBlendMode::AlphaBlend);   // 次のコンポーネントのため戻す
    }
}

void FWater2DComponent::DrawCaustics(FSpriteBatch& sb, const FVec2* disp) noexcept {
    // 空色 (薄いフラット加算)。TopDown で平面反射の代わりに水面を少し明るくする。
    if (m_SkyAlpha > 0.0f) {
        const FVec4 sky{ m_SkyTint.x * m_SkyAlpha, m_SkyTint.y * m_SkyAlpha, m_SkyTint.z * m_SkyAlpha, 1.0f };
        for (u32 t = 0; t < m_TCount; ++t) {
            const u16 a = m_Tri[t*3], b = m_Tri[t*3+1], c = m_Tri[t*3+2];
            sb.DrawTriangleVC(disp[a].x, disp[a].y, disp[b].x, disp[b].y, disp[c].x, disp[c].y, sky, sky, sky);
        }
    }
    const bool anyRip = AnyRippleActive();
    if (!m_CausticsOn && !anyRip) return;
    // 頂点ごとの加算カラー (光の網目 + リップルの輪 glow)。alpha で「脈」「輪」だけ光る。
    FVec4 cc[kMaxVerts];
    for (u32 i = 0; i < m_VCount; ++i) {
        f32 kk = 0.0f;
        if (m_CausticsOn) {
            const f32 cell  = CausticAt(disp[i]);
            const f32 shore = 0.4f + 0.6f * Saturate(m_EdgeDist[i] * 2.0f);   // 岸では泡が主役
            kk += cell * m_CausticsIntensity * shore;
        }
        if (anyRip) kk += Abs(RippleAt(disp[i])) * 2.2f;   // 広がる輪 front が白く光る
        cc[i] = FVec4{ m_CausticsTint.x, m_CausticsTint.y, m_CausticsTint.z, kk };
    }
    for (u32 t = 0; t < m_TCount; ++t) {
        const u16 a = m_Tri[t*3], b = m_Tri[t*3+1], c = m_Tri[t*3+2];
        sb.DrawTriangleVC(disp[a].x, disp[a].y, disp[b].x, disp[b].y, disp[c].x, disp[c].y, cc[a], cc[b], cc[c]);
    }
}

void FWater2DComponent::DrawGlints(FSpriteBatch& sb, const FVec2* disp) noexcept {
    const f32 sz = ((m_MaxX - m_MinX) + (m_MaxY - m_MinY)) * 0.006f + 0.02f;
    u32 drawn = 0;
    for (u32 i = 0; i < m_VCount && drawn < 30; ++i) {
        const f32 cell = CausticAt(disp[i]);
        const f32 ph   = Sin(m_Time * 3.0f + static_cast<f32>(i) * 2.399963f);   // 散発的に点滅
        if (cell > 0.82f && ph > 0.6f) {
            f32 br = (cell - 0.82f) * 4.0f; if (br > 1.0f) br = 1.0f;
            const FVec4 g{ 0.9f, 0.97f, 1.0f, br * 0.8f };
            sb.DrawRect(disp[i].x - sz * 0.5f, disp[i].y - sz * 0.5f, sz, sz, g);
            ++drawn;
        }
    }
}

// ===========================================================================
// FFire2DComponent
// ===========================================================================

void FFire2DComponent::OnDraw(RenderContext& rc) noexcept {
    if (!rc.HasSprites()) return;
    FSpriteBatch& sb = rc.Sprites();
    const FVec2 base = Owner().World().position;   // 炎の根元
    const u32   M    = 14;
    const f32   H    = m_H * (0.7f + 0.3f * m_Intensity);

    for (u32 k = 0; k < M; ++k) {
        const f32 t     = static_cast<f32>(k) / static_cast<f32>(M);     // 0 根元 .. 1 先端
        const f32 flick = 1.0f + 0.25f * m_Intensity * Sin(m_Time * 12.0f + static_cast<f32>(k) * 1.7f);
        const f32 w     = m_W * (1.0f - t) * flick;
        const f32 sway  = Sin(m_Time * 5.0f + t * 4.0f) * m_W * 0.25f * t; // 先端ほど揺れる
        const f32 cx    = base.x + sway;
        const f32 cy    = base.y - H * t;   // +Y=画面下なので炎は -Y (画面上) へ伸ばす
        // 色: 黄 → 橙 → 赤、alpha は上ほど薄く。
        FVec4 col;
        if (t < 0.5f) {
            const f32 u = t * 2.0f;
            col = FVec4{ 1.0f, 0.9f - 0.4f * u, 0.3f * (1.0f - u), (1.0f - t) * 0.9f };
        } else {
            const f32 u = (t - 0.5f) * 2.0f;
            col = FVec4{ 1.0f - 0.2f * u, 0.5f - 0.35f * u, 0.05f, (1.0f - t) * 0.9f };
        }
        sb.DrawRectRotated(cx, cy, w, H / static_cast<f32>(M) * 1.8f, 0.0f, col);
    }
    // 根元のオレンジの残り火。
    sb.DrawRectRotated(base.x, base.y, m_W * 1.1f, m_H * 0.12f, 0.0f,
                       FVec4{ 1.0f, 0.8f, 0.3f, 0.5f });
}

// ===========================================================================
// FTrail2DComponent
// ===========================================================================

void FTrail2DComponent::OnUpdate(f32 dt) noexcept {
    m_SampleAccum += dt;
    const FVec2 p = Owner().World().position;
    if (m_Count == 0) {
        m_Pts[0] = p;
        m_Count  = 1;
        return;
    }
    if (m_SampleAccum >= 0.012f) {          // 時間間隔でサンプル (fps 非依存)
        m_SampleAccum = 0.0f;
        const u32 last = (m_Count < m_Max) ? m_Count : (m_Max - 1u);
        for (u32 i = last; i > 0; --i) m_Pts[i] = m_Pts[i - 1];   // FIFO shift
        m_Pts[0] = p;
        if (m_Count < m_Max) ++m_Count;
    } else {
        m_Pts[0] = p;                       // head は常に現在位置へ追従
    }
}

void FTrail2DComponent::OnDraw(RenderContext& rc) noexcept {
    if (!rc.HasSprites() || m_Count < 2) return;
    FSpriteBatch& sb = rc.Sprites();
    for (u32 i = 0; i + 1 < m_Count; ++i) {
        const FVec2 a = m_Pts[i];
        const FVec2 b = m_Pts[i + 1];
        const f32 dx = b.x - a.x, dy = b.y - a.y;
        const f32 len = Sqrt(dx * dx + dy * dy);
        if (len < 1e-5f) continue;
        const f32 nx = -dy / len, ny = dx / len;          // 進行方向の法線
        const f32 ta = static_cast<f32>(i)     / static_cast<f32>(m_Count);   // 0 head .. 1 tail
        const f32 tb = static_cast<f32>(i + 1) / static_cast<f32>(m_Count);
        const f32 wa = m_Width * (1.0f - ta) * 0.5f;       // 先頭ほど太く
        const f32 wb = m_Width * (1.0f - tb) * 0.5f;
        const FVec4 col{ m_Color.x, m_Color.y, m_Color.z, (1.0f - ta) * 0.8f };
        const f32 a0x = a.x + nx * wa, a0y = a.y + ny * wa;
        const f32 a1x = a.x - nx * wa, a1y = a.y - ny * wa;
        const f32 b0x = b.x + nx * wb, b0y = b.y + ny * wb;
        const f32 b1x = b.x - nx * wb, b1y = b.y - ny * wb;
        sb.DrawTriangle(a0x, a0y, a1x, a1y, b0x, b0y, col);
        sb.DrawTriangle(a1x, a1y, b1x, b1y, b0x, b0y, col);
    }
}

// ===========================================================================
// FStencilClip2DComponent
// ===========================================================================

void FStencilClip2DComponent::PushTri(FVec2 a, FVec2 b, FVec2 c) noexcept {
    if (m_TriCount >= kMaxTris) return;
    m_Tri[m_TriCount * 3 + 0] = a;
    m_Tri[m_TriCount * 3 + 1] = b;
    m_Tri[m_TriCount * 3 + 2] = c;
    ++m_TriCount;
}

void FStencilClip2DComponent::SetRect(FVec2 center, FVec2 half) noexcept {
    m_TriCount = 0;
    const FVec2 tl{ center.x - half.x, center.y - half.y };
    const FVec2 tr{ center.x + half.x, center.y - half.y };
    const FVec2 br{ center.x + half.x, center.y + half.y };
    const FVec2 bl{ center.x - half.x, center.y + half.y };
    PushTri(tl, tr, br);
    PushTri(tl, br, bl);
}

void FStencilClip2DComponent::SetEllipse(FVec2 center, f32 rx, f32 ry, u32 segments) noexcept {
    m_TriCount = 0;
    if (segments < 3) segments = 3;
    if (segments > kMaxTris) segments = kMaxTris;
    FVec2 prev{ center.x + rx, center.y };
    for (u32 i = 1; i <= segments; ++i) {
        const f32 t = 6.2831853f * (static_cast<f32>(i) / static_cast<f32>(segments));
        const FVec2 cur{ center.x + Cos(t) * rx, center.y + Sin(t) * ry };
        PushTri(center, prev, cur);   // 中心からの扇
        prev = cur;
    }
}

void FStencilClip2DComponent::SetCircle(FVec2 center, f32 radius, u32 segments) noexcept {
    SetEllipse(center, radius, radius, segments);
}

void FStencilClip2DComponent::SetPolygon(const FVec2* pts, u32 count) noexcept {
    m_TriCount = 0;
    if (!pts || count < 3) return;
    // centroid を求めて扇状に三角形化 (凸〜緩い凹向け)。
    FVec2 c{ 0.0f, 0.0f };
    for (u32 i = 0; i < count; ++i) { c.x += pts[i].x; c.y += pts[i].y; }
    c.x /= static_cast<f32>(count); c.y /= static_cast<f32>(count);
    for (u32 i = 0; i < count; ++i) {
        const FVec2 a = pts[i];
        const FVec2 b = pts[(i + 1) % count];
        PushTri(c, a, b);
    }
}

void FStencilClip2DComponent::OnDraw(RenderContext& rc) noexcept {
    m_Active = false;
    // stencil バッファが用意されていないパスではマスクせず素通し。
    if (!rc.HasSprites() || !rc.StencilMaskActive() || m_TriCount == 0) return;
    FSpriteBatch& sb = rc.Sprites();
    const FVec2 o = Owner().World().position;

    // 1) マスク形状をステンシルへ焼く。色は不可視 (alpha 0)、debug 時のみ可視。
    const FVec4 col = m_DebugVis ? m_DebugColor : FVec4{ 0.0f, 0.0f, 0.0f, 0.0f };
    sb.SetStencilMode(EStencilMode::WriteMask, m_Ref);
    for (u32 t = 0; t < m_TriCount; ++t) {
        const FVec2 a = m_Tri[t * 3 + 0], b = m_Tri[t * 3 + 1], cc = m_Tri[t * 3 + 2];
        sb.DrawTriangle(o.x + a.x, o.y + a.y, o.x + b.x, o.y + b.y, o.x + cc.x, o.y + cc.y, col);
    }
    // 2) 以降の描画 (= 子ツリー) を内側 (or 外側) だけに通す。
    sb.SetStencilMode(m_Outside ? EStencilMode::KeepOutside : EStencilMode::KeepInside, m_Ref);
    m_Active = true;
}

void FStencilClip2DComponent::OnDrawPostChildren(RenderContext& rc) noexcept {
    if (!m_Active || !rc.HasSprites()) return;
    rc.Sprites().SetStencilMode(EStencilMode::Off, m_Ref);   // クリップ解除
    m_Active = false;
}

} // namespace acs::game
