// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — ARotatingNode 実装。
#include "RotatingNode.h"

#include "foundation/Log.h"

namespace hellogf {

void ARotatingNode::OnSpawn() noexcept {
    const auto w = World2D();
    ACS_LOG_INFO("[Node] %s spawned at world (%.2f, %.2f)",
                 m_Label,
                 static_cast<double>(w.position.x),
                 static_cast<double>(w.position.y));
}

void ARotatingNode::OnUpdate(acs::f32 dt) noexcept {
    SetRotation2D(Rotation2D() + m_Speed * dt);
}

void ARotatingNode::OnDespawn() noexcept {
    ACS_LOG_INFO("[Node] %s despawn", m_Label);
}

} // namespace hellogf
