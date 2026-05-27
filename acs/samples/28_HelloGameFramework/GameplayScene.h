// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — Gameplay scene。本サンプルの中核。
//
// ここで FNode2D ツリー (root → wheel → spoke + rotator) を構築し、
// FTween / FCamera follow + screen shake / FCollisionWorld2D / FPhysicsBody2D を
// FSceneServices() 経由で扱う。各機能の詳細は実装側 (cpp) の節コメントを参照。
#pragma once

#include "gameframework/GameFramework.h"
#include "math/Vec.h"

namespace hellogf {

class RotatingNode;

class GameplayScene : public acs::game::Scene {
public:
    // FSceneServices に Default2D + FCamera2D + Physics2D を要求する宣言。
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
    // FTween / Clock / Sequences / Input は Services() 経由で取得するので member は不要。
    acs::FVec3                m_Color {0.05f, 0.20f, 0.10f};
    acs::game::FTweenHandle   m_ColorTween;
    bool                     m_ToBright = true;
    acs::f32                 m_FixedSecs = 0.0f;
    acs::u32                 m_FixedStepLogCounter = 0;

    // FNode2D ツリー (root → wheel → spoke[0/1])。
    acs::game::FNode2D        m_Root;
    RotatingNode*            m_Wheel    = nullptr;
    RotatingNode*            m_Spoke[2] = {nullptr, nullptr};
    // composition 版 (プレーン FNode2D + RotateComponent attach)。
    acs::game::FNode2D*       m_Rotator  = nullptr;
    // spoke[0/1] の world pos に毎フレーム追従させる CollisionWorld 上の円。
    acs::game::FShapeId       m_SpokeShape[2];
    // 重力で落下する ball (弱参照、ツリーの所有) と静的 ground。
    acs::game::FNode2D*       m_Ball         = nullptr;
    acs::game::FShapeId       m_GroundShape;

    static constexpr acs::FVec3 kColorDark  {0.05f, 0.20f, 0.10f};
    static constexpr acs::FVec3 kColorBright{0.10f, 0.32f, 0.18f};
};

} // namespace hellogf
