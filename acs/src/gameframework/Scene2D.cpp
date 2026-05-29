// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Scene2D.h"

#include "gameframework/RenderContext.h"
#include "gameframework/SceneServices.h"
#include "gameframework/Camera2D.h"
#include "render/Renderer.h"
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

void FScene2D::OnRender(RenderContext& rc) noexcept {
    if (!EnsureSpriteBatch(rc)) return;
    FSpriteBatch& sb = m_Sprites;
    sb.Begin(rc.Cmd(), rc.Width(), rc.Height());
    rc._SetSpriteBatch(&sb);

    FCamera2D& cam = Services().Camera();
    const FVec2 center = cam.EffectiveViewCenter();
    sb.SetView(center.x, center.y, m_PixelsPerUnit * cam.Zoom());
    m_Root.DrawTree(rc);
    OnDrawWorld(rc, sb);

    sb.SetView(static_cast<f32>(rc.Width()) * 0.5f,
               static_cast<f32>(rc.Height()) * 0.5f,
               1.0f);
    OnDrawHud(rc, sb);

    rc._SetSpriteBatch(nullptr);
    sb.End();
}

} // namespace acs::game
