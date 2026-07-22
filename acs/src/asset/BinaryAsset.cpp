// SPDX-License-Identifier: Apache-2.0
// FBinaryAsset のローダ実装
#include "asset/BinaryAsset.h"

namespace acs {

TResult<TSharedPtr<FAsset>> FBinaryAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    // バイト列を所有する FBinaryAsset を生成
    TSharedPtr<FBinaryAsset> asset = MakeShared<FBinaryAsset>(bytes.Clone());
    asset->SetId(id);
    asset->SetState(EAssetState::Ready);
    // TSharedPtr<FBinaryAsset> → TSharedPtr<Asset> にアップキャスト変換（参照カウント共有）
    return TResult<TSharedPtr<FAsset>>(OkInit, TSharedPtr<FAsset>(Move(asset)));
}

} // namespace acs
