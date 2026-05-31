// SPDX-License-Identifier: Apache-2.0
// HelloScene2D - practical starter for ACS 2D games.
//
// Demonstrates the new FScene2D base:
//   - Root node tree
//   - shared SpriteBatch via RenderContext
//   - FSprite2DComponent rendering
//   - FPhysicsBody2D collide-and-slide
//   - camera follow
#include "gameframework/GameFramework.h"
#include "platform/InputCodes.h"

using namespace acs;
using namespace acs::game;

namespace {

constexpr ActionId kMoveX("MoveX");
constexpr ActionId kMoveY("MoveY");
constexpr ActionId kQuit("Quit");

class FStarterScene final : public FScene2D
{
public:
    void OnReady() noexcept override
    {
        SetPixelsPerUnit(64.0f);

        FInputMap& input = Services().Input();
        input.ClearAll();
        input.BindAxisKeys(kMoveX, EKey::A, EKey::D);
        input.BindAxisKeys(kMoveY, EKey::S, EKey::W);
        input.BindKey(kQuit, EKey::Escape);

        FCollisionWorld2D& physics = Services().Physics();
        physics.Init(1.0f);
        physics.AddAabb(Aabb2{FVec2{0.0f, 2.25f}, FVec2{8.0f, 0.35f}});
        physics.AddAabb(Aabb2{FVec2{-4.25f, 0.0f}, FVec2{0.35f, 2.5f}});
        physics.AddAabb(Aabb2{FVec2{4.25f, 0.0f}, FVec2{0.35f, 2.5f}});

        AddBox(FVec2{0.0f, 2.25f}, FVec2{16.0f, 0.7f}, FVec4{0.25f, 0.28f, 0.34f, 1.0f});
        AddBox(FVec2{-4.25f, 0.0f}, FVec2{0.7f, 5.0f}, FVec4{0.22f, 0.24f, 0.30f, 1.0f});
        AddBox(FVec2{4.25f, 0.0f}, FVec2{0.7f, 5.0f}, FVec4{0.22f, 0.24f, 0.30f, 1.0f});

        auto player = MakeUnique<FNode2D>();
        player->Local().position = FVec2{0.0f, 0.0f};
        player->AddComponent<FSprite2DComponent>(FVec2{0.9f, 0.9f}, FVec4{0.15f, 0.85f, 1.0f, 1.0f});
        FPhysicsBody2D& body = player->AddComponent<FPhysicsBody2D>(physics);
        body.SetCircle(0.45f);
        body.gravity = FVec2{0.0f, 14.0f};  // +Y = 画面下 (重力は下向き = 正の Y)
        body.slide = true;
        m_PlayerBody = &body;
        m_Player = &Root().AddChild(Move(player));

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);
        GetGame().SetClearColor(0.04f, 0.045f, 0.055f);
    }

    void OnTick(f32 /*dt*/) noexcept override
    {
        if (Services().Input().IsPressed(kQuit))
        {
            GetGame().Quit();
            return;
        }
        if (!m_Player || !m_PlayerBody) return;
        const f32 mx = Services().Input().Axis(kMoveX);
        const f32 my = Services().Input().Axis(kMoveY);
        m_PlayerBody->velocity.x = mx * 4.0f;
        if (my > 0.1f && m_Player->Local().position.y >= 1.30f)
        {
            m_PlayerBody->velocity.y = -7.0f;  // ジャンプ = 画面上 = 負の Y
        }
        Services().Camera().SetTargetPos(m_Player->Local().position, 8.0f);
    }

    void OnDrawWorld(RenderContext& /*rc*/, FSpriteBatch& sb) noexcept override
    {
        // A small grid so camera/scale are obvious.
        for (i32 x = -8; x <= 8; ++x)
        {
            const FVec4 c = (x == 0) ? FVec4{0.25f, 0.35f, 0.45f, 0.65f} : FVec4{0.12f, 0.14f, 0.18f, 0.45f};
            sb.DrawRect(static_cast<f32>(x) - 0.01f, -3.0f, 0.02f, 6.0f, c);
        }
        for (i32 y = -3; y <= 3; ++y)
        {
            const FVec4 c = (y == 0) ? FVec4{0.25f, 0.35f, 0.45f, 0.65f} : FVec4{0.12f, 0.14f, 0.18f, 0.45f};
            sb.DrawRect(-8.0f, static_cast<f32>(y) - 0.01f, 16.0f, 0.02f, c);
        }
    }

    void OnDrawHud(RenderContext& rc, FSpriteBatch& sb) noexcept override
    {
        sb.DrawRect(12.0f, 12.0f, 360.0f, 54.0f, FVec4{0.0f, 0.0f, 0.0f, 0.45f});
        // Text omitted intentionally: this starter has no font dependency.
        (void)rc;
    }

private:
    void AddBox(FVec2 pos, FVec2 size, FVec4 color) noexcept
    {
        auto n = MakeUnique<FNode2D>();
        n->Local().position = pos;
        n->AddComponent<FSprite2DComponent>(size, color);
        Root().AddChild(Move(n));
    }

    FNode2D* m_Player = nullptr;
    FPhysicsBody2D* m_PlayerBody = nullptr;
};

class FStarterGame final : public FGame
{
protected:
    TUniquePtr<Scene> InitialScene() noexcept override { return MakeUnique<FStarterScene>(); }
};

}; // namespace

ACS_GAME_MAIN(FStarterGame)
