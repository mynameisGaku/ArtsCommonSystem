// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "subsystem/Subsystem.h"

namespace acs {

class FAssetRegistry;

/** FApplication が所有するアセット登録簿を Engine スコープへ公開するアダプター。 */
class FAssetSubsystem final : public FSubsystem {
public:
    ACS_SUBSYSTEM_KIND(FAssetSubsystem)

    /** Application owner を検証し、既存アセット登録簿を非所有で結び付ける。 */
    bool OnOwnerAssigned() noexcept override;

    /** 終了する Engine スコープから非所有参照を外す。 */
    void OnDeinitialize() noexcept override;

    /** 結び付け済みのアセット登録簿を返し、未初期化なら nullptr を返す。 */
    FAssetRegistry* GetAssets() noexcept
    {
        return m_Assets;
    }

    /** 結び付け済みのアセット登録簿を返し、未初期化なら nullptr を返す。 */
    const FAssetRegistry* GetAssets() const noexcept
    {
        return m_Assets;
    }

private:
    /** FApplication が所有するアセット登録簿への非所有参照。 */
    FAssetRegistry* m_Assets = nullptr;
};

} // namespace acs
