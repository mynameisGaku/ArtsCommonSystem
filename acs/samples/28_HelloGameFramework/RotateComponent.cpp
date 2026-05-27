// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — RotateComponent 実装。
#include "RotateComponent.h"

#include "foundation/Log.h"

namespace hellogf {

void RotateComponent::OnAttach(acs::game::FNode2D& /*owner*/) noexcept {
    ACS_LOG_INFO("[Component] RotateComponent attached (speed=%.2f rad/s)",
                 static_cast<double>(_speed));
}

void RotateComponent::OnUpdate(acs::f32 dt) noexcept {
    Owner().Local().rotation += _speed * dt;
}

void RotateComponent::OnDetach() noexcept {
    ACS_LOG_INFO("[Component] RotateComponent detached");
}

} // namespace hellogf
