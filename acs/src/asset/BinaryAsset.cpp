// SPDX-License-Identifier: Apache-2.0
// FBinaryAsset のローダ実装
#include "asset/BinaryAsset.h"

namespace acs {

TResult<TRc<FAsset>> FBinaryAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    // バイト列を所有する FBinaryAsset を生成
    TRc<FBinaryAsset> asset = MakeRc<FBinaryAsset>(bytes.Clone());
    asset->SetId(id);
    asset->SetState(EAssetState::Ready);
    // TRc<FBinaryAsset> → TRc<FAsset> にアップキャスト変換（参照カウント共有）
    return TResult<TRc<FAsset>>(OkInit, TRc<FAsset>(Move(asset)));
}

} // namespace acs
