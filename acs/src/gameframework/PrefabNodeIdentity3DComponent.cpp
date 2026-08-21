// SPDX-License-Identifier: Apache-2.0
#include "gameframework/PrefabNodeIdentity3DComponent.h"
#include "gameframework/Scene3DSerialize.h"

namespace acs::game {

FStringView APrefabNodeIdentity3DComponent::SourceNodeId() const noexcept
{
    return m_SourceNodeId.View();
}

bool APrefabNodeIdentity3DComponent::TrySetSourceNodeId(FStringView source_node_id) noexcept
{
    if (source_node_id.Size() != kScene3DSerializePrefabSourceNodeIdBytes) return false;
    for (u32 index = 0u; index < source_node_id.Size(); ++index) {
        const char value = source_node_id[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
    }
    m_SourceNodeId = FString(source_node_id);
    return true;
}

} // namespace acs::game
