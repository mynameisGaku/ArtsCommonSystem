// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — ARotateComponent 実装。
#include "RotateComponent.h"

#include "foundation/Log.h"

namespace hellogf {

void ARotateComponent::OnAttach(acs::game::ANode& /*owner*/) noexcept {
    ACS_LOG_INFO("[Component] ARotateComponent attached (speed=%.2f rad/s)",
                 static_cast<double>(m_Speed));
}

void ARotateComponent::OnUpdate(acs::f32 dt) noexcept {
    Owner().SetRotation2D(Owner().Rotation2D() + m_Speed * dt);
}

void ARotateComponent::OnDetach() noexcept {
    ACS_LOG_INFO("[Component] ARotateComponent detached");
}

} // namespace hellogf
