// SPDX-License-Identifier: Apache-2.0
// アセットローダのインターフェイス
//
// 各アセット型 (Texture, Mesh, Audio など) ごとにローダを実装し、
// AssetRegistry に登録する。レジストリは拡張子から適切なローダを呼び出す。
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Rc.h"
#include "container/Array.h"
#include "container/StringView.h"
#include "asset/Asset.h"

namespace acs {

class IAssetLoader {
public:
    virtual ~IAssetLoader() noexcept = default;

    // 担当するアセット型 ID
    virtual AssetType TypeId() const noexcept = 0;

    // 担当する拡張子（"dds", "png", "mesh" など）— レジストリがマッチング
    virtual const char* Extension() const noexcept = 0;

    // バイト列からアセットを生成（同期）
    // 非同期化は AssetRegistry 側が ThreadPool で本関数を呼ぶ形で実現する
    virtual TResult<TRc<Asset>> LoadFromBytes(AssetId id, const TArray<byte>& bytes) noexcept = 0;
};

} // namespace acs
