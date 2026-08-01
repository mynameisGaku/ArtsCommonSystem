// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** サブシステム owner の責務種別。 */
enum class ESubsystemOwnerKind : u8 {
    /** 旧 API または種別を保証できない owner。 */
    Unknown = 0,
    /** アプリケーション寿命を所有する CApplication。 */
    Application = 1,
    /** ゲームセッション寿命を所有する CGame。 */
    Game = 2,
    /** ワールド寿命を所有する AScene。 */
    Scene = 3,
};

/** サブシステムへ渡す非所有 owner とその責務種別。 */
struct FSubsystemOwner {
    /** owner の生ポインタ。サブシステムは所有しない。 */
    void* pointer = nullptr;
    /** pointer が満たす責務種別。 */
    ESubsystemOwnerKind kind = ESubsystemOwnerKind::Unknown;
};

} // namespace acs
