// SPDX-License-Identifier: Apache-2.0
// テキストアセット実装
#include "asset/TextAsset.h"

namespace acs {

TResult<TRc<Asset>> TextAssetLoader::LoadFromBytes(AssetId id, const TArray<byte>& bytes) noexcept {
    TArray<char> text;
    text.Resize(bytes.Size() + 1);
    for (usize i = 0; i < bytes.Size(); ++i) text[i] = static_cast<char>(bytes[i]);
    text[bytes.Size()] = '\0';
    TRc<TextAsset> a = MakeRc<TextAsset>(Move(text));
    a->SetId(id);
    a->SetState(EAssetState::Ready);
    return TResult<TRc<Asset>>(OkInit, TRc<Asset>(Move(a)));
}

} // namespace acs
