// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — RotatingNode。
//
// Node2D を **継承** して「毎フレーム rotation を加算する」振る舞いを持たせた
// シンプルなサブクラス。OnSpawn / OnDespawn でログを出すので、Node2D 自体の
// lifecycle (AddChild / Destroy + ResolveStructuralChanges) を観察しやすい。
// composition 版 (RotateComponent) との対比に使う。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class RotatingNode : public acs::game::Node2D {
public:
    explicit RotatingNode(acs::f32 speed_rps, const char* label) noexcept
        : _speed(speed_rps), _label(label) {}

    void OnSpawn()             noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnDespawn()           noexcept override;

private:
    acs::f32    _speed;
    const char* _label;
};

} // namespace hellogf
