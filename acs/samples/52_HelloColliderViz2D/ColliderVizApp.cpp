// SPDX-License-Identifier: Apache-2.0
// HelloColliderViz2D 実装。
#include "ColliderVizApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"
#include "foundation/Log.h"

#include <cstdio>
#include <cmath>

using namespace acs;

namespace colliderviz {

namespace {

constexpr u32 kTexW = 128, kTexH = 128;

// (a,b) を太さ thickness の線として描く (回転矩形)。
void DrawLine(CSpriteBatch& sb, FVec2 a, FVec2 b, f32 thickness, FVec4 color) noexcept {
    const f32 dx = b.x - a.x, dy = b.y - a.y;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    const f32 ang = std::atan2(dy, dx);
    sb.DrawRectRotated((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, len, thickness, ang, color);
}

void DrawRing(CSpriteBatch& sb, FVec2 c, f32 r, FVec4 color) noexcept {
    constexpr int kSeg = 20;
    for (int i = 0; i < kSeg; ++i) {
        const f32 a0 = (static_cast<f32>(i)     / kSeg) * 6.2831853f;
        const f32 a1 = (static_cast<f32>(i + 1) / kSeg) * 6.2831853f;
        DrawLine(sb, FVec2{ c.x + std::cos(a0) * r, c.y + std::sin(a0) * r },
                     FVec2{ c.x + std::cos(a1) * r, c.y + std::sin(a1) * r }, 2.0f, color);
    }
}

} // namespace

void CColliderVizApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    m_bFontReady = FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, false).IsOk();

    // ----- 5 角星のスプライトを手続き生成 -----
    static u8 img[kTexW * kTexH * 4];
    for (u32 i = 0; i < kTexW * kTexH * 4u; ++i) img[i] = 0;
    const f32 cx = 64.0f, cy = 64.0f, outer = 52.0f, inner = 24.0f;
    for (u32 y = 0; y < kTexH; ++y)
        for (u32 x = 0; x < kTexW; ++x) {
            const f32 dx = static_cast<f32>(x) - cx, dy = static_cast<f32>(y) - cy;
            const f32 ang = std::atan2(dy, dx);
            const f32 f = std::cos(5.0f * ang);                 // 5 スパイク
            const f32 rmax = inner + (outer - inner) * (f * 0.5f + 0.5f);
            if (std::sqrt(dx * dx + dy * dy) <= rmax) {
                const u32 idx = (y * kTexW + x) * 4u;
                img[idx + 0] = 240; img[idx + 1] = 180; img[idx + 2] = 90; img[idx + 3] = 255;
            }
        }
    m_TexW = kTexW; m_TexH = kTexH;
    {
        FTextureDesc td{};
        td.width = kTexW; td.height = kTexH; td.format = EFormat::R8G8B8A8_UNorm;
        td.initial_data = img; td.initial_data_size = kTexW * kTexH * 4u;
        auto r = CreateRhiTexture(*dev, td);
        if (r.IsErr()) { Quit(); return; }
        m_SpriteTex = Move(r.Value());
    }

    ACS_SAMPLE_INIT(m_Collider.BuildFromAlpha(img, kTexW, kTexH, 128, 1.5f));

    m_SpritePos = FVec2{ 140.0f, 120.0f };

    // 凸包をスクリーン空間に変換して衝突ワールドに登録。
    m_World.Init(128.0f);
    FConvexPoly2 hull = m_Collider.HullPolygon();
    FConvexPoly2 screen_hull;
    for (u32 i = 0; i < hull.count; ++i) {
        screen_hull.Add(FVec2{ m_SpritePos.x + hull.verts[i].x * m_Scale,
                               m_SpritePos.y + hull.verts[i].y * m_Scale });
    }
    m_PolyId = m_World.AddPolygon(screen_hull);
}

void CColliderVizApp::OnUpdate(f32) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) { Quit(); return; }
    m_Probe = CInput::MousePos();
    TArray<acs::game::FShapeId> hits;
    m_World.OverlapCircle(FCircle{ m_Probe, 18.0f }, hits);
    m_bOverlap = hits.Num() > 0;
}

void CColliderVizApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain*   sc = GetRenderer().Swapchain();
    if (!cl || !sc) return;
    const u32 sw = sc->Width(), sh = sc->Height();

    m_Batch.Begin(*cl, sw, sh);
    m_Batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                     FVec4{ 0.10f, 0.11f, 0.14f, 1.0f });

    // スプライト本体
    if (m_SpriteTex) {
        m_Batch.Draw(*m_SpriteTex, m_SpritePos.x, m_SpritePos.y,
                     static_cast<f32>(m_TexW) * m_Scale, static_cast<f32>(m_TexH) * m_Scale);
    }

    auto to_screen = [&](FVec2 v) {
        return FVec2{ m_SpritePos.x + v.x * m_Scale, m_SpritePos.y + v.y * m_Scale };
    };

    // 簡略化輪郭 (シアン、凹に追従)
    const FVec2* outline = m_Collider.Outline();
    const u32    on = m_Collider.OutlineCount();
    for (u32 i = 0; i < on; ++i) {
        DrawLine(m_Batch, to_screen(outline[i]), to_screen(outline[(i + 1) % on]),
                 2.0f, FVec4{ 0.3f, 0.9f, 1.0f, 1.0f });
    }
    // 凸包 (緑)
    const FVec2* hull = m_Collider.Hull();
    const u32    hn = m_Collider.HullCount();
    for (u32 i = 0; i < hn; ++i) {
        DrawLine(m_Batch, to_screen(hull[i]), to_screen(hull[(i + 1) % hn]),
                 2.0f, FVec4{ 0.4f, 1.0f, 0.4f, 1.0f });
    }

    // マウスプローブ (重なると赤)
    DrawRing(m_Batch, m_Probe, 18.0f,
             m_bOverlap ? FVec4{ 1.0f, 0.3f, 0.3f, 1.0f } : FVec4{ 0.7f, 0.7f, 0.7f, 1.0f });

    if (m_bFontReady) {
        m_Batch.DrawString(m_Font,
            "ColliderViz2D  cyan=outline(concave)  green=convex hull  ring=mouse probe",
            24.0f, 22.0f, FVec4{ 0.95f, 0.96f, 1.0f, 1.0f });
        m_Batch.DrawString(m_Font, m_bOverlap ? "probe: OVERLAP" : "probe: free",
            24.0f, 46.0f, m_bOverlap ? FVec4{ 1.0f, 0.5f, 0.5f, 1.0f }
                                     : FVec4{ 0.6f, 0.8f, 0.6f, 1.0f });
        m_Batch.DrawString(m_Font, "Esc: quit", 24.0f, static_cast<f32>(sh) - 34.0f,
            FVec4{ 0.6f, 0.62f, 0.7f, 1.0f });
    }

    m_Batch.End();
}

void CColliderVizApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    if (m_bFontReady) m_Font.Shutdown();
    m_SpriteTex.Reset();
    m_Batch.Shutdown();
}

} // namespace colliderviz
