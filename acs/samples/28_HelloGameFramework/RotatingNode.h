// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — RotatingNode。
//
// FNode2D を **継承** して「毎フレーム rotation を加算する」振る舞いを持たせた
// シンプルなサブクラス。OnSpawn / OnDespawn でログを出すので、FNode2D 自体の
// lifecycle (AddChild / Destroy + ResolveStructuralChanges) を観察しやすい。
// composition 版 (RotateComponent) との対比に使う。
#pragma once

#include "gameframework/GameFramework.h"

namespace hellogf {

class RotatingNode : public acs::game::FNode2D {
public:
    explicit RotatingNode(acs::f32 speed_rps, const char* label) noexcept
        : m_Speed(speed_rps), m_Label(label) {}

    void OnSpawn()             noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnDespawn()           noexcept override;

private:
    acs::f32    m_Speed;
    const char* m_Label;
};

} // namespace hellogf
