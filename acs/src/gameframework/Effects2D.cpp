// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Effects2D.h"
#include "gameframework/Node2D.h"
#include "gameframework/RenderContext.h"
#include "render/SpriteBatch.h"
#include "math/Math.h"

namespace acs::game {

// ===========================================================================
// FWater2DComponent
// ===========================================================================

void FWater2DComponent::Disturb(f32 world_x, f32 strength) noexcept {
    // 空きスロット優先、無ければ最も古い波紋を上書き。
    u32 slot = kMaxRipples;
    f32 oldest = -1.0f;
    for (u32 i = 0; i < kMaxRipples; ++i) {
        if (!m_Ripples[i].active) { slot = i; break; }
        if (m_Ripples[i].time > oldest) { oldest = m_Ripples[i].time; slot = i; }
    }
    if (slot >= kMaxRipples) slot = 0;
    m_Ripples[slot] = Ripple{ world_x, strength, strength, 0.0f, true };
}

f32 FWater2DComponent::SurfaceY() const noexcept {
    return Owner().World().position.y + m_Center.y + m_Half.y;
}

bool FWater2DComponent::ContainsX(f32 world_x) const noexcept {
    const f32 ox = Owner().World().position.x + m_Center.x;
    return world_x >= ox - m_Half.x && world_x <= ox + m_Half.x;
}

bool FWater2DComponent::ContainsPoint(FVec2 world) const noexcept {
    const FVec2 o = Owner().World().position;
    const f32 cx = o.x + m_Center.x, cy = o.y + m_Center.y;
    return world.x >= cx - m_Half.x && world.x <= cx + m_Half.x &&
           world.y >= cy - m_Half.y && world.y <= cy + m_Half.y;
}

f32 FWater2DComponent::SurfaceOffsetAt(f32 world_x) const noexcept {
    // ambient: 2 つの sin の和で有機的なうねり。
    f32 off = m_Amp        * Sin(world_x * 3.0f + m_Time * m_Speed)
            + m_Amp * 0.4f * Sin(world_x * 7.0f - m_Time * m_Speed * 1.7f);
    // 干渉: 各 Disturb から外側へ広がりながら減衰する波紋。
    for (u32 i = 0; i < kMaxRipples; ++i) {
        if (!m_Ripples[i].active) continue;
        const f32 d    = world_x - m_Ripples[i].x;
        const f32 dist = d < 0.0f ? -d : d;
        f32 env = 1.0f - dist * 0.6f - m_Ripples[i].time * 0.25f;  // 時間で縮む包絡
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
    if (!rc.HasSprites()) return;
    FSpriteBatch& sb = rc.Sprites();
    const FVec2 o = Owner().World().position;
    const f32 left   = o.x + m_Center.x - m_Half.x;
    const f32 right  = o.x + m_Center.x + m_Half.x;
    const f32 bottom = o.y + m_Center.y - m_Half.y;
    const f32 surf   = o.y + m_Center.y + m_Half.y;
    const u32 N = m_Segs;

    const FVec4 body{ m_Color.x, m_Color.y, m_Color.z, m_Alpha };
    const FVec4 foam{ 0.85f, 0.95f, 1.0f, m_Alpha * 0.9f };
    const f32   foamT = (m_Half.x + m_Half.y) * 0.02f + 0.03f;   // 泡の厚み

    f32 prevX   = left;
    f32 prevTop = surf + SurfaceOffsetAt(left);
    for (u32 i = 1; i <= N; ++i) {
        const f32 x   = left + (right - left) * (static_cast<f32>(i) / static_cast<f32>(N));
        const f32 top = surf + SurfaceOffsetAt(x);
        // 本体 (波打つ上辺 → 平らな底) を 2 三角形で塗る。
        sb.DrawTriangle(prevX, bottom, x, bottom, x, top, body);
        sb.DrawTriangle(prevX, bottom, x, top, prevX, prevTop, body);
        // 泡 (crest の薄い明るい帯)。
        sb.DrawTriangle(prevX, prevTop - foamT, x, top - foamT, x, top, foam);
        sb.DrawTriangle(prevX, prevTop - foamT, x, top, prevX, prevTop, foam);
        prevX = x; prevTop = top;
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
