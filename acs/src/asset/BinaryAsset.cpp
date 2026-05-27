// SPDX-License-Identifier: Apache-2.0
// FBinaryAsset のローダ実装
#include "asset/BinaryAsset.h"

namespace acs {

TResult<TRc<Asset>> FBinaryAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    // バイト列を所有する FBinaryAsset を生成
    TRc<FBinaryAsset> asset = MakeRc<FBinaryAsset>(bytes.Clone());
    asset->SetId(id);
    asset->SetState(EAssetState::Ready);
    // TRc<FBinaryAsset> → TRc<Asset> にアップキャスト変換（参照カウント共有）
    return TResult<TRc<Asset>>(OkInit, TRc<Asset>(Move(asset)));
}

} // namespace acs
