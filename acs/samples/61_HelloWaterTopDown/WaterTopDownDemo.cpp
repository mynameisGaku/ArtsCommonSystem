// SPDX-License-Identifier: Apache-2.0
// HelloWaterTopDown - 見下ろし (Core Keeper 風) の水面デモ。
//
//   ・大きな湖 + 小さな池。縁が浅く中心が深い radial 深度、揺らめくコースティクス
//     (光の網目)、全周の岸泡、散発的なきらめき。
//   ・マウスでなぞる/クリックすると、その点から放射状の波紋 (輪) が広がる。
//   ・水面には時々「雨だれ」の波紋が自動で立つ。
//
// すべて FWater2DComponent (EWaterStyle::TopDown) の手続き生成。HLSL 不要。
#include "gameframework/GameFramework.h"
#include "platform/Input.h"
#include "platform/InputCodes.h"
#include "math/Math.h"

using namespace acs;
using namespace acs::game;

namespace {

class FLakeScene final : public FScene2D {
public:
    void OnReady() noexcept override {
        SetPixelsPerUnit(48.0f);

        // --- 大きな湖 (中央)。格子メッシュなのでコースティクスが細かく出る ---
        {
            auto n = MakeUnique<FNode2D>();
            n->Local().position = FVec2{0.5f, 0.6f};
            auto& w = n->AddComponent<FWater2DComponent>();
            w.SetStyle(EWaterStyle::TopDown);
            w.SetMeshResolution(22, 11);   // 細かい格子 = くっきりしたコースティクス
            w.SetRect(FVec2{0.0f, 0.0f}, FVec2{6.6f, 3.4f});
            w.SetDepthColors(FVec3{0.24f, 0.62f, 0.74f}, FVec3{0.02f, 0.15f, 0.36f}); // 縁=浅, 中心=深
            w.SetDepthAlpha(0.95f, 1.0f);
            w.SetCaustics(FVec3{0.5f, 0.82f, 1.0f}, 0.55f, 2.4f, 0.7f);                // 光の網目
            w.EnableGlints(true);
            w.SetFoam(FVec3{0.96f, 0.99f, 1.0f}, 0.0f, 2.0f, 0.85f);                   // 全周の岸泡
            w.SetSkyTint(FVec3{0.40f, 0.60f, 0.90f}, 0.05f);                           // 薄い空色
            w.SetWaves(0.04f, 1.2f);
            w.SetRipplePropagation(3.4f, 0.8f);
            m_Waters[m_Count++] = &w;
            Root().AddChild(Move(n));
        }
        // --- 小さな池 (左上、楕円) ---
        {
            auto n = MakeUnique<FNode2D>();
            n->Local().position = FVec2{-8.0f, -3.0f};
            auto& w = n->AddComponent<FWater2DComponent>();
            w.SetStyle(EWaterStyle::TopDown);
            w.SetMeshResolution(44, 7);
            w.SetEllipse(FVec2{0.0f, 0.0f}, 2.4f, 1.7f, 36);
            w.SetDepthColors(FVec3{0.26f, 0.64f, 0.76f}, FVec3{0.03f, 0.18f, 0.38f});
            w.SetDepthAlpha(0.95f, 1.0f);
            w.SetCaustics(FVec3{0.55f, 0.85f, 1.0f}, 0.6f, 2.6f, 0.85f);
            w.EnableGlints(true);
            w.SetFoam(FVec3{0.96f, 0.99f, 1.0f}, 0.0f, 2.4f, 0.85f);
            w.SetSkyTint(FVec3{0.40f, 0.60f, 0.90f}, 0.06f);
            w.SetWaves(0.05f, 1.4f);
            m_Waters[m_Count++] = &w;
            Root().AddChild(Move(n));
        }
        // --- 蛇行する小川 (下、見下ろし) ---
        {
            auto n = MakeUnique<FNode2D>();
            n->Local().position = FVec2{0.0f, 0.0f};
            auto& w = n->AddComponent<FWater2DComponent>();
            w.SetStyle(EWaterStyle::TopDown);
            const FVec2 ctrl[5] = {
                FVec2{-10.5f, 5.4f}, FVec2{-5.0f, 4.4f}, FVec2{0.0f, 5.6f},
                FVec2{ 5.0f, 4.6f}, FVec2{10.5f, 5.2f},
            };
            w.SetSplineRiver(ctrl, 5, 1.6f, 10);
            w.SetDepthColors(FVec3{0.24f, 0.60f, 0.74f}, FVec3{0.03f, 0.18f, 0.38f});
            w.SetDepthAlpha(0.95f, 1.0f);
            w.SetCaustics(FVec3{0.55f, 0.85f, 1.0f}, 0.55f, 2.8f, 0.9f);
            w.SetFoam(FVec3{0.96f, 0.99f, 1.0f}, 0.0f, 2.2f, 0.85f);
            w.SetFlow(FVec2{1.0f, 0.0f}, 1.8f);
            m_Waters[m_Count++] = &w;
            Root().AddChild(Move(n));
        }
        // プレイヤー = マウス追従ドット。
        {
            auto n = MakeUnique<FNode2D>();
            n->AddComponent<FSprite2DComponent>(FVec2{0.30f, 0.30f},
                                                FVec4{1.0f, 0.85f, 0.4f, 1.0f});
            m_Player = &Root().AddChild(Move(n));
        }

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);
        GetGame().SetClearColor(0.10f, 0.09f, 0.07f);   // 土っぽい背景 (見下ろし)
    }

    void OnTick(f32 dt) noexcept override {
        if (Input::IsKeyPressed(EKey::Escape)) { GetGame().Quit(); return; }
        m_Time += dt;

        const FVec2 mw = ScreenToWorld(Input::MousePos());
        if (m_Player) m_Player->Local().position = mw;

        // なぞる/クリックで放射状リップル。
        const FVec2 md = Input::MouseDelta();
        const f32 sp = Sqrt(md.x * md.x + md.y * md.y);
        const bool click = Input::IsMouseButtonPressed(EMouseButton::Left);
        for (u32 i = 0; i < m_Count; ++i) {
            FWater2DComponent* w = m_Waters[i];
            if (!w->ContainsPoint(mw)) continue;
            if (click)          w->Disturb(mw, 0.6f);
            else if (sp > 2.0f) w->Disturb(mw, 0.12f + sp * 0.0008f);
        }

        // 自動の「雨だれ」: 一定間隔で散らした位置に小さな輪。
        m_DripTimer += dt;
        if (m_DripTimer > 0.6f) {
            m_DripTimer = 0.0f;
            const FVec2 p{ Cos(m_Time * 2.3f) * 4.5f + 0.5f, Sin(m_Time * 1.7f) * 2.2f + 0.6f };
            if (m_Count > 0) m_Waters[0]->Disturb(p, 0.3f);
        }
    }

    void OnDrawHud(RenderContext& rc, FSpriteBatch& sb) noexcept override {
        sb.DrawRect(8.0f, 8.0f, 660.0f, 30.0f, FVec4{0.0f, 0.0f, 0.0f, 0.45f});
        if (rc.HasFont()) {
            sb.DrawString(rc.GetFont(),
                          "WaterTopDown (Core Keeper style)  caustics + shore foam + radial depth   "
                          "mouse=ripple, click=splash  [Esc]",
                          16.0f, 15.0f, FVec4{0.9f, 0.95f, 1.0f, 1.0f});
        }
    }

private:
    FWater2DComponent* m_Waters[3] = { nullptr, nullptr, nullptr };
    u32                m_Count = 0;
    FNode2D*           m_Player = nullptr;
    f32                m_Time = 0.0f;
    f32                m_DripTimer = 0.0f;
};

class FLakeGame final : public FGame {
protected:
    TUniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<FLakeScene>();
    }
};

} // namespace

ACS_GAME_MAIN(FLakeGame)
