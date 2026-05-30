// SPDX-License-Identifier: Apache-2.0
// HelloEffects2D - Package D デモ: シェーダー無しのインタラクティブエフェクト集。
//
//   ・マウス追従のドットが FTrail2DComponent で残像を引く。
//   ・水域 (FWater2DComponent) をマウスでなぞる/クリックすると波紋が立つ。
//   ・焚き火 (FFire2DComponent) にマウスが近づくと炎の勢いが増す。
//
// すべて SpriteBatch のプリミティブを手続き生成して描くので HLSL 不要。
// マウス→ワールド変換は FScene2D::ScreenToWorld (ppu 対応) を使う。
#include "gameframework/GameFramework.h"
#include "platform/Input.h"
#include "platform/InputCodes.h"
#include "math/Math.h"

using namespace acs;
using namespace acs::game;

namespace {

class FEffectsScene final : public FScene2D {
public:
    void OnReady() noexcept override {
        SetPixelsPerUnit(48.0f);

        // 水域 (画面下側、world +Y = 画面下)。
        {
            auto n = MakeUnique<FNode2D>();
            n->Local().position = FVec2{0.0f, 0.0f};
            auto& w = n->AddComponent<FWater2DComponent>();
            w.SetArea(FVec2{0.0f, 4.6f}, FVec2{11.0f, 2.4f});
            w.SetColor(FVec3{0.10f, 0.38f, 0.62f});
            w.SetWaves(0.18f, 1.6f);
            m_Water = &w;
            Root().AddChild(Move(n));
        }
        // 焚き火 (左下)。
        {
            auto n = MakeUnique<FNode2D>();
            m_FirePos = FVec2{-6.5f, 1.6f};
            n->Local().position = m_FirePos;
            auto& f = n->AddComponent<FFire2DComponent>();
            f.SetSize(0.85f, 2.1f);
            m_Fire = &f;
            Root().AddChild(Move(n));
        }
        // プレイヤー = マウス追従ドット + トレイル。
        {
            auto n = MakeUnique<FNode2D>();
            n->Local().position = FVec2{0.0f, 0.0f};
            n->AddComponent<FSprite2DComponent>(FVec2{0.35f, 0.35f},
                                                FVec4{0.45f, 0.95f, 1.0f, 1.0f});
            auto& t = n->AddComponent<FTrail2DComponent>();
            t.SetColor(FVec3{0.45f, 0.9f, 1.0f});
            t.SetWidth(0.38f);
            t.SetPoints(28);
            m_Player = &Root().AddChild(Move(n));
        }

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);
        GetGame().SetClearColor(0.05f, 0.06f, 0.09f);
    }

    void OnTick(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(EKey::Escape)) { GetGame().Quit(); return; }

        const FVec2 mw = ScreenToWorld(Input::MousePos());   // ppu 対応のピッキング
        if (m_Player) m_Player->Local().position = mw;

        // 水: なぞると波紋、左クリックで splash。
        if (m_Water) {
            if (m_Water->ContainsPoint(mw)) {
                const FVec2 md = Input::MouseDelta();
                const f32 sp = Sqrt(md.x * md.x + md.y * md.y);   // px/frame
                if (sp > 1.0f) m_Water->Disturb(mw.x, 0.05f + sp * 0.0006f);
            }
            if (Input::IsMouseButtonPressed(EMouseButton::Left) && m_Water->ContainsX(mw.x)) {
                m_Water->Disturb(mw.x, 0.55f);
            }
        }
        // 炎: 近づくと勢い UP。
        if (m_Fire) {
            const f32 dx = mw.x - m_FirePos.x, dy = mw.y - m_FirePos.y;
            m_Fire->SetIntensity(Sqrt(dx * dx + dy * dy) < 2.5f ? 2.2f : 1.0f);
        }
    }

    void OnDrawHud(RenderContext& rc, FSpriteBatch& sb) noexcept override {
        sb.DrawRect(8.0f, 8.0f, 640.0f, 30.0f, FVec4{0.0f, 0.0f, 0.0f, 0.45f});
        if (rc.HasFont()) {
            sb.DrawString(rc.GetFont(),
                          "Effects2D  mouse=trail / drag-click on water=ripple / near fire=flare  [Esc]",
                          16.0f, 15.0f, FVec4{0.9f, 0.95f, 1.0f, 1.0f});
        }
    }

private:
    FWater2DComponent* m_Water  = nullptr;
    FFire2DComponent*  m_Fire   = nullptr;
    FNode2D*           m_Player = nullptr;
    FVec2              m_FirePos{0.0f, 0.0f};
};

class FEffectsGame final : public FGame {
protected:
    TUniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<FEffectsScene>();
    }
};

} // namespace

ACS_GAME_MAIN(FEffectsGame)
