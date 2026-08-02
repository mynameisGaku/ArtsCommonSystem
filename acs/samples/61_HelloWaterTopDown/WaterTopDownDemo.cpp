// SPDX-License-Identifier: Apache-2.0
// HelloWaterTopDown - 見下ろし (Core Keeper 風) の水面デモ。
//
//   ・大きな湖 + 小さな池。縁が浅く中心が深い radial 深度、揺らめくコースティクス
//     (光の網目)、全周の岸泡、散発的なきらめき。
//   ・マウスでなぞる/クリックすると、その点から放射状の波紋 (輪) が広がる。
//   ・水面には時々「雨だれ」の波紋が自動で立つ。
//
// AWater2DComponent (EWaterStyle::TopDown) の GPU プロシージャル水面。
#include "gameframework/GameFramework.h"
#include "gameframework/PrimitiveRenderer2D.h"
#include "platform/Input.h"
#include "platform/InputCodes.h"
#include "math/Math.h"

using namespace acs;
using namespace acs::game;

namespace {

class ALakeScene final : public AScene {
public:
    /** 2D の標準サービス構成 (Default2D | Camera2D | Physics2D) を要求する。 */
    ESvc WantedServices() const noexcept override { return kScene2DServices; }

    void OnReady() noexcept override {
        SetPixelsPerUnit(48.0f);
        SetReflectionEnabled(true);
        SetWaterSceneSamplingEnabled(true);

        const auto addPrimitive = [this](APrimitiveRenderer2D::EShape shape,
                                         FVec2 position, FVec2 size,
                                         FVec4 color, f32 rotation) noexcept {
            auto n = NewObject<ANode>();
            n->SetPosition2D(position);
            n->SetRotation2D(rotation);
            auto& p = n->AddComponent<APrimitiveRenderer2D>();
            p.SetShape(shape);
            p.SetSize(size);
            p.SetColor(color);
            Root().AddChild(Move(n));
        };

        // --- 実シーン屈折の読みやすい水底/地形。すべて水より先に描く。 ---
        // 大地を一枚の暗色で済ませず、砂州・地層・小石を捕捉 scene color に残す。
        addPrimitive(APrimitiveRenderer2D::EShape::Box,
                     FVec2{0.0f, 0.0f}, FVec2{31.0f, 18.0f},
                     FVec4{0.17f, 0.135f, 0.085f, 1.0f}, 0.0f);
        addPrimitive(APrimitiveRenderer2D::EShape::Circle,
                     FVec2{0.4f, 0.35f}, FVec2{13.8f, 7.7f},
                     FVec4{0.34f, 0.285f, 0.18f, 1.0f}, -0.04f);
        addPrimitive(APrimitiveRenderer2D::EShape::Circle,
                     FVec2{-8.8f, -3.6f}, FVec2{5.45f, 4.05f},
                     FVec4{0.37f, 0.31f, 0.19f, 1.0f}, 0.08f);
        addPrimitive(APrimitiveRenderer2D::EShape::Box,
                     FVec2{0.0f, 5.05f}, FVec2{23.5f, 2.25f},
                     FVec4{0.32f, 0.255f, 0.15f, 1.0f}, -0.01f);

        // 水底の細い地層線。法線で曲がるため静止画でも屈折量が読める。
        const FVec4 strata{0.18f, 0.145f, 0.09f, 0.42f};
        addPrimitive(APrimitiveRenderer2D::EShape::Box,
                     FVec2{-2.9f, -1.1f}, FVec2{4.0f, 0.11f}, strata, 0.20f);
        addPrimitive(APrimitiveRenderer2D::EShape::Box,
                     FVec2{ 1.7f, -1.5f}, FVec2{4.6f, 0.12f}, strata, -0.16f);
        addPrimitive(APrimitiveRenderer2D::EShape::Box,
                     FVec2{-1.0f,  1.2f}, FVec2{5.2f, 0.10f}, strata, 0.09f);
        addPrimitive(APrimitiveRenderer2D::EShape::Box,
                     FVec2{ 3.5f,  1.6f}, FVec2{2.7f, 0.10f}, strata, -0.26f);
        addPrimitive(APrimitiveRenderer2D::EShape::Box,
                     FVec2{-8.9f, -3.7f}, FVec2{3.2f, 0.09f}, strata, 0.28f);

        const FVec4 submergedStone{0.24f, 0.255f, 0.22f, 1.0f};
        addPrimitive(APrimitiveRenderer2D::EShape::Circle,
                     FVec2{-3.7f, 0.2f}, FVec2{0.72f, 0.54f},
                     submergedStone, 0.0f);
        addPrimitive(APrimitiveRenderer2D::EShape::Circle,
                     FVec2{ 2.6f, 0.7f}, FVec2{0.95f, 0.62f},
                     FVec4{0.20f, 0.22f, 0.19f, 1.0f}, 0.0f);
        addPrimitive(APrimitiveRenderer2D::EShape::Circle,
                     FVec2{ 4.1f,-1.0f}, FVec2{0.58f, 0.46f},
                     FVec4{0.29f, 0.28f, 0.22f, 1.0f}, 0.0f);
        addPrimitive(APrimitiveRenderer2D::EShape::Circle,
                     FVec2{-8.3f,-3.4f}, FVec2{0.62f, 0.44f},
                     submergedStone, 0.0f);

        // --- 大きな湖 (中央)。滑らかな自然輪郭 + GPU 水面 ---
        {
            auto n = NewObject<ANode>();
            n->SetPosition2D(FVec2{0.4f, 0.3f});
            auto& w = n->AddComponent<AWater2DComponent>();
            w.SetStyle(EWaterStyle::TopDown);
            w.SetMeshResolution(0, 3);
            const FVec2 shore[12] = {
                {-6.4f,-0.7f}, {-5.0f,-2.3f}, {-2.7f,-3.0f}, { 0.2f,-2.8f},
                { 3.2f,-2.5f}, { 5.7f,-1.4f}, { 6.4f, 0.5f}, { 5.5f, 2.5f},
                { 3.0f, 3.3f}, { 0.1f, 3.5f}, {-3.0f, 3.0f}, {-5.8f, 1.7f},
            };
            w.SetSplineRegion(shore, 12, 8);
            w.SetDepthColors(FVec3{0.075f, 0.42f, 0.52f}, FVec3{0.006f, 0.065f, 0.22f});
            w.SetDepthAlpha(0.70f, 0.95f);
            w.SetCaustics(FVec3{0.46f, 0.76f, 0.94f}, 0.24f, 2.2f, 0.62f);
            w.EnableGlints(true);
            w.SetFoam(FVec3{0.78f, 0.90f, 0.94f}, 0.11f, 1.35f, 0.30f);
            w.SetSkyTint(FVec3{0.24f, 0.46f, 0.72f}, 0.10f);
            w.SetSurfaceOptics(0.18f, 0.90f, 0.085f, 0.42f);
            w.SetAbsorption(FVec3{0.42f, 0.17f, 0.050f});
            w.SetFlow(FVec2{0.92f, 0.38f}, 1.05f);
            w.SetWaves(0.045f, 1.05f);
            w.SetRipplePropagation(3.4f, 0.8f);
            m_Waters[m_Count++] = &w;
            Root().AddChild(Move(n));
        }
        // --- 小さな池 (左上、楕円) ---
        {
            auto n = NewObject<ANode>();
            n->SetPosition2D(FVec2{-8.8f, -3.6f});
            auto& w = n->AddComponent<AWater2DComponent>();
            w.SetStyle(EWaterStyle::TopDown);
            w.SetMeshResolution(44, 7);
            w.SetEllipse(FVec2{0.0f, 0.0f}, 2.4f, 1.7f, 36);
            w.SetDepthColors(FVec3{0.10f, 0.48f, 0.57f}, FVec3{0.012f, 0.10f, 0.28f});
            w.SetDepthAlpha(0.66f, 0.92f);
            w.SetCaustics(FVec3{0.50f, 0.79f, 0.96f}, 0.27f, 2.5f, 0.75f);
            w.EnableGlints(true);
            w.SetFoam(FVec3{0.82f, 0.93f, 0.96f}, 0.075f, 1.55f, 0.27f);
            w.SetSkyTint(FVec3{0.28f, 0.52f, 0.76f}, 0.09f);
            w.SetSurfaceOptics(0.22f, 0.78f, 0.10f, 0.36f);
            w.SetAbsorption(FVec3{0.36f, 0.14f, 0.045f});
            w.SetWaves(0.05f, 1.4f);
            m_Waters[m_Count++] = &w;
            Root().AddChild(Move(n));
        }
        // --- 蛇行する小川 (下、見下ろし) ---
        {
            auto n = NewObject<ANode>();
            n->SetPosition2D(FVec2{0.0f, 0.0f});
            auto& w = n->AddComponent<AWater2DComponent>();
            w.SetStyle(EWaterStyle::TopDown);
            const FVec2 ctrl[5] = {
                FVec2{-10.5f, 5.4f}, FVec2{-5.0f, 4.4f}, FVec2{0.0f, 5.6f},
                FVec2{ 5.0f, 4.6f}, FVec2{10.5f, 5.2f},
            };
            w.SetSplineRiver(ctrl, 5, 1.6f, 10);
            w.SetDepthColors(FVec3{0.09f, 0.45f, 0.56f}, FVec3{0.012f, 0.10f, 0.27f});
            w.SetDepthAlpha(0.67f, 0.91f);
            w.SetCaustics(FVec3{0.50f, 0.79f, 0.95f}, 0.25f, 2.7f, 0.78f);
            w.SetFoam(FVec3{0.80f, 0.91f, 0.95f}, 0.06f, 1.6f, 0.25f);
            w.SetSurfaceOptics(0.24f, 0.72f, 0.12f, 0.32f);
            w.SetAbsorption(FVec3{0.34f, 0.13f, 0.042f});
            w.SetFlow(FVec2{1.0f, 0.0f}, 1.8f);
            m_Waters[m_Count++] = &w;
            Root().AddChild(Move(n));
        }

        // --- 岸辺オブジェクト。水より後に描くが scene capture にも入り、画面反射へ映る。 ---
        const FVec4 shoreRock{0.31f, 0.30f, 0.25f, 1.0f};
        addPrimitive(APrimitiveRenderer2D::EShape::Circle,
                     FVec2{-5.8f,-1.65f}, FVec2{1.05f, 0.72f}, shoreRock, 0.0f);
        addPrimitive(APrimitiveRenderer2D::EShape::Circle,
                     FVec2{ 5.7f, 1.55f}, FVec2{0.88f, 0.64f},
                     FVec4{0.36f, 0.34f, 0.27f, 1.0f}, 0.0f);
        addPrimitive(APrimitiveRenderer2D::EShape::Circle,
                     FVec2{-10.4f,-3.8f}, FVec2{0.76f, 0.58f}, shoreRock, 0.0f);
        addPrimitive(APrimitiveRenderer2D::EShape::Triangle,
                     FVec2{-4.9f, 2.65f}, FVec2{0.34f, 0.95f},
                     FVec4{0.28f, 0.44f, 0.20f, 1.0f}, -0.22f);
        addPrimitive(APrimitiveRenderer2D::EShape::Triangle,
                     FVec2{ 4.8f,-1.9f}, FVec2{0.30f, 0.86f},
                     FVec4{0.25f, 0.40f, 0.18f, 1.0f}, 0.26f);
        // プレイヤー = マウス追従ドット。
        {
            auto n = NewObject<ANode>();
            n->AddComponent<ASprite2DComponent>(FVec2{0.30f, 0.30f},
                                                FVec4{1.0f, 0.85f, 0.4f, 1.0f});
            m_Player = &Root().AddChild(Move(n));
        }

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);
        GetGame().SetClearColor(0.12f, 0.095f, 0.06f);
    }

    void OnTick(f32 dt) noexcept override {
        if (CInput::IsKeyPressed(EKey::Escape)) { GetGame().Quit(); return; }
        m_Time += dt;

        const FVec2 mw = ScreenToWorld(CInput::MousePos());
        if (m_Player) m_Player->SetPosition2D(mw);

        // カーソル/接触点の world 速度から連続 wake を生成する。
        const bool click = CInput::IsMouseButtonPressed(EMouseButton::Left);
        const bool dragging = CInput::IsMouseButtonDown(EMouseButton::Left);
        if (click) {
            for (u32 i = 0; i < m_Count; ++i) {
                AWater2DComponent* w = m_Waters[i];
                if (w->ContainsPoint(mw))
                    w->AddDisturbance(mw, 0.18f, 0.72f); // 強い着水衝撃
            }
        }

        if (m_HasLastMouse && dt > 1e-5f) {
            const FVec2 delta{mw.x - m_LastMouseWorld.x, mw.y - m_LastMouseWorld.y};
            const f32 travel = Sqrt(delta.x * delta.x + delta.y * delta.y);
            const FVec2 velocity{delta.x / dt, delta.y / dt};
            const f32 speed = travel / dt;
            const f32 spacing = dragging ? 0.16f : 0.27f;
            m_WakeTravel += travel;
            if (speed > 0.28f && m_WakeTravel >= spacing) {
                u32 emissions = static_cast<u32>(m_WakeTravel / spacing);
                if (emissions > 2) emissions = 2; // 1 frameの急移動でも過密にしない
                const f32 strengthBase = dragging ? 0.12f : 0.070f;
                f32 strength = strengthBase + speed * (dragging ? 0.0027f : 0.0017f);
                const f32 strengthMax = dragging ? 0.24f : 0.15f;
                if (strength > strengthMax) strength = strengthMax;
                f32 radius = (dragging ? 0.20f : 0.13f) + speed * 0.0014f;
                if (radius > 0.32f) radius = 0.32f;

                for (u32 emission = 0; emission < emissions; ++emission) {
                    const f32 u = static_cast<f32>(emission + 1)
                                / static_cast<f32>(emissions + 1);
                    const FVec2 p{m_LastMouseWorld.x + delta.x * u,
                                  m_LastMouseWorld.y + delta.y * u};
                    for (u32 i = 0; i < m_Count; ++i) {
                        AWater2DComponent* w = m_Waters[i];
                        if (w->ContainsPoint(p))
                            w->AddWake(p, velocity, radius, strength);
                    }
                }
                m_WakeTravel -= spacing * static_cast<f32>(emissions);
            }
        }
        m_LastMouseWorld = mw;
        m_HasLastMouse = true;

        // 自動の「雨だれ」: 一定間隔で散らした位置に小さな輪。
        m_DripTimer += dt;
        if (m_DripTimer > 0.6f) {
            m_DripTimer = 0.0f;
            const FVec2 p{ Cos(m_Time * 2.3f) * 4.5f + 0.5f, Sin(m_Time * 1.7f) * 2.2f + 0.6f };
            if (m_Count > 0) m_Waters[0]->AddDisturbance(p, 0.04f, 0.26f);
        }
    }

    void OnDrawHud(FRenderContext& rc, CSpriteBatch& sb) noexcept override {
        sb.DrawRect(8.0f, 8.0f, 1080.0f, 50.0f, FVec4{0.0f, 0.0f, 0.0f, 0.55f});
        if (rc.HasFont()) {
            sb.DrawString(rc.GetFont(),
                          "GPU Water  scene color/depth refraction + screen reflection + normal map + GGX   "
                          "[Esc]",
                          16.0f, 15.0f, FVec4{0.9f, 0.95f, 1.0f, 1.0f});
            const bool shaderReady = m_Count > 0 && m_Waters[0]->IsGpuSurfaceReady();
            sb.DrawString(rc.GetFont(),
                          shaderReady
                              ? "SHADER: OK   move=continuous wake   drag=directional wake   click=impact"
                              : "SHADER: FALLBACK (water effect creation failed)",
                          16.0f, 35.0f,
                          shaderReady ? FVec4{0.56f, 1.0f, 0.72f, 1.0f}
                                      : FVec4{1.0f, 0.28f, 0.24f, 1.0f});
        }
    }

private:
    AWater2DComponent* m_Waters[3] = { nullptr, nullptr, nullptr };
    u32                m_Count = 0;
    ANode*           m_Player = nullptr;
    f32                m_Time = 0.0f;
    f32                m_DripTimer = 0.0f;
    FVec2              m_LastMouseWorld{0.0f, 0.0f};
    f32                m_WakeTravel = 0.0f;
    bool               m_HasLastMouse = false;
};

class CLakeGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override {
        return MakeUnique<ALakeScene>();
    }
};

} // namespace

ACS_GAME_MAIN(CLakeGame)
