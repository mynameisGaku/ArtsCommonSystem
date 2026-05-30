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

        // --- 形状ちがいの水域を 3 つ (world +Y = 画面下) ---
        // 海 (矩形、画面下端)。
        {
            auto n = MakeUnique<FNode2D>();
            n->Local().position = FVec2{0.0f, 0.0f};
            auto& w = n->AddComponent<FWater2DComponent>();
            w.SetRect(FVec2{0.0f, 5.0f}, FVec2{11.0f, 2.0f});
            w.SetWaves(0.16f, 1.6f);
            w.SetDepthColors(FVec3{0.22f, 0.55f, 0.78f}, FVec3{0.03f, 0.14f, 0.30f}); // 浅→深
            w.SetDepthAlpha(0.60f, 0.95f);
            w.SetFoam(FVec3{0.95f, 0.98f, 1.0f}, 0.0f, 3.0f, 0.9f);                   // 陸際の白泡
            w.SetRim(FVec3{0.60f, 0.85f, 1.0f}, 0.0f, 0.5f);                          // 水際の縁
            w.SetReflection(true, FVec3{0.85f, 0.92f, 1.0f}, 0.5f);                   // 平面反射
            w.SetReflectionDistortion(1.2f);
            m_Waters[m_WaterCount++] = &w;
            Root().AddChild(Move(n));
        }
        // 水溜まり (楕円)。
        {
            auto n = MakeUnique<FNode2D>();
            n->Local().position = FVec2{4.8f, 0.8f};
            auto& w = n->AddComponent<FWater2DComponent>();
            w.SetEllipse(FVec2{0.0f, 0.0f}, 2.2f, 1.1f, 28);
            w.SetColor(FVec3{0.15f, 0.45f, 0.66f});
            w.SetWaves(0.08f, 2.2f);
            m_Waters[m_WaterCount++] = &w;
            Root().AddChild(Move(n));
        }
        // 川 (スプライン: 少ない制御点で曲がりくねった帯)。
        {
            auto n = MakeUnique<FNode2D>();
            n->Local().position = FVec2{0.0f, 0.0f};
            auto& w = n->AddComponent<FWater2DComponent>();
            const FVec2 ctrl[5] = {
                FVec2{-10.0f, -3.2f}, FVec2{-5.0f, -1.6f}, FVec2{0.0f, -3.4f},
                FVec2{ 5.0f, -1.2f}, FVec2{10.0f, -2.6f},
            };
            w.SetSplineRiver(ctrl, 5, 1.3f, 10);
            w.SetColor(FVec3{0.12f, 0.42f, 0.68f});
            w.SetWaves(0.10f, 2.0f);
            w.SetFlow(FVec2{1.0f, -0.1f}, 2.4f);    // 川は左→右に流れる
            m_Waters[m_WaterCount++] = &w;
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

        // 海の上に立つ柱 (反射の確認用、海面より上 = 小さい Y)。
        {
            auto n = MakeUnique<FNode2D>();
            n->Local().position = FVec2{0.0f, 1.4f};
            n->AddComponent<FSprite2DComponent>(FVec2{0.7f, 3.2f}, FVec4{0.95f, 0.55f, 0.25f, 1.0f});
            Root().AddChild(Move(n));
        }

        SetReflectionEnabled(true);   // 3 パス描画 (world→RT→ swapchain で水が反射)
        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);
        GetGame().SetClearColor(0.05f, 0.06f, 0.09f);
    }

    void OnTick(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(EKey::Escape)) { GetGame().Quit(); return; }

        const FVec2 mw = ScreenToWorld(Input::MousePos());   // ppu 対応のピッキング
        if (m_Player) m_Player->Local().position = mw;

        // 水: なぞると波紋、左クリックで splash (各水域に対して)。
        const FVec2 md = Input::MouseDelta();
        const f32 sp = Sqrt(md.x * md.x + md.y * md.y);          // px/frame
        const bool click = Input::IsMouseButtonPressed(EMouseButton::Left);
        for (u32 i = 0; i < m_WaterCount; ++i) {
            FWater2DComponent* w = m_Waters[i];
            if (sp > 1.0f && w->ContainsPoint(mw)) w->Disturb(mw.x, 0.05f + sp * 0.0006f);
            if (click && w->ContainsPoint(mw))     w->Disturb(mw.x, 0.55f);
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
                          "Effects2D  sea/puddle/spline-river (depth+foam+rim)  mouse=trail, drag/click water=ripple, near fire=flare  [Esc]",
                          16.0f, 15.0f, FVec4{0.9f, 0.95f, 1.0f, 1.0f});
        }
    }

private:
    FWater2DComponent* m_Waters[3] = { nullptr, nullptr, nullptr };
    u32                m_WaterCount = 0;
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
