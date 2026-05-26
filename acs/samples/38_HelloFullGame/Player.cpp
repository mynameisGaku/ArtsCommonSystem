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
    _node = &root.AddChild(Move(player_up));
    _node->_SetId(FNodeId{1u, static_cast<u8>(1)});

    _health_id = health.Spawn(kPlayerHp);
    _shape     = scene.Services().Physics().AddCircle(
                    FCircle{FVec2{0.0f, 0.0f}, kPlayerRadius});
    _fire_cd   = 0.0f;
}

void Player::Shutdown() noexcept {
    // root subtree から自分の FNode を外して解体する。Health / Shape は
    // GameplayScene::OnExit が ClearAll / Physics().ClearAll() でまとめて掃除する。
    if (_node) {
        _node->Destroy();
        _node = nullptr;
    }
    _health_id = FHealthId{};
    _shape     = FShapeId{};
    _fire_cd   = 0.0f;
}

FVec2 Player::Position() const noexcept {
    if (!_node) return FVec2{0.0f, 0.0f};
    return _node->Local().position;
}

bool Player::IsInvulnerable(const FHealthSystem& h) const noexcept {
    return h.IsInvulnerable(_health_id);
}

FVec2 Player::UpdateMovement(GameplayScene& scene, f32 dt) noexcept {
    if (!_node) return FVec2{0.0f, 0.0f};

    const FInputMap& im = scene.Services().Input();
    FVec2 move{ im.Axis(FActionId("MoveX")), im.Axis(FActionId("MoveY")) };
    if (move.x != 0.0f || move.y != 0.0f) {
        // 斜め入力で 1.41x 倍速にならないよう正規化。
        const f32 len = Length(move);
        if (len > 0.0f) move = move * (1.0f / len);
        _node->Local().position = _node->Local().position
            + move * (kPlayerSpeed * dt);

        // world 境界クランプ (半径ぶん内側に押し戻す)。
        FVec2& p = _node->Local().position;
        if (p.x < -kWorldHalfW + kPlayerRadius) p.x = -kWorldHalfW + kPlayerRadius;
        if (p.x >  kWorldHalfW - kPlayerRadius) p.x =  kWorldHalfW - kPlayerRadius;
        if (p.y < -kWorldHalfH + kPlayerRadius) p.y = -kWorldHalfH + kPlayerRadius;
        if (p.y >  kWorldHalfH - kPlayerRadius) p.y =  kWorldHalfH - kPlayerRadius;

        scene.Services().Physics().UpdateCircle(_shape,
                                                FCircle{p, kPlayerRadius});
    }
    return _node->Local().position;
}

void Player::UpdateFire(GameplayScene& scene, f32 dt) noexcept {
    _fire_cd -= dt;
    if (!_node) return;
    const FInputMap& im = scene.Services().Input();
    if (im.IsHeld(FActionId("Fire")) && _fire_cd <= 0.0f) {
        const FVec2 from = _node->Local().position;
        const FVec2 dir  = Normalize(scene.MouseWorld() - from);
        if (LengthSq(dir) > 0.0f) {
            scene.GetBullets().Fire(scene, from, dir);
            _fire_cd = kFireCooldown;
        }
    }
}

bool Player::TryTakeContactDamage(GameplayScene& scene,
                                  FHealthSystem& health,
                                  FVec2 /*player_pos*/) noexcept {
    // この関数は Enemy 側で接触判定したあと呼ばれる。無敵中なら何もしない。
    if (health.IsInvulnerable(_health_id)) return false;

    const bool lethal = health.ApplyDamage(_health_id, 1.0f, EDamageType::Physical);
    health.SetInvulnerable(_health_id, kPlayerInvuln);

    // フィードバックはシーン経由で hit effects に。
    scene.OnPlayerHurt();

    ACS_LOG_INFO("[Gameplay] player hit! hp=%.0f",
                 static_cast<double>(health.GetCurrentHp(_health_id)));

    return lethal || health.GetCurrentHp(_health_id) <= 0.0f;
}

} // namespace hellofg
