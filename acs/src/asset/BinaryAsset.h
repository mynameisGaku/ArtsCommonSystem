// SPDX-License-Identifier: Apache-2.0
// 汎用バイナリアセット（バイト列をそのまま保持する最小実装）
//
// 専用ローダがない拡張子のフォールバック用、またはテストデータ用に使う。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "container/Hash.h"

namespace acs {

class FBinaryAsset : public Asset {
public:
    ACS_ASSET_TYPE("FBinaryAsset")

    FBinaryAsset() noexcept = default;
    explicit FBinaryAsset(TArray<byte>&& bytes) noexcept : m_Bytes(Move(bytes)) {}

    const TArray<byte>& Bytes() const noexcept { return m_Bytes; }
    TArray<byte>&       Bytes()       noexcept { return m_Bytes; }

private:
    TArray<byte> m_Bytes;
};

// 拡張子フォールバックローダ（任意の拡張子を担当する）
class FBinaryAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return FBinaryAsset::StaticType(); }
    const char* Extension() const noexcept override { return "*"; }  // 全拡張子のフォールバック

    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

} // namespace acs
