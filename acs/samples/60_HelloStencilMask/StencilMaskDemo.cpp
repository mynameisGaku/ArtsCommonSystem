// SPDX-License-Identifier: Apache-2.0
// HelloStencilMask - 任意形状のステンシルマスクで「窓越し」にエフェクトを覗かせるデモ。
//
//   ・隠しシーン (水/炎/光る四角) は普段は背景に溶けて見えない。
//   ・マウス追従の「窓」(円 or 矩形) の中だけ隠しシーンが現れる。
//   ・[Space] 内外反転 (窓の中だけ隠す = 視界の穴)。 [Tab] 円⇔矩形。 [Esc] 終了。
//
// 仕組み: FStencilClip2DComponent を持つノードの子ツリーを stencil でクリップする。
//         シェーダは書かない — FScene2D::SetStencilMaskEnabled(true) と
//         clip.SetCircle/SetRect だけ。
#include "gameframework/GameFramework.h"
#include "platform/Input.h"
#include "platform/InputCodes.h"
#include "math/Math.h"

using namespace acs;
using namespace acs::game;

namespace {

class FStencilScene final : public FScene2D {
public:
    void OnReady() noexcept override {
        SetPixelsPerUnit(48.0f);
        SetStencilMaskEnabled(true);   // world パスを stencil 付き DSV で描く

        // --- 窓 (マスク) ノード。原点に置き、形状の中心をマウスで動かす ---
        {
            auto win = MakeUnique<FNode2D>();
            win->Local().position = FVec2{0.0f, 0.0f};
            m_Clip = &win->AddComponent<FStencilClip2DComponent>();
            m_Clip->SetCircle(FVec2{0.0f, 0.0f}, m_Radius, 48);

            // 子ツリー = 窓の中だけに見える「隠しシーン」。
            // 海 (画面下の矩形水面)。
            {
                auto n = MakeUnique<FNode2D>();
                n->Local().position = FVec2{0.0f, 0.0f};
                auto& w = n->AddComponent<FWater2DComponent>();
                w.SetRect(FVec2{0.0f, 4.5f}, FVec2{12.0f, 2.4f});
                w.SetWaves(0.16f, 1.6f);
                w.SetDepthColors(FVec3{0.22f, 0.55f, 0.78f}, FVec3{0.03f, 0.14f, 0.30f});
                w.SetDepthAlpha(0.85f, 1.0f);
                w.SetFoam(FVec3{0.95f, 0.98f, 1.0f}, 0.0f, 3.0f, 0.9f);
                win->AddChild(Move(n));
            }
            // 焚き火。
            {
                auto n = MakeUnique<FNode2D>();
                n->Local().position = FVec2{-3.0f, 1.6f};
                auto& f = n->AddComponent<FFire2DComponent>();
                f.SetSize(1.0f, 2.4f);
                f.SetIntensity(1.6f);
                win->AddChild(Move(n));
            }
            // 光る四角を格子状に (窓が動くと次々に現れる)。
            for (i32 gy = 0; gy < 4; ++gy) {
                for (i32 gx = 0; gx < 7; ++gx) {
                    auto n = MakeUnique<FNode2D>();
                    n->Local().position = FVec2{-6.0f + static_cast<f32>(gx) * 2.0f,
                                                -3.4f + static_cast<f32>(gy) * 1.6f};
                    const f32 hue = static_cast<f32>((gx + gy) % 4) / 4.0f;
                    n->AddComponent<FSprite2DComponent>(
                        FVec2{0.7f, 0.7f},
                        FVec4{0.4f + 0.6f * hue, 0.9f - 0.5f * hue, 0.5f + 0.4f * hue, 1.0f});
                    win->AddChild(Move(n));
                }
            }
            Root().AddChild(Move(win));
        }

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);
        GetGame().SetClearColor(0.04f, 0.05f, 0.07f);
    }

    void OnTick(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(EKey::Escape)) { GetGame().Quit(); return; }

        // 窓の中心をマウスへ追従させる (owner は原点なので world = 相対座標)。
        const FVec2 mw = ScreenToWorld(Input::MousePos());
        if (m_Clip) {
            if (m_RectShape) m_Clip->SetRect(mw, FVec2{m_Radius * 1.2f, m_Radius * 0.9f});
            else             m_Clip->SetCircle(mw, m_Radius, 48);
        }

        // [Space] 内外反転 (押した瞬間だけトグル)。
        const bool space = Input::IsKeyPressed(EKey::Space);
        if (space && !m_SpacePrev && m_Clip) { m_Invert = !m_Invert; m_Clip->SetInvert(m_Invert); }
        m_SpacePrev = space;

        // [Tab] 円 ⇔ 矩形。
        const bool tab = Input::IsKeyPressed(EKey::Tab);
        if (tab && !m_TabPrev) m_RectShape = !m_RectShape;
        m_TabPrev = tab;

        // ホイール代わりに上下キーで窓サイズ調整。
        if (Input::IsKeyDown(EKey::Up))   m_Radius += 0.04f;
        if (Input::IsKeyDown(EKey::Down)) m_Radius -= 0.04f;
        if (m_Radius < 0.6f) m_Radius = 0.6f;
        if (m_Radius > 6.0f) m_Radius = 6.0f;
    }

    void OnDrawHud(RenderContext& rc, FSpriteBatch& sb) noexcept override {
        sb.DrawRect(8.0f, 8.0f, 720.0f, 30.0f, FVec4{0.0f, 0.0f, 0.0f, 0.45f});
        if (rc.HasFont()) {
            sb.DrawString(rc.GetFont(),
                          "StencilMask  move mouse = window into hidden scene   "
                          "[Space] invert  [Tab] circle/rect  [Up/Down] size  [Esc]",
                          16.0f, 15.0f, FVec4{0.9f, 0.95f, 1.0f, 1.0f});
        }
    }

private:
    FStencilClip2DComponent* m_Clip = nullptr;
    f32  m_Radius    = 2.4f;
    bool m_RectShape = false;
    bool m_Invert    = false;
    bool m_SpacePrev = false;
    bool m_TabPrev   = false;
};

class FStencilGame final : public FGame {
protected:
    TUniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<FStencilScene>();
    }
};

} // namespace

ACS_GAME_MAIN(FStencilGame)
