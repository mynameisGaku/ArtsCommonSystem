// SPDX-License-Identifier: Apache-2.0
// テキストアセット実装
#include "asset/TextAsset.h"

namespace acs {

/** バイト列を NUL 終端の char バッファへコピーし、Ready 状態の ATextAsset を生成して返す。 */
TResult<TSharedPtr<AAsset>> CTextAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    TArray<char> text;
    text.SetNum(bytes.Num() + 1);
    for (usize i = 0; i < bytes.Num(); ++i) text[i] = static_cast<char>(bytes[i]);
    text[bytes.Num()] = '\0';
    TSharedPtr<ATextAsset> a = MakeShared<ATextAsset>(Move(text));
    a->SetId(id);
    a->SetState(EAssetState::Ready);
    return TResult<TSharedPtr<AAsset>>(OkInit, TSharedPtr<AAsset>(Move(a)));
}

} // namespace acs
