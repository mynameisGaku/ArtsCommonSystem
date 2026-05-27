// SPDX-License-Identifier: Apache-2.0
// テキストアセット（UTF-8 文字列を保持）
//
// 対応拡張子: txt / json / xml / yaml / yml / toml / ini / csv / md / log / hlsl /
//          glsl / vert / frag / lua / py（任意のテキストファイル）
#pragma once

#include "container/Array.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "container/Hash.h"

namespace acs {

class FTextAsset : public Asset {
public:
    ACS_ASSET_TYPE("FTextAsset")

    FTextAsset() noexcept = default;
    explicit FTextAsset(TArray<char>&& text) noexcept : _text(Move(text)) {}

    // NUL 終端された文字列ポインタ
    const char* CStr() const noexcept { return _text.IsEmpty() ? "" : _text.Data(); }
    usize       Size() const noexcept { return _text.IsEmpty() ? 0 : _text.Size() - 1; }
    const TArray<char>& Raw() const noexcept { return _text; }

private:
    TArray<char> _text;  // 末尾に NUL を含む
};

class FTextAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return FTextAsset::StaticType(); }
    const char* Extension() const noexcept override { return "txt"; }   // 主要拡張子
    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

} // namespace acs
