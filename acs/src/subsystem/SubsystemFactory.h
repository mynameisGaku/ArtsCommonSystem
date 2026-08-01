// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"
#include "subsystem/Subsystem.h"
#include "subsystem/SubsystemTickPhase.h"

namespace acs {

/** 1 つのサブシステムを生成する非捕捉関数。 */
using FSubsystemCreateFn = TUniquePtr<ASubsystem> (*)();

/** サブシステム型の生成条件と決定順序を表す登録値。 */
struct FSubsystemFactory {
    /** SubsystemKindOf<T>() が返す同一 link image 内種別 ID。 */
    const void* kind = nullptr;
    /** 生成する owner 寿命スコープ。 */
    ESubsystemScope scope = ESubsystemScope::World;
    /**
     * 同一order内の決定順序と診断に使うimmutableなNUL終端borrowed文字列。
     * 登録成功から対応Unregisterまで有効に保ち、自動登録値はprocess終了まで保持する。
     */
    const char* name = "";
    /** 1 体を生成し、失敗時は空を返す関数。 */
    FSubsystemCreateFn create = nullptr;
    /** 自動更新する段階。 */
    ESubsystemTickPhase phase = ESubsystemTickPhase::PreUpdate;
    /** 小さい値から初期化・更新する決定順序。 */
    i32 order = 0;
};

/** scopeが公開列挙値かを判定する。 */
inline bool IsValidSubsystemScope(ESubsystemScope Scope) noexcept
{
    return Scope == ESubsystemScope::Engine || Scope == ESubsystemScope::GameInstance ||
           Scope == ESubsystemScope::World;
}

/** 更新段階が公開列挙値かを判定する。 */
inline bool IsValidSubsystemTickPhase(ESubsystemTickPhase Phase) noexcept
{
    return Phase == ESubsystemTickPhase::None || Phase == ESubsystemTickPhase::PreUpdate ||
           Phase == ESubsystemTickPhase::PostUpdate;
}

/** 登録可能なfactory metadataが全項目揃っているかを判定する。 */
inline bool IsValidSubsystemFactory(const FSubsystemFactory& Factory) noexcept
{
    return Factory.kind != nullptr && Factory.name != nullptr && Factory.name[0] != '\0' &&
           Factory.create != nullptr && IsValidSubsystemScope(Factory.scope) &&
           IsValidSubsystemTickPhase(Factory.phase);
}

} // namespace acs
