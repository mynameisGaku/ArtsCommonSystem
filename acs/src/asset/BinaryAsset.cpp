// BinaryAsset のローダ実装
#include "asset/BinaryAsset.h"

namespace acs {

Result<Rc<Asset>> BinaryAssetLoader::LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept {
    // バイト列を所有する BinaryAsset を生成
    Rc<BinaryAsset> asset = MakeRc<BinaryAsset>(bytes.Clone());
    asset->SetId(id);
    asset->SetState(AssetState::Ready);
    // Rc<BinaryAsset> → Rc<Asset> にアップキャスト変換（参照カウント共有）
    return Result<Rc<Asset>>(OkInit, Rc<Asset>(Move(asset)));
}

} // namespace acs
