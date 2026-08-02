// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Spawn2DSubsystem.h"

/** Spawn2DSubsystem.hだけで正規node戻り値を使えることを固定する。 */
acs::ANode* SpawnFromHeader(
    acs::ASpawn2DSubsystem& Spawner, const char* Text, acs::FVec2 Position) noexcept
{
    return Spawner.SpawnPrefabText(Text, Position);
}

/** Spawn2DSubsystem.h 単独で旧2Dシーン前方宣言を利用できることを固定する。 */
void AcceptLegacyScene2DForward(acs::game::FScene2D*) noexcept;
