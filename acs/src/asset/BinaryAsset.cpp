// SPDX-License-Identifier: Apache-2.0
// ABinaryAsset のローダ実装
#include "asset/BinaryAsset.h"

namespace acs {

TResult<TSharedPtr<AAsset>> CBinaryAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    // バイト列を所有する ABinaryAsset を生成
    TSharedPtr<ABinaryAsset> asset = MakeShared<ABinaryAsset>(bytes.Clone());
    asset->SetId(id);
    asset->SetState(EAssetState::Ready);
    // TSharedPtr<ABinaryAsset> → TSharedPtr<Asset> にアップキャスト変換（参照カウント共有）
    return TResult<TSharedPtr<AAsset>>(OkInit, TSharedPtr<AAsset>(Move(asset)));
}

} // namespace acs
