// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Spawn2DSubsystem.h"

#include "gameframework/ANode.h"
#include "gameframework/SceneTextLoader.h"

namespace acs::game {

bool ASpawn2DSubsystem::OnOwnerAssigned() noexcept
{
    m_TargetRoot = nullptr;
    if (OwnerKind() == ESubsystemOwnerKind::Unknown) return true;
    return OwnerKind() == ESubsystemOwnerKind::Scene && Owner() != nullptr;
}

void ASpawn2DSubsystem::OnDeinitialize() noexcept
{
    m_TargetRoot = nullptr;
}

ANode* ASpawn2DSubsystem::SpawnPrefabText(const char* Text, FVec2 Position) noexcept
{
    if (m_TargetRoot == nullptr || Text == nullptr) return nullptr;
    /** 生成したプレハブのルートノード。 */
    ANode* Node = ::acs::game::SpawnPrefabText(Text, *m_TargetRoot);
    if (Node != nullptr) Node->SetPosition2D(Position);
    return Node;
}

ANode* ASpawn2DSubsystem::SpawnPrefabFile(const char* Path, FVec2 Position) noexcept
{
    if (m_TargetRoot == nullptr || Path == nullptr) return nullptr;
    /** 生成したプレハブのルートノード。 */
    ANode* Node = ::acs::game::SpawnPrefabFile(Path, *m_TargetRoot);
    if (Node != nullptr) Node->SetPosition2D(Position);
    return Node;
}

} // namespace acs::game
