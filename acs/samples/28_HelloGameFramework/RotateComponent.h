// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — RotateComponent。
//
// 「回転させる」振る舞いを FNode2D 継承ではなく **コンポーネント** で実装した版。
// プレーン FNode2D に `AddComponent<RotateComponent>(speed)` で取付け、毎フレーム
// Owner().Local().rotation に speed*dt を加算する。継承版 (RotatingNode) と
// 同じ動きを composition で得られることを示すための対比サンプル。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class RotateComponent : public acs::game::FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(RotateComponent)
    explicit RotateComponent(acs::f32 speed_rps) noexcept : _speed(speed_rps) {}

    void OnAttach(acs::game::FNode2D& owner) noexcept override;
    void OnUpdate(acs::f32 dt)              noexcept override;
    void OnDetach()                         noexcept override;

private:
    acs::f32 _speed;
};

} // namespace hellogf
