// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "subsystem/Subsystem.h"

namespace acs {

class CAssetRegistry;

class AAssetSubsystem;
/** 旧公開名を正規アセットサブシステム型へ接続する互換別名。 */
using FAssetSubsystem = AAssetSubsystem;

/** CApplication が所有するアセット登録簿を Engine スコープへ公開するアダプター。 */
class AAssetSubsystem final : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(FAssetSubsystem)

    /** Application owner を検証し、既存アセット登録簿を非所有で結び付ける。 */
    bool OnOwnerAssigned() noexcept override;

    /** 終了する Engine スコープから非所有参照を外す。 */
    void OnDeinitialize() noexcept override;

    /** 結び付け済みのアセット登録簿を返し、未初期化なら nullptr を返す。 */
    CAssetRegistry* GetAssets() noexcept
    {
        return m_Assets;
    }

    /** 結び付け済みのアセット登録簿を返し、未初期化なら nullptr を返す。 */
    const CAssetRegistry* GetAssets() const noexcept
    {
        return m_Assets;
    }

private:
    /** CApplication が所有するアセット登録簿への非所有参照。 */
    CAssetRegistry* m_Assets = nullptr;
};

} // namespace acs
