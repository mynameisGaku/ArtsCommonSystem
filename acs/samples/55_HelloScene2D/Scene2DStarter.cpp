// SPDX-License-Identifier: Apache-2.0
// HelloScene2D - ACS 2D ゲームの実用的な開始例。
//
// AScene 基盤の次の機能を示す:
//   - ルートノードツリー
//   - FRenderContext 経由で共有する SpriteBatch
//   - ASprite2DComponent による描画
//   - APhysicsBody2D の collide-and-slide
//   - カメラ追従
#include "gameframework/GameFramework.h"
#include "platform/InputCodes.h"

using namespace acs;
using namespace acs::game;

namespace {

constexpr FActionId kMoveX("MoveX");
constexpr FActionId kMoveY("MoveY");
constexpr FActionId kQuit("Quit");

class AStarterScene final : public AScene
{
public:
    /** 2D の標準サービス構成 (Default2D | Camera2D | Physics2D) を要求する。 */
    ESvc WantedServices() const noexcept override { return kScene2DServices; }

    void OnReady() noexcept override
    {
        SetPixelsPerUnit(64.0f);

        FInputMap& input = Services().Input();
        input.ClearAll();
        input.BindAxisKeys(kMoveX, EKey::A, EKey::D);
        input.BindAxisKeys(kMoveY, EKey::S, EKey::W);
        input.BindKey(kQuit, EKey::Escape);

        CCollisionWorld2D& physics = Services().Physics();
        physics.Init(1.0f);
        physics.AddAabb(FAabb2{FVec2{0.0f, 2.25f}, FVec2{8.0f, 0.35f}});
        physics.AddAabb(FAabb2{FVec2{-4.25f, 0.0f}, FVec2{0.35f, 2.5f}});
        physics.AddAabb(FAabb2{FVec2{4.25f, 0.0f}, FVec2{0.35f, 2.5f}});

        AddBox(FVec2{0.0f, 2.25f}, FVec2{16.0f, 0.7f}, FVec4{0.25f, 0.28f, 0.34f, 1.0f});
        AddBox(FVec2{-4.25f, 0.0f}, FVec2{0.7f, 5.0f}, FVec4{0.22f, 0.24f, 0.30f, 1.0f});
        AddBox(FVec2{4.25f, 0.0f}, FVec2{0.7f, 5.0f}, FVec4{0.22f, 0.24f, 0.30f, 1.0f});

        auto player = NewObject<ANode>();
        player->SetPosition2D(FVec2{0.0f, 0.0f});
        player->AddComponent<ASprite2DComponent>(FVec2{0.9f, 0.9f}, FVec4{0.15f, 0.85f, 1.0f, 1.0f});
        APhysicsBody2D& body = player->AddComponent<APhysicsBody2D>(physics);
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
        if (my > 0.1f && m_Player->Position2D().y >= 1.30f)
        {
            m_PlayerBody->velocity.y = -7.0f;  // ジャンプ = 画面上 = 負の Y
        }
        Services().Camera().SetTargetPos(m_Player->Position2D(), 8.0f);
    }

    // 描画は batch を受け取らない引数なし版を使う。Draw* は «今の描画パス» へ
    // そのまま積まれる (gameframework/Draw.h)。
    void OnDrawWorld() noexcept override
    {
        // A small grid so camera/scale are obvious.
        for (i32 x = -8; x <= 8; ++x)
        {
            const FVec4 c = (x == 0) ? FVec4{0.25f, 0.35f, 0.45f, 0.65f} : FVec4{0.12f, 0.14f, 0.18f, 0.45f};
            DrawRect(static_cast<f32>(x) - 0.01f, -3.0f, 0.02f, 6.0f, c);
        }
        for (i32 y = -3; y <= 3; ++y)
        {
            const FVec4 c = (y == 0) ? FVec4{0.25f, 0.35f, 0.45f, 0.65f} : FVec4{0.12f, 0.14f, 0.18f, 0.45f};
            DrawRect(-8.0f, static_cast<f32>(y) - 0.01f, 16.0f, 0.02f, c);
        }
        // 円と線もテクスチャ無しで描ける (world 座標なので単位は world unit)。
        DrawCircleOutline(0.0f, 0.0f, 0.35f, FVec4{0.35f, 0.55f, 0.75f, 0.9f}, 0.02f);
    }

    void OnDrawHud() noexcept override
    {
        // HUD は画面ピクセル座標。
        DrawRect(12.0f, 12.0f, 360.0f, 54.0f, FVec4{0.0f, 0.0f, 0.0f, 0.45f});
        DrawRectOutline(12.0f, 12.0f, 360.0f, 54.0f, FVec4{1.0f, 1.0f, 1.0f, 0.25f}, 2.0f);
        // Text omitted intentionally: this starter has no font dependency.
    }

private:
    void AddBox(FVec2 pos, FVec2 size, FVec4 color) noexcept
    {
        auto n = NewObject<ANode>();
        n->SetPosition2D(pos);
        n->AddComponent<ASprite2DComponent>(size, color);
        Root().AddChild(Move(n));
    }

    ANode* m_Player = nullptr;
    APhysicsBody2D* m_PlayerBody = nullptr;
};

class CStarterGame final : public CGame
{
protected:
    TUniquePtr<AScene> InitialScene() noexcept override { return MakeUnique<AStarterScene>(); }
};

}; // namespace

ACS_GAME_MAIN(CStarterGame)
