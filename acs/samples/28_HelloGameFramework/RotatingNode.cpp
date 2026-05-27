// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — RotatingNode 実装。
#include "RotatingNode.h"

#include "foundation/Log.h"

namespace hellogf {

void RotatingNode::OnSpawn() noexcept {
    const auto w = World();
    ACS_LOG_INFO("[Node] %s spawned at world (%.2f, %.2f)",
                 m_Label,
                 static_cast<double>(w.position.x),
                 static_cast<double>(w.position.y));
}

void RotatingNode::OnUpdate(acs::f32 dt) noexcept {
    Local().rotation += m_Speed * dt;
}

void RotatingNode::OnDespawn() noexcept {
    ACS_LOG_INFO("[Node] %s despawn", m_Label);
}

} // namespace hellogf
