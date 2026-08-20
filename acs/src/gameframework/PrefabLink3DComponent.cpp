// SPDX-License-Identifier: Apache-2.0
#include "gameframework/PrefabLink3DComponent.h"

namespace acs::game {

FStringView APrefabLink3DComponent::SourcePath() const noexcept
{
    return m_SourcePath.View();
}

void APrefabLink3DComponent::SetSourcePath(FStringView path) noexcept
{
    m_SourcePath = FString(path);
}

FStringView APrefabLink3DComponent::InstanceId() const noexcept
{
    return m_InstanceId.View();
}

void APrefabLink3DComponent::SetInstanceId(FStringView instance_id) noexcept
{
    m_InstanceId = FString(instance_id);
}

} // namespace acs::game
