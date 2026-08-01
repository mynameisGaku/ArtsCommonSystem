// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Spawn2DSubsystem.h"

/** Spawn2DSubsystem.hだけで正規node戻り値を使えることを固定する。 */
acs::ANode* SpawnFromHeader(
    acs::FSpawn2DSubsystem& Spawner, const char* Text, acs::FVec2 Position) noexcept
{
    return Spawner.SpawnPrefabText(Text, Position);
}
