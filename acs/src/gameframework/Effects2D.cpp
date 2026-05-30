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
} // namespace

// ===========================================================================
// FWater2DComponent
// ===========================================================================

// ---- 形状ビルダー共通ヘルパ ----
void FWater2DComponent::PushVert(FVec2 v) noexcept {
    if (m_VCount < kMaxVerts) m_Vert[m_VCount++] = v;
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
}

void FWater2DComponent::SetRect(FVec2 center, FVec2 half, u32 top_segments) noexcept {
    m_VCount = 0; m_TCount = 0;
    if (top_segments < 1) top_segments = 1;
    if (top_segments > kMaxVerts - 3) top_segments = kMaxVerts - 3;
    const f32 topY = center.y - half.y;     // 画面上 = min Y
    const f32 botY = center.y + half.y;
    const f32 lx = center.x - half.x, rx = center.x + half.x;
    for (u32 i = 0; i <= top_segments; ++i) {            // 上辺を分割 (滑らかな波)
        const f32 x = lx + (rx - lx) * (static_cast<f32>(i) / static_cast<f32>(top_segments));
        PushVert(FVec2{ x, topY });
    }
    const u32 br = m_VCount; PushVert(FVec2{ rx, botY });
    const u32 bl = m_VCount; PushVert(FVec2{ lx, botY });
    for (u32 i = 0; i < top_segments; ++i) PushTri(bl, i, i + 1);   // bl から扇状に
    PushTri(bl, top_segments, br);
    Finish();
}

void FWater2DComponent::SetEllipse(FVec2 center, f32 rx, f32 ry, u32 segments) noexcept {
    m_VCount = 0; m_TCount = 0;
    if (segments < 3) segments = 3;
    if (segments > kMaxVerts - 1) segments = kMaxVerts - 1;
    const u32 c = m_VCount; PushVert(center);            // 中心 (扇の要)
    for (u32 i = 0; i < segments; ++i) {
        const f32 a = 6.2831853f * (static_cast<f32>(i) / static_cast<f32>(segments));
        PushVert(FVec2{ center.x + Cos(a) * rx, center.y + Sin(a) * ry });
    }
    for (u32 i = 0; i < segments; ++i)
        PushTri(c, c + 1 + i, c + 1 + ((i + 1) % segments));
    Finish();
}

void FWater2DComponent::SetPolygon(const FVec2* pts, u32 count) noexcept {
    m_VCount = 0; m_TCount = 0;
    if (pts == nullptr || count < 3) return;
    if (count > kMaxVerts - 1) count = kMaxVerts - 1;
    FVec2 ctr{ 0.0f, 0.0f };
    for (u32 i = 0; i < count; ++i) { ctr.x += pts[i].x; ctr.y += pts[i].y; }
    ctr.x /= static_cast<f32>(count); ctr.y /= static_cast<f32>(count);
    const u32 c = m_VCount; PushVert(ctr);               // centroid から扇状に
    for (u32 i = 0; i < count; ++i) PushVert(pts[i]);
    for (u32 i = 0; i < count; ++i)
        PushTri(c, c + 1 + i, c + 1 + ((i + 1) % count));
    Finish();
}

void FWater2DComponent::SetRiver(const FVec2* path, u32 count, f32 width) noexcept {
    m_VCount = 0; m_TCount = 0;
    if (path == nullptr || count < 2) return;
    const u32 maxPts = kMaxVerts / 2u;
    if (count > maxPts) count = maxPts;
    const f32 hw = width * 0.5f;
    for (u32 i = 0; i < count; ++i) {                    // 各 path 点で左右にオフセット
        FVec2 dir;
        if (i == 0)            dir = FVec2{ path[1].x - path[0].x, path[1].y - path[0].y };
        else if (i == count-1) dir = FVec2{ path[i].x - path[i-1].x, path[i].y - path[i-1].y };
        else                   dir = FVec2{ path[i+1].x - path[i-1].x, path[i+1].y - path[i-1].y };
        const f32 len = Sqrt(dir.x * dir.x + dir.y * dir.y);
        const FVec2 n = len > 1e-5f ? FVec2{ -dir.y / len, dir.x / len } : FVec2{ 0.0f, 1.0f };
        PushVert(FVec2{ path[i].x + n.x * hw, path[i].y + n.y * hw });   // 左岸 (偶数)
        PushVert(FVec2{ path[i].x - n.x * hw, path[i].y - n.y * hw });   // 右岸 (奇数)
    }
    for (u32 i = 0; i + 1 < count; ++i) {                // quad strip
        const u32 a = 2*i, b = 2*i+1, cc = 2*i+2, d = 2*i+3;
        PushTri(a, b, cc);
        PushTri(b, d, cc);
    }
    Finish();
}

void FWater2DComponent::SetSplineRiver(const FVec2* control, u32 count, f32 width,
                                       u32 samples_per_seg) noexcept {
    if (control == nullptr || count < 2) return;
    if (samples_per_seg < 1) samples_per_seg = 1;
    FVec2 dense[kMaxVerts / 2u];
    const u32 cap = kMaxVerts / 2u;
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

void FWater2DComponent::Disturb(f32 world_x, f32 strength) noexcept {
    u32 slot = kMaxRipples;
    f32 oldest = -1.0f;
    for (u32 i = 0; i < kMaxRipples; ++i) {
        if (!m_Ripples[i].active) { slot = i; break; }
        if (m_Ripples[i].time > oldest) { oldest = m_Ripples[i].time; slot = i; }
    }
    if (slot >= kMaxRipples) slot = 0;
    m_Ripples[slot] = Ripple{ world_x, strength, strength, 0.0f, true };
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

f32 FWater2DComponent::WaveAt(f32 world_x) const noexcept {
    f32 off = m_Amp        * Sin(world_x * 3.0f + m_Time * m_Speed)
            + m_Amp * 0.4f * Sin(world_x * 7.0f - m_Time * m_Speed * 1.7f);
    for (u32 i = 0; i < kMaxRipples; ++i) {
        if (!m_Ripples[i].active) continue;
        const f32 d    = world_x - m_Ripples[i].x;
        const f32 dist = d < 0.0f ? -d : d;
        f32 env = 1.0f - dist * 0.6f - m_Ripples[i].time * 0.25f;
        if (env <= 0.0f) continue;
        off += m_Ripples[i].amp * Cos(dist * 9.0f - m_Ripples[i].time * 11.0f) * env;
    }
    return off;
}

void FWater2DComponent::OnUpdate(f32 dt) noexcept {
    m_Time += dt;
    for (u32 i = 0; i < kMaxRipples; ++i) {
        if (!m_Ripples[i].active) continue;
        m_Ripples[i].time += dt;
        m_Ripples[i].amp = m_Ripples[i].amp0 * Exp(-m_Ripples[i].time * 2.2f);
        if (m_Ripples[i].time > 3.0f || m_Ripples[i].amp < 0.002f) m_Ripples[i].active = false;
    }
}

void FWater2DComponent::OnDraw(RenderContext& rc) noexcept {
    if (!rc.HasSprites() || m_TCount == 0) return;
    FSpriteBatch& sb = rc.Sprites();
    const FVec2 o = Owner().World().position;

    // 各頂点を「水面らしさ × 波」で画面上 (-Y) へ揺らした world 位置に変換。
    FVec2 disp[kMaxVerts];
    for (u32 i = 0; i < m_VCount; ++i) {
        const f32 wx = o.x + m_Vert[i].x;
        const f32 wy = o.y + m_Vert[i].y - WaveAt(wx) * m_Weight[i];
        disp[i] = FVec2{ wx, wy };
    }
    const FVec4 body{ m_Color.x, m_Color.y, m_Color.z, m_Alpha };
    for (u32 t = 0; t < m_TCount; ++t) {
        const u16 a = m_Tri[t*3], b = m_Tri[t*3+1], c = m_Tri[t*3+2];
        sb.DrawTriangle(disp[a].x, disp[a].y, disp[b].x, disp[b].y, disp[c].x, disp[c].y, body);
    }
    // 泡: 水面らしさが高い頂点に小さな明るい点 (どの形状でも crest が光る)。
    const FVec4 foam{ 0.85f, 0.95f, 1.0f, m_Alpha * 0.9f };
    const f32   fs = (m_MaxX - m_MinX + m_MaxY - m_MinY) * 0.006f + 0.04f;
    for (u32 i = 0; i < m_VCount; ++i) {
        if (m_Weight[i] > 0.5f) sb.DrawRectRotated(disp[i].x, disp[i].y, fs, fs, 0.0f, foam);
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
        const f32 cy    = base.y + H * t;
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

} // namespace acs::game
