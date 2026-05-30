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

    // マウスピッキング用: 画面ピクセル座標 (左上原点、Input::MousePos() の値) を
    // ワールド座標へ変換する。FScene2D のレンダリング (ppu * camera zoom、camera
    // 中心) と厳密に逆対応するので、Camera2D::ScreenToWorld (ppu 非考慮) ではなく
    // こちらを使うこと。画面サイズは直近の OnRender でキャッシュした値を用いる。
    FVec2 ScreenToWorld(FVec2 screen_px) noexcept;
    u32   ScreenWidth()  const noexcept { return m_ScreenW; }
    u32   ScreenHeight() const noexcept { return m_ScreenH; }

    // 平面反射を有効化する。ON にすると OnRender が「world をオフスクリーン RT に
    // 焼く → スワップチェーンに world+水(反射)+HUD」の 3 パスになる (反射する水が
    // 無くても world を 2 度描くコストがかかるので、反射を使うシーンだけ ON にする)。
    // 既定 OFF = 従来の単一パス (挙動・コスト無変更)。
    void SetReflectionEnabled(bool on) noexcept { m_ReflectionEnabled = on; }
    bool ReflectionEnabled() const noexcept { return m_ReflectionEnabled; }

    // ステンシルマスクを有効化する。ON にすると world パスが stencil 付き深度バッファ
    // (D24S8) を bind した状態で描かれ、FStencilClip2DComponent 等が任意形状で描画
    // 範囲をマスクできるようになる。既定 OFF = 従来どおり (DSV 無し)。反射と併用可。
    void SetStencilMaskEnabled(bool on) noexcept { m_StencilMaskEnabled = on; }
    bool StencilMaskEnabled() const noexcept { return m_StencilMaskEnabled; }

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
    bool EnsureSceneSprites(RenderContext& rc) noexcept;  // 反射オフスクリーン用の別バッチ
    bool EnsureSceneRt(RenderContext& rc) noexcept;       // 反射用シーン RT を遅延作成
    bool EnsureStencilBuffer(RenderContext& rc) noexcept; // マスク用 D24S8 を遅延作成
    void DrawWorldPass(RenderContext& rc) noexcept;       // world (DrawTree + OnDrawWorld)
    void DrawHudPass(RenderContext& rc) noexcept;         // HUD view + OnDrawHud

    FNode2D      m_Root;
    FSpriteBatch m_Sprites;
    bool         m_SpritesReady = false;
    FSpriteBatch m_SceneSprites;          // 反射オフスクリーン (Phase 1) 専用
    bool         m_SceneSpritesReady = false;
    f32          m_PixelsPerUnit = 64.0f;
    u32          m_ScreenW = 1280;        // 直近 OnRender の画面サイズ (picking 用)
    u32          m_ScreenH = 720;

    TUniquePtr<IRhiTexture> m_SceneRt;    // 反射用: world を焼くオフスクリーン RT
    u32          m_RtW = 0, m_RtH = 0;
    bool         m_ReflectionEnabled = false;

    TUniquePtr<IRhiTexture> m_StencilBuf; // マスク用: stencil 付き深度バッファ (D24S8)
    u32          m_StencilW = 0, m_StencilH = 0;
    bool         m_StencilMaskEnabled = false;
};

} // namespace acs::game
