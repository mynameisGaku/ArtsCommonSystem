// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/Subsystem.h"
#include "subsystem/SubsystemRegistry.h"

namespace acs::game {

/** 旧 GameFramework 生成関数名を正規 alias へ転送する。 */
using ::acs::FSubsystemCreateFn;
/** 旧 GameFramework factory 名を正規型へ転送する。 */
using ::acs::FSubsystemFactory;
/** 旧 GameFramework 登録簿名を正規型へ転送する。 */
using ::acs::FSubsystemRegistry;
/** 旧 GameFramework 自動登録補助名を正規型へ転送する。 */
using ::acs::FSubsystemAutoRegister;

} // namespace acs::game
