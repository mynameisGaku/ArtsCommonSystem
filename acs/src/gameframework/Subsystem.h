// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "subsystem/Subsystem.h"

namespace acs::game {

/** 旧 GameFramework スコープ名を正規型へ転送する。 */
using ::acs::ESubsystemScope;
/** 旧 GameFramework owner 種別名を正規型へ転送する。 */
using ::acs::ESubsystemOwnerKind;
/** 旧 GameFramework owner descriptor 名を正規型へ転送する。 */
using ::acs::FSubsystemOwner;
/** 旧 GameFramework 更新段階名を正規型へ転送する。 */
using ::acs::ESubsystemTickPhase;
/** 旧 GameFramework 更新 context 名を正規型へ転送する。 */
using ::acs::FSubsystemFrameContext;
/** 旧 GameFramework 基底名を正規型へ転送する。 */
using ::acs::FSubsystem;
/** 旧 GameFramework 種別 ID 関数を正規関数へ転送する。 */
using ::acs::SubsystemKindOf;

} // namespace acs::game
