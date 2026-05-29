// SPDX-License-Identifier: Apache-2.0
// FScene2D - practical base scene for 2D games.
//
// It wires the common 2D stack:
//   SceneServices(Default2D | Camera2D | Physics2D)
//   root FNode2D tree
//   one shared FSpriteBatch for world + HUD drawing
//
// Users override OnReady/OnTick/OnFixedTick/OnDrawWorld/OnDrawHud instead of
// re-implementing the same root/update/render plumbing in every scene.
#pragma once

#include "gameframework/Scene.h"
#include "gameframework/Node2D.h"
#include "render/SpriteBatch.h"

namespace acs::game {

class FScene2D : public Scene {
public:
    FScene2D() noexcept = default;
    ~FScene2D() noexcept override = default;

    FScene2D(const FScene2D&)            = delete;
    FScene2D& operator=(const FScene2D&) = delete;

    ESvc WantedServices() const noexcept override {
        return ESvc::Default2D | ESvc::Camera2D | ESvc::Physics2D;
    }

    FNode2D& Root() noexcept { return m_Root; }
    const FNode2D& Root() const noexcept { return m_Root; }
    FSpriteBatch& SpriteBatch() noexcept { return m_Sprites; }

    void SetPixelsPerUnit(f32 ppu) noexcept { m_PixelsPerUnit = ppu > 0.001f ? ppu : 1.0f; }
    f32  PixelsPerUnit() const noexcept { return m_PixelsPerUnit; }

    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnFixedUpdate(f32 fixed_dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

protected:
    virtual void OnReady() noexcept {}
    virtual void OnTick(f32 /*dt*/) noexcept {}
    virtual void OnFixedTick(f32 /*fixed_dt*/) noexcept {}
    virtual void OnDrawWorld(RenderContext& /*rc*/, FSpriteBatch& /*sb*/) noexcept {}
    virtual void OnDrawHud(RenderContext& /*rc*/, FSpriteBatch& /*sb*/) noexcept {}

private:
    bool EnsureSpriteBatch(RenderContext& rc) noexcept;

    FNode2D      m_Root;
    FSpriteBatch m_Sprites;
    bool         m_SpritesReady = false;
    f32          m_PixelsPerUnit = 64.0f;
};

} // namespace acs::game
