// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Player 実装。
#include "Player.h"
#include "GameplayScene.h"
#include "Bullets.h"

#include "platform/Input.h"
#include "foundation/Log.h"
#include "math/Math.h"

using namespace acs;
using namespace acs::game;

namespace hellofg {

void Player::Init(GameplayScene& scene, FNode2D& root, FHealthSystem& health) noexcept {
    auto player_up = MakeUnique<FNode2D>();
    player_up->Local().position = FVec2{0.0f, 0.0f};
    m_Node = &root.AddChild(Move(player_up));
    m_Node->_SetId(FNodeId{1u, static_cast<u8>(1)});

    m_HealthId = health.Spawn(kPlayerHp);
    m_Shape     = scene.Services().Physics().AddCircle(
                    Circle{FVec2{0.0f, 0.0f}, kPlayerRadius});
    m_FireCd   = 0.0f;
}

void Player::Shutdown() noexcept {
    // root subtree から自分の Node を外して解体する。Health / Shape は
    // GameplayScene::OnExit が ClearAll / Physics().ClearAll() でまとめて掃除する。
    if (m_Node) {
        m_Node->Destroy();
        m_Node = nullptr;
    }
    m_HealthId = FHealthId{};
    m_Shape     = FShapeId{};
    m_FireCd   = 0.0f;
}

FVec2 Player::Position() const noexcept {
    if (!m_Node) return FVec2{0.0f, 0.0f};
    return m_Node->Local().position;
}

bool Player::IsInvulnerable(const FHealthSystem& h) const noexcept {
    return h.IsInvulnerable(m_HealthId);
}

FVec2 Player::UpdateMovement(GameplayScene& scene, f32 dt) noexcept {
    if (!m_Node) return FVec2{0.0f, 0.0f};

    const FInputMap& im = scene.Services().Input();
    FVec2 move{ im.Axis(ActionId("MoveX")), im.Axis(ActionId("MoveY")) };
    if (move.x != 0.0f || move.y != 0.0f) {
        // 斜め入力で 1.41x 倍速にならないよう正規化。
        const f32 len = Length(move);
        if (len > 0.0f) move = move * (1.0f / len);
        m_Node->Local().position = m_Node->Local().position
            + move * (kPlayerSpeed * dt);

        // world 境界クランプ (半径ぶん内側に押し戻す)。
        FVec2& p = m_Node->Local().position;
        if (p.x < -kWorldHalfW + kPlayerRadius) p.x = -kWorldHalfW + kPlayerRadius;
        if (p.x >  kWorldHalfW - kPlayerRadius) p.x =  kWorldHalfW - kPlayerRadius;
        if (p.y < -kWorldHalfH + kPlayerRadius) p.y = -kWorldHalfH + kPlayerRadius;
        if (p.y >  kWorldHalfH - kPlayerRadius) p.y =  kWorldHalfH - kPlayerRadius;

        scene.Services().Physics().UpdateCircle(m_Shape,
                                                Circle{p, kPlayerRadius});
    }
    return m_Node->Local().position;
}

void Player::UpdateFire(GameplayScene& scene, f32 dt) noexcept {
    m_FireCd -= dt;
    if (!m_Node) return;
    const FInputMap& im = scene.Services().Input();
    if (im.IsHeld(ActionId("Fire")) && m_FireCd <= 0.0f) {
        const FVec2 from = m_Node->Local().position;
        const FVec2 dir  = Normalize(scene.MouseWorld() - from);
        if (LengthSq(dir) > 0.0f) {
            scene.GetBullets().Fire(scene, from, dir);
            m_FireCd = kFireCooldown;
        }
    }
}

bool Player::TryTakeContactDamage(GameplayScene& scene,
                                  FHealthSystem& health,
                                  FVec2 /*player_pos*/) noexcept {
    // この関数は Enemy 側で接触判定したあと呼ばれる。無敵中なら何もしない。
    if (health.IsInvulnerable(m_HealthId)) return false;

    const bool lethal = health.ApplyDamage(m_HealthId, 1.0f, EDamageType::Physical);
    health.SetInvulnerable(m_HealthId, kPlayerInvuln);

    // フィードバックはシーン経由で hit effects に。
    scene.OnPlayerHurt();

    ACS_LOG_INFO("[Gameplay] player hit! hp=%.0f",
                 static_cast<double>(health.GetCurrentHp(m_HealthId)));

    return lethal || health.GetCurrentHp(m_HealthId) <= 0.0f;
}

} // namespace hellofg
