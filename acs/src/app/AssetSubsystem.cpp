// SPDX-License-Identifier: Apache-2.0
#include "app/AssetSubsystem.h"

#include "app/Application.h"
#include "asset/AssetRegistry.h"

namespace acs {

/** Application owner を検証し、既存アセット登録簿を非所有で結び付ける。 */
bool FAssetSubsystem::OnOwnerAssigned() noexcept
{
    m_Assets = nullptr;
    if (OwnerKind() == ESubsystemOwnerKind::Unknown) return true;
    if (OwnerKind() != ESubsystemOwnerKind::Application || Owner() == nullptr) {
        return false;
    }

    // 検証済みの Application owner。
    FApplication* const application = static_cast<FApplication*>(Owner());
    m_Assets = &application->GetAssets();
    return true;
}

/** 終了する Engine スコープから非所有参照を外す。 */
void FAssetSubsystem::OnDeinitialize() noexcept
{
    m_Assets = nullptr;
}

} // namespace acs
