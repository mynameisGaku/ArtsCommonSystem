// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — RotatingNode 実装。
#include "RotatingNode.h"

#include "foundation/Log.h"

namespace hellogf {

void RotatingNode::OnSpawn() noexcept {
    const auto w = FWorld();
    ACS_LOG_INFO("[FNode] %s spawned at world (%.2f, %.2f)",
                 _label,
                 static_cast<double>(w.position.x),
                 static_cast<double>(w.position.y));
}

void RotatingNode::OnUpdate(acs::f32 dt) noexcept {
    Local().rotation += _speed * dt;
}

void RotatingNode::OnDespawn() noexcept {
    ACS_LOG_INFO("[FNode] %s despawn", _label);
}

} // namespace hellogf
