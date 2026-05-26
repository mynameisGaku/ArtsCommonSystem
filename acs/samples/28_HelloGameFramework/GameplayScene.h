// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Gameplay scene。本サンプルの中核。
//
// ここで Node2D ツリー (root → wheel → spoke + rotator) を構築し、
// Tween / FCamera follow + screen shake / CollisionWorld2D / PhysicsBody2D を
// SceneServices() 経由で扱う。各機能の詳細は実装側 (cpp) の節コメントを参照。
#pragma once

#include "gameframework/GameFramework.h"
#include "math/Vec.h"

namespace hellogf {

class RotatingNode;

class GameplayScene : public acs::game::Scene {
public:
    // SceneServices に Default2D + Camera2D + Physics2D を要求する宣言。
    // フレームワークが対応する subsystem を構築 / tick してくれる。
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
    // Tween / Clock / Sequences / Input は Services() 経由で取得するので member は不要。
    acs::FVec3                _color {0.05f, 0.20f, 0.10f};
    acs::game::TweenHandle   _color_tween;
    bool                     _to_bright = true;
    acs::f32                 _fixed_secs = 0.0f;
    acs::u32                 _fixed_step_log_counter = 0;

    // Node2D ツリー (root → wheel → spoke[0/1])。
    acs::game::Node2D        _root;
    RotatingNode*            _wheel    = nullptr;
    RotatingNode*            _spoke[2] = {nullptr, nullptr};
    // composition 版 (プレーン Node2D + RotateComponent attach)。
    acs::game::Node2D*       _rotator  = nullptr;
    // spoke[0/1] の world pos に毎フレーム追従させる CollisionWorld 上の円。
    acs::game::FShapeId       _spoke_shape[2];
    // 重力で落下する ball (弱参照、ツリーの所有) と静的 ground。
    acs::game::Node2D*       _ball         = nullptr;
    acs::game::FShapeId       _ground_shape;

    static constexpr acs::FVec3 kColorDark  {0.05f, 0.20f, 0.10f};
    static constexpr acs::FVec3 kColorBright{0.10f, 0.32f, 0.18f};
};

} // namespace hellogf
