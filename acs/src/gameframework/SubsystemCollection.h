// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/Subsystem.h"
#include "gameframework/SubsystemRegistry.h"
#include "subsystem/SubsystemCollection.h"

namespace acs::game {

/** 旧 GameFramework コレクション名を正規型へ転送する。 */
using ::acs::CSubsystemCollection;
/** 旧集合名を GameFramework 名前空間でも利用できるようにする。 */
using ::acs::FSubsystemCollection;

} // namespace acs::game
