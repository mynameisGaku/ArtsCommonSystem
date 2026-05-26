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

class BinaryAsset : public Asset {
public:
    ACS_ASSET_TYPE("BinaryAsset")

    BinaryAsset() noexcept = default;
    explicit BinaryAsset(TArray<byte>&& bytes) noexcept : _bytes(Move(bytes)) {}

    const TArray<byte>& Bytes() const noexcept { return _bytes; }
    TArray<byte>&       Bytes()       noexcept { return _bytes; }

private:
    TArray<byte> _bytes;
};

// 拡張子フォールバックローダ（任意の拡張子を担当する）
class BinaryAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return BinaryAsset::StaticType(); }
    const char* Extension() const noexcept override { return "*"; }  // 全拡張子のフォールバック

    TResult<TRc<Asset>> LoadFromBytes(AssetId id, const TArray<byte>& bytes) noexcept override;
};

} // namespace acs
