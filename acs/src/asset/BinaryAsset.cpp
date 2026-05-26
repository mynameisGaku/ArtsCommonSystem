// SPDX-License-Identifier: Apache-2.0
// BinaryAsset のローダ実装
#include "asset/BinaryAsset.h"

namespace acs {

TResult<TRc<Asset>> BinaryAssetLoader::LoadFromBytes(AssetId id, const TArray<byte>& bytes) noexcept {
    // バイト列を所有する BinaryAsset を生成
    TRc<BinaryAsset> asset = MakeRc<BinaryAsset>(bytes.Clone());
    asset->SetId(id);
    asset->SetState(EAssetState::Ready);
    // TRc<BinaryAsset> → TRc<Asset> にアップキャスト変換（参照カウント共有）
    return TResult<TRc<Asset>>(OkInit, TRc<Asset>(Move(asset)));
}

} // namespace acs
