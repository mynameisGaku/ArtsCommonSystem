// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — ARotateComponent。
//
// 「回転させる」振る舞いを ANode 継承ではなく **コンポーネント** で実装した版。
// プレーン ANode に `AddComponent<ARotateComponent>(speed)` で取付け、毎フレーム
// Owner().Rotation2D() に speed*dt を加算する。継承版 (ARotatingNode) と
// 同じ動きを composition で得られることを示すための対比サンプル。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class ARotateComponent : public acs::game::AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ARotateComponent)
    explicit ARotateComponent(acs::f32 speed_rps) noexcept : m_Speed(speed_rps) {}

    void OnAttach(acs::game::ANode& owner) noexcept override;
    void OnUpdate(acs::f32 dt)              noexcept override;
    void OnDetach()                         noexcept override;

private:
    acs::f32 m_Speed;
};

} // namespace hellogf
