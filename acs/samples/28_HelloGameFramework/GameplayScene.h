// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Gameplay scene。本サンプルの中核。
//
// Phase 5: Node2D ツリー (root → wheel → spoke[0/1] + rotator)
// Phase 7: Component2D 経由の composition (RotateComponent)
// Phase 8: SceneServices (Default2D | Camera2D | Physics2D) を宣言、Tween /
//          Sequence / Input / Camera / Physics を Services() 経由で取得
// Phase 9: Camera2D follow + screen shake (trauma)
// Phase 10/11: CollisionWorld2D + PhysicsBody2D で重力で落下する ball
#pragma once

#include "gameframework/GameFramework.h"
#include "math/Vec.h"

namespace hellogf {

class RotatingNode;

class GameplayScene : public acs::game::Scene {
public:
    // Phase 8+9+10: services 宣言。Default2D + Camera2D + Physics2D。
    acs::game::ESvc WantedServices() const noexcept override {
        return acs::game::ESvc::Default2D
             | acs::game::ESvc::Camera2D
             | acs::game::ESvc::Physics2D;
    }

    void OnEnter()                  noexcept override;
    void OnExit()                   noexcept override;
    void OnPause()                  noexcept override;
    void OnResume()                 noexcept override;
    void OnUpdate(acs::f32 dt)      noexcept override;
    void OnFixedUpdate(acs::f32 dt) noexcept override;

private:
    // Tween/Clock/Sequences/Input は Services() 経由 (member 不要)
    acs::Vec3                _color {0.05f, 0.20f, 0.10f};
    acs::game::TweenHandle   _color_tween;
    bool                     _to_bright = true;
    acs::f32                 _fixed_secs = 0.0f;
    acs::u32                 _fixed_step_log_counter = 0;

    // Phase 5: Node2D ツリー (root → wheel → spoke[0/1])
    acs::game::Node2D        _root;
    RotatingNode*            _wheel    = nullptr;
    RotatingNode*            _spoke[2] = {nullptr, nullptr};
    // Phase 7: composition 版 (Node2D + RotateComponent)
    acs::game::Node2D*       _rotator  = nullptr;
    // Phase 10: CollisionWorld2D shape handles (spoke[0/1] の world pos に追従する円)
    acs::game::ShapeId       _spoke_shape[2];
    // Phase 11: 落下 ball + 床
    acs::game::Node2D*       _ball         = nullptr;        // 弱参照
    acs::game::ShapeId       _ground_shape;

    static constexpr acs::Vec3 kColorDark  {0.05f, 0.20f, 0.10f};
    static constexpr acs::Vec3 kColorBright{0.10f, 0.32f, 0.18f};
};

} // namespace hellogf
