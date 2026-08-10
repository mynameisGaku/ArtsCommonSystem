// SPDX-License-Identifier: Apache-2.0
#include "asset/cinematics/CCinematicAssetLoader.h"

namespace acs::asset {

TResult<TSharedPtr<AAsset>> CCinematicAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept
{
    // Codecの検証結果を受け取り、失敗時は公開しません。
    TResult<TSharedPtr<ACinematicAsset>> decoded = FCinematicCodec::Decode(bytes);
    if (decoded.IsErr()) return Err<TSharedPtr<AAsset>>(decoded.Error());
    // loaderから渡された識別子とReady状態を成功したassetへ設定します。
    TSharedPtr<ACinematicAsset> asset = Move(decoded.Value());
    if (!asset) return ACS_ERR(Asset, 907, "CCinematicAssetLoader: null asset");
    asset->SetId(id);
    asset->SetState(EAssetState::Ready);
    return TResult<TSharedPtr<AAsset>>(OkInit, TSharedPtr<AAsset>(Move(asset)));
}

} // namespace acs::asset
