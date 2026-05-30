// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Scene2D.h"

#include "gameframework/RenderContext.h"
#include "gameframework/SceneServices.h"
#include "gameframework/Camera2D.h"
#include "gameframework/Game.h"
#include "render/Renderer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiTexture.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

void FScene2D::OnEnter() noexcept {
    OnReady();
}

void FScene2D::OnExit() noexcept {
    m_Root.ResolveStructuralChanges();
    if (HasServices()) {
        Services().Physics().ClearAll();
        Services().Tweens().CancelAll();
    }
    m_Sprites.Shutdown();
    m_SpritesReady = false;
}

void FScene2D::OnUpdate(f32 dt) noexcept {
    OnTick(dt);
    m_Root.UpdateTree(dt);
    m_Root.ResolveStructuralChanges();
}

void FScene2D::OnFixedUpdate(f32 fixed_dt) noexcept {
    OnFixedTick(fixed_dt);
    m_Root.FixedUpdateTree(fixed_dt);
    m_Root.ResolveStructuralChanges();
}

bool FScene2D::EnsureSpriteBatch(RenderContext& rc) noexcept {
    if (m_SpritesReady) return true;
    FRenderer& renderer = rc.GetRenderer();
    IRhiDevice* device = renderer.Device();
    if (!device) return false;
    auto r = m_Sprites.Init(*device, renderer.ColorFormat(), 8192);
    if (r.IsErr()) {
        ACS_LOG_ERROR("FScene2D: FSpriteBatch init failed");
        return false;
    }
    m_SpritesReady = true;
    return true;
}

FVec2 FScene2D::ScreenToWorld(FVec2 screen_px) noexcept {
    FVec2 vc{0.0f, 0.0f};
    f32   zoom = 1.0f;
    if (HasServices()) {
        FCamera2D& cam = Services().Camera();
        vc   = cam.EffectiveViewCenter();
        zoom = cam.Zoom();
    }
    const f32 scale = m_PixelsPerUnit * zoom;
    const f32 inv   = scale > 1e-6f ? 1.0f / scale : 0.0f;
    return FVec2{ vc.x + (screen_px.x - static_cast<f32>(m_ScreenW) * 0.5f) * inv,
                  vc.y + (screen_px.y - static_cast<f32>(m_ScreenH) * 0.5f) * inv };
}

void FScene2D::DrawWorldPass(RenderContext& rc) noexcept {
    FSpriteBatch& sb = m_Sprites;
    FCamera2D& cam = Services().Camera();
    const FVec2 center = cam.EffectiveViewCenter();
    sb.SetView(center.x, center.y, m_PixelsPerUnit * cam.Zoom());
    m_Root.DrawTree(rc);
    OnDrawWorld(rc, sb);
}

void FScene2D::DrawHudPass(RenderContext& rc) noexcept {
    FSpriteBatch& sb = m_Sprites;
    sb.SetView(static_cast<f32>(rc.Width()) * 0.5f,
               static_cast<f32>(rc.Height()) * 0.5f, 1.0f);
    OnDrawHud(rc, sb);
}

bool FScene2D::EnsureSceneRt(RenderContext& rc) noexcept {
    const u32 w = rc.Width(), h = rc.Height();
    if (w == 0 || h == 0) return false;
    if (m_SceneRt && m_RtW == w && m_RtH == h) return true;
    IRhiDevice* dev = rc.GetRenderer().Device();
    if (dev == nullptr) return false;
    FTextureDesc td{};
    td.width = w; td.height = h;
    td.format = rc.GetRenderer().ColorFormat();
    td.is_render_target = true;
    auto r = CreateRhiTexture(*dev, td);
    if (r.IsErr()) { ACS_LOG_WARN("FScene2D: 反射用 scene RT の作成に失敗 (反射無効)"); return false; }
    m_SceneRt = Move(r.Value());
    m_RtW = w; m_RtH = h;
    return true;
}

void FScene2D::OnRender(RenderContext& rc) noexcept {
    if (!EnsureSpriteBatch(rc)) return;
    m_ScreenW = rc.Width();        // picking 用に画面サイズをキャッシュ
    m_ScreenH = rc.Height();
    FSpriteBatch& sb = m_Sprites;
    FCamera2D& cam = Services().Camera();
    const FVec2 center = cam.EffectiveViewCenter();
    const f32 scale = m_PixelsPerUnit * cam.Zoom();
    rc._SetView2D(center, scale);   // 反射等の world→screen 投影用に配線

    if (m_ReflectionEnabled && EnsureSceneRt(rc)) {
        FRenderer& renderer = rc.GetRenderer();
        IRhiCommandList& cl = rc.Cmd();
        IRhiSwapchain* sc = renderer.Swapchain();
        const ClearColor cc = GetGame().GetClearColor();

        // Phase 1: world をオフスクリーン RT に焼く (反射バインド無し → 水はプレーン fill)。
        cl.BeginRenderToTexture(*m_SceneRt, cc);
        sb.Begin(cl, rc.Width(), rc.Height());
        rc._SetSpriteBatch(&sb);
        rc._SetReflection(nullptr);
        DrawWorldPass(rc);
        rc._SetSpriteBatch(nullptr);
        sb.End();
        cl.EndRenderToTexture(*m_SceneRt);

        // Phase 2: スワップチェーンを再バインド (バリアはガード済) → world+水(反射)+HUD。
        if (sc) cl.BeginRenderToSwapchain(*sc, renderer.CurrentBuffer(), cc);
        sb.Begin(cl, rc.Width(), rc.Height());
        rc._SetSpriteBatch(&sb);
        rc._SetReflection(m_SceneRt.Get());   // 水がこの RT を鏡像 UV でサンプル
        DrawWorldPass(rc);
        rc._SetReflection(nullptr);
        DrawHudPass(rc);
        rc._SetSpriteBatch(nullptr);
        sb.End();
        // EndRenderToSwapchain は FGame/Renderer の EndFrame が行う。
    } else {
        sb.Begin(rc.Cmd(), rc.Width(), rc.Height());
        rc._SetSpriteBatch(&sb);
        DrawWorldPass(rc);
        DrawHudPass(rc);
        rc._SetSpriteBatch(nullptr);
        sb.End();
    }
}

} // namespace acs::game
