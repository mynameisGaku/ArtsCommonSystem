// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/IAssetLoader.h"
#include "asset/cinematics/FCinematicCodec.h"

namespace acs::asset {

/** .cine のAssetRegistry接続とCodec呼出しだけを担うローダーです。 */
class CCinematicAssetLoader final : public IAssetLoader {
public:
    /** レジストリへ公開する型を返します。 */
    AssetType TypeId() const noexcept override
    {
        return ACinematicAsset::StaticType();
    }

    /** 対応拡張子を返します。 */
    const char* Extension() const noexcept override
    {
        return "cine";
    }

    /** レジストリから渡された識別子とバイト列を検証して読み込みます。 */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

} // namespace acs::asset
